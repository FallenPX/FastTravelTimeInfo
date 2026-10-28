#include "pch.h"

#include "MapMenuHook.h"
#include "Estimator.h"
#include "Settings.h"

// How it works
// ------------
// When the cursor rests on a world-map marker, the vanilla map movie (map.swf) shows a
// small flyout: MarkerDescriptionHolder.Description, an instance of the ActionScript class
// Map.MarkerDescription. The game fills it by calling
//     Description.SetDescription(title:String, lineItems:Array of {Item, Value})
// which draws the title (the location name) and one "Item: Value" row per entry.
//
// After the game has advanced the map movie each frame, this plugin checks which marker is
// selected (MapMenu::selectedMarker), works out the travel time and arrival time for it, and
// calls SetDescription again with the game's own rows plus two of ours. It only rewrites the
// flyout when our rows are missing, so it costs almost nothing while the cursor is still.

namespace FTI::MapMenuHook
{
	namespace
	{
		struct Line
		{
			std::string item;
			std::string value;

			bool operator==(const Line&) const = default;
		};

		struct HoverState
		{
			std::int32_t                selected{ -1 };
			std::string                 title;
			std::vector<Line>           lines;    // our rows for the current marker
			std::optional<RE::NiPoint3> destPos;  // current marker position (for calibration)
			int                         failedApplies{ 0 };
			bool                        tripArmed{ false };
			bool                        warnedNoDescription{ false };
		};

		std::recursive_mutex g_stateLock;  // guards g_state (AdvanceMovie and menu events)
		HoverState        g_state;
		std::atomic<bool> g_resetRequested{ true };

		constexpr int MAX_FAILED_APPLIES = 3;

		// ---- Scaleform helpers (every access is type-checked: GFxValue asserts are
		// compiled out in release builds, so an unchecked call on a non-object would crash).

		bool GetMember(const RE::GFxValue& a_obj, const char* a_name, RE::GFxValue& a_out)
		{
			return a_obj.IsObject() && a_obj.GetMember(a_name, &a_out);
		}

		std::string ReadText(const RE::GFxValue& a_textField)
		{
			RE::GFxValue text;
			if (GetMember(a_textField, "text", text) && text.IsString() && text.GetString()) {
				return text.GetString();
			}
			return {};
		}

		bool IsMarkerDescription(const RE::GFxValue& a_value)
		{
			RE::GFxValue fn;
			return GetMember(a_value, "SetDescription", fn) && fn.IsObject();
		}

		bool FindMarkerDescription(RE::MapMenu* a_menu, const RE::MapMenu::RUNTIME_DATA2& a_data, RE::GFxValue& a_out)
		{
			// 1) through the game's own handle to the map movie object
			const auto& mapMovie = a_data.mapMovie;
			if (GetMember(mapMovie, "MarkerDescriptionObj", a_out) && IsMarkerDescription(a_out)) {
				return true;
			}
			RE::GFxValue holder;
			if (GetMember(mapMovie, "MarkerDescriptionHolder", holder) &&
				GetMember(holder, "Description", a_out) && IsMarkerDescription(a_out)) {
				return true;
			}
			// 2) vanilla layout: the holder is attached to the root of map.swf
			if (a_menu->uiMovie &&
				a_menu->uiMovie->GetVariable(&a_out, "_root.MarkerDescriptionHolder.Description") &&
				IsMarkerDescription(a_out)) {
				return true;
			}
			return false;
		}

		std::vector<Line> ReadVisibleLines(const RE::GFxValue& a_description)
		{
			std::vector<Line> out;
			RE::GFxValue      list;
			if (!GetMember(a_description, "DescriptionList", list) || !list.IsArray()) {
				return out;
			}
			const auto size = list.GetArraySize();
			for (std::uint32_t i = 0; i < size; ++i) {
				RE::GFxValue entry;
				if (!list.GetElement(i, &entry)) {
					continue;
				}
				RE::GFxValue visible;
				if (!GetMember(entry, "_visible", visible) || !visible.IsBool() || !visible.GetBool()) {
					continue;
				}
				RE::GFxValue itemField;
				RE::GFxValue valueField;
				GetMember(entry, "Item", itemField);
				GetMember(entry, "Value", valueField);

				auto item = ReadText(itemField);
				if (item.ends_with(": ")) {  // MarkerDescription appends ": " to non-empty items
					item.resize(item.size() - 2);
				}
				out.push_back({ std::move(item), ReadText(valueField) });
			}
			return out;
		}

		bool ApplyLines(RE::GFxMovieView* a_movie, RE::GFxValue& a_description, const std::string& a_title, const std::vector<Line>& a_lines)
		{
			std::array<RE::GFxValue, 2> args;
			a_movie->CreateString(&args[0], a_title.c_str());
			a_movie->CreateArray(&args[1]);
			if (!args[1].IsArray()) {
				return false;
			}
			for (const auto& line : a_lines) {
				RE::GFxValue entry;
				a_movie->CreateObject(&entry);
				if (!entry.IsObject()) {
					return false;
				}
				RE::GFxValue item;
				RE::GFxValue value;
				a_movie->CreateString(&item, line.item.c_str());
				a_movie->CreateString(&value, line.value.c_str());
				entry.SetMember("Item", item);
				entry.SetMember("Value", value);
				args[1].PushBack(entry);
			}
			return a_description.IsObject() && a_description.Invoke("SetDescription", nullptr, args.data(), args.size());
		}

		bool IsOurLabel(const std::string& a_item)
		{
			const auto& s = Settings::Get();
			return a_item == s.travelLabel || a_item == s.arrivalLabel || (s.showDistance && a_item == s.distanceLabel);
		}

		// ---- Game-side helpers

		std::string_view MarkerName(const RE::MapMenuMarker& a_marker)
		{
			if (a_marker.fullName) {
				if (const auto name = a_marker.fullName->GetFullName()) {
					return name;
				}
			}
			return {};
		}

		// The flyout index and the engine's marker array normally line up one-to-one, but the
		// marker's name is cross-checked against the flyout title so a mismatch can never
		// produce an estimate for the wrong place.
		const RE::MapMenuMarker* ResolveMarker(const RE::BSTArray<RE::MapMenuMarker>& a_markers, std::int32_t a_selected, const std::string& a_title)
		{
			if (a_selected < 0) {
				return nullptr;
			}
			const auto matches = [&](const RE::MapMenuMarker& a_marker) {
				return a_title.empty() || MarkerName(a_marker) == a_title;
			};

			if (static_cast<std::uint32_t>(a_selected) < a_markers.size()) {
				const auto& candidate = a_markers[static_cast<std::uint32_t>(a_selected)];
				if (matches(candidate)) {
					return &candidate;
				}
			}
			for (const auto& marker : a_markers) {
				if (marker.index == a_selected && matches(marker)) {
					return &marker;
				}
			}
			if (a_title.empty()) {
				return nullptr;
			}
			const RE::MapMenuMarker* found = nullptr;
			for (const auto& marker : a_markers) {
				if (MarkerName(marker) == a_title) {
					if (found) {
						return nullptr;  // ambiguous name: show nothing rather than guess
					}
					found = &marker;
				}
			}
			return found;
		}

		std::optional<RE::NiPoint3> PlayerOrigin(const RE::MapMenu::RUNTIME_DATA2& a_data)
		{
			if (auto player = RE::PlayerCharacter::GetSingleton()) {
				if (auto cell = player->GetParentCell(); cell && !cell->IsInteriorCell()) {
					return player->GetPosition();
				}
			}
			// Interior (only possible with mods that allow fast travel indoors):
			// use the position the map draws the player marker at.
			const auto& pos = a_data.playerMarkerPosition;
			if (pos.x != 0.0f || pos.y != 0.0f) {
				return pos;
			}
			return std::nullopt;
		}

		void ComputeLines(const RE::MapMenu::RUNTIME_DATA2& a_data, std::int32_t a_selected, const std::string& a_title)
		{
			const auto& settings = Settings::Get();

			const auto* marker = ResolveMarker(a_data.mapMarkers, a_selected, a_title);
			if (!marker) {
				return;
			}
			const auto ref = RE::TESObjectREFR::LookupByHandle(marker->ref);
			if (!ref) {
				return;
			}
			// Only real location markers (not quest targets, doors or the player's own marker).
			const auto extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
			if (!extra || !extra->mapData) {
				return;
			}
			if (settings.onlyFastTravelMarkers && !extra->mapData->flags.all(RE::MapMarkerData::Flag::kCanTravelTo)) {
				return;
			}
			const auto origin = PlayerOrigin(a_data);
			if (!origin) {
				return;
			}
			const auto destination = ref->GetPosition();
			const auto estimate = Estimator::Get().Estimate(*origin, destination);
			if (!estimate) {
				return;
			}

			g_state.destPos = destination;
			g_state.lines.push_back({ settings.travelLabel, FormatDuration(estimate->hours, !estimate->calibrated) });
			g_state.lines.push_back({ settings.arrivalLabel, FormatArrival(estimate->hours) });
			if (settings.showDistance) {
				g_state.lines.push_back({ settings.distanceLabel, FormatDistance(estimate->distance) });
			}
		}

		// Remember the trip for calibration once the player has said "yes" to fast travel.
		// Caller holds g_stateLock.
		void TryArmTrip(const RE::MapMenu::RUNTIME_DATA2& a_data)
		{
			if (!a_data.fastTravelConfirmed || g_state.tripArmed) {
				return;
			}
			g_state.tripArmed = true;
			if (g_state.destPos) {
				if (const auto origin = PlayerOrigin(a_data)) {
					Estimator::Get().ArmTrip(*origin, *g_state.destPos);
				}
			}
		}

		void Update(RE::MapMenu* a_menu)
		{
			if (!Settings::Get().enabled || REL::Module::IsVR()) {
				return;
			}
			auto* data = a_menu->GetRuntimeData2();
			auto* movie = a_menu->uiMovie.get();
			if (!data || !movie) {
				return;
			}

			std::lock_guard guard{ g_stateLock };

			if (g_resetRequested.exchange(false)) {
				const bool warned = g_state.warnedNoDescription;
				g_state = HoverState{};
				g_state.warnedNoDescription = warned;
			}

			TryArmTrip(*data);

			const auto selected = data->selectedMarker;
			if (selected < 0) {
				g_state.selected = -1;
				g_state.title.clear();
				g_state.lines.clear();
				return;
			}

			RE::GFxValue description;
			if (!FindMarkerDescription(a_menu, *data, description)) {
				if (!g_state.warnedNoDescription) {
					g_state.warnedNoDescription = true;
					logs::warn("Map marker flyout not found; another mod may have replaced map.swf");
				}
				return;
			}

			RE::GFxValue titleField;
			std::string  title;
			if (GetMember(description, "Title", titleField)) {
				title = ReadText(titleField);
			}
			if (title.empty()) {
				return;  // the game has not filled the flyout yet
			}

			if (selected != g_state.selected || title != g_state.title) {
				g_state.selected = selected;
				g_state.title = title;
				g_state.lines.clear();
				g_state.destPos.reset();
				g_state.failedApplies = 0;
				ComputeLines(*data, selected, title);
			}

			// Compare what the flyout shows with what it should show.
			const auto        visible = ReadVisibleLines(description);
			std::vector<Line> foreign;
			for (const auto& line : visible) {
				if (!IsOurLabel(line.item)) {
					foreign.push_back(line);
				}
			}
			auto wanted = foreign;
			wanted.insert(wanted.end(), g_state.lines.begin(), g_state.lines.end());

			if (visible == wanted) {
				return;
			}
			if (g_state.failedApplies >= MAX_FAILED_APPLIES) {
				return;  // the flyout does not take our rows; stop retrying for this marker
			}
			// Re-check straight away: if the game later overwrites the flyout we simply
			// re-apply next frame, but if SetDescription itself does not produce our rows
			// (a replaced map.swf, say) we give up after a few tries instead of looping.
			if (ApplyLines(movie, description, title, wanted) && ReadVisibleLines(description) == wanted) {
				g_state.failedApplies = 0;
			} else if (++g_state.failedApplies == MAX_FAILED_APPLIES) {
				logs::warn("Could not add rows to the flyout for \"{}\"", title);
			}
		}

		struct AdvanceMovieHook
		{
			static void thunk(RE::MapMenu* a_this, float a_interval, std::uint32_t a_currentTime)
			{
				func(a_this, a_interval, a_currentTime);
				try {
					Update(a_this);
				} catch (const std::exception& e) {
					logs::error("Update failed: {}", e.what());
				} catch (...) {
					logs::error("Update failed: unknown exception");
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		class EventHandler final :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
			public RE::BSTEventSink<RE::TESFastTravelEndEvent>
		{
		public:
			static EventHandler* GetSingleton()
			{
				static EventHandler singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || a_event->menuName != RE::MapMenu::MENU_NAME) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->opening) {
					g_resetRequested = true;
				} else if (auto ui = RE::UI::GetSingleton()) {
					// Fallback in case the map closed before another AdvanceMovie ran.
					if (auto menu = ui->GetMenu<RE::MapMenu>()) {
						if (auto* data = menu->GetRuntimeData2()) {
							std::lock_guard guard{ g_stateLock };
							TryArmTrip(*data);
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent* a_event, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override
			{
				if (a_event) {
					Estimator::Get().OnFastTravelEnd(a_event->fastTravelEndHours);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		if (REL::Module::IsVR()) {
			return;
		}

		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_MapMenu[0] };
		AdvanceMovieHook::func = vtable.write_vfunc(0x5, AdvanceMovieHook::thunk);
		logs::info("Hooked MapMenu::AdvanceMovie");

		auto handler = EventHandler::GetSingleton();
		if (auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(handler);
		}
		if (auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			if (auto source = holder->GetEventSource<RE::TESFastTravelEndEvent>()) {
				source->AddEventSink(handler);
				logs::info("Listening for fast-travel end events (auto-calibration)");
			}
		}
	}
}
