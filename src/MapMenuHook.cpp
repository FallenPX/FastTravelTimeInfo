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
			std::vector<std::string>    staleItems;  // labels used earlier for this marker
			std::uint32_t               settingsVersion{ 0 };
			int                         failedApplies{ 0 };
			bool                        tripArmed{ false };
			bool                        warnedNoDescription{ false };
			bool                        warnedNoBackground{ false };

			struct BackgroundShape
			{
				double x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
				int    opacity{ 0 };

				bool operator==(const BackgroundShape&) const = default;
			};
			std::optional<BackgroundShape> background;  // what is currently drawn, if anything
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

		bool GetNumber(const RE::GFxValue& a_obj, const char* a_name, double& a_out)
		{
			RE::GFxValue value;
			if (!GetMember(a_obj, a_name, value) || !value.IsNumber()) {
				return false;
			}
			a_out = value.GetNumber();
			return std::isfinite(a_out);
		}

		// Re-stacks the flyout's title and visible rows with a custom line spacing.
		// Mirrors the vanilla Map.MarkerDescription.SetDescription layout: each element is
		// placed below the previous one by its own height (here scaled by a_factor), and the
		// block is centred vertically on the marker. Only writes positions that differ, so
		// calling it every frame is cheap.
		void ApplySpacing(const RE::GFxValue& a_description, double a_factor)
		{
			struct Element
			{
				RE::GFxValue clip;
				double       height;
			};
			std::vector<Element> elements;

			RE::GFxValue title;
			double       height = 0.0;
			if (GetMember(a_description, "Title", title) && !ReadText(title).empty() && GetNumber(title, "_height", height)) {
				elements.push_back({ title, height });
			}
			RE::GFxValue list;
			if (GetMember(a_description, "DescriptionList", list) && list.IsArray()) {
				const auto size = list.GetArraySize();
				for (std::uint32_t i = 0; i < size; ++i) {
					RE::GFxValue entry;
					RE::GFxValue visible;
					if (list.GetElement(i, &entry) && GetMember(entry, "_visible", visible) && visible.IsBool() &&
						visible.GetBool() && GetNumber(entry, "_height", height)) {
						elements.push_back({ entry, height });
					}
				}
			}
			if (elements.size() < 2) {
				return;  // nothing to space out
			}

			double total = 0.0;
			for (const auto& element : elements) {
				total += element.height * a_factor;
			}
			double y = -total / 2.0;
			for (auto& element : elements) {
				double current = 0.0;
				if (!GetNumber(element.clip, "_y", current) || std::abs(current - y) > 0.25) {
					element.clip.SetMember("_y", RE::GFxValue(y));
				}
				y += element.height * a_factor;
			}
		}

		// ---- Optional translucent background behind the flyout text
		//
		// An empty movie clip is created inside the flyout at depth -16384, below everything the
		// Flash file itself places (SkyUI uses the same trick for its widget layer), and a filled
		// rounded rectangle is drawn into it with the AS2 drawing API. Being a child of the
		// flyout, it moves and fades in and out together with it.

		constexpr auto   BACKGROUND_NAME = "FTIBackground";
		constexpr int    BACKGROUND_DEPTH = -16384;
		constexpr double BACKGROUND_PAD_X = 10.0;
		constexpr double BACKGROUND_PAD_Y = 5.0;
		constexpr double BACKGROUND_RADIUS = 6.0;

		struct Box
		{
			double left{ 0 }, top{ 0 }, right{ 0 }, bottom{ 0 };
			bool   valid{ false };

			void Add(double a_left, double a_top, double a_right, double a_bottom)
			{
				if (!valid) {
					left = a_left;
					top = a_top;
					right = a_right;
					bottom = a_bottom;
					valid = true;
					return;
				}
				left = (std::min)(left, a_left);
				top = (std::min)(top, a_top);
				right = (std::max)(right, a_right);
				bottom = (std::max)(bottom, a_bottom);
			}
		};

		// Adds a non-empty text field to the box. a_x/a_y/a_scaleX/a_scaleY describe the
		// field's parent clip relative to the flyout (identity for the title).
		void AddTextField(const RE::GFxValue& a_field, double a_x, double a_y, double a_scaleX, double a_scaleY, Box& a_box)
		{
			if (ReadText(a_field).empty()) {
				return;
			}
			double x = 0.0;
			double y = 0.0;
			double w = 0.0;
			double h = 0.0;
			if (!GetNumber(a_field, "_x", x) || !GetNumber(a_field, "_y", y) ||
				!GetNumber(a_field, "_width", w) || !GetNumber(a_field, "_height", h) || w <= 0.0 || h <= 0.0) {
				return;
			}
			a_box.Add(a_x + x * a_scaleX, a_y + y * a_scaleY, a_x + (x + w) * a_scaleX, a_y + (y + h) * a_scaleY);
		}

		// The area covered by the title and the visible rows, in the flyout's own coordinates.
		Box MeasureFlyout(const RE::GFxValue& a_description)
		{
			Box          box;
			RE::GFxValue title;
			if (GetMember(a_description, "Title", title)) {
				AddTextField(title, 0.0, 0.0, 1.0, 1.0, box);
			}
			RE::GFxValue list;
			if (!GetMember(a_description, "DescriptionList", list) || !list.IsArray()) {
				return box;
			}
			const auto size = list.GetArraySize();
			for (std::uint32_t i = 0; i < size; ++i) {
				RE::GFxValue row;
				RE::GFxValue visible;
				if (!list.GetElement(i, &row) || !GetMember(row, "_visible", visible) || !visible.IsBool() || !visible.GetBool()) {
					continue;
				}
				double x = 0.0;
				double y = 0.0;
				double scaleX = 100.0;
				double scaleY = 100.0;
				if (!GetNumber(row, "_x", x) || !GetNumber(row, "_y", y)) {
					continue;
				}
				GetNumber(row, "_xscale", scaleX);
				GetNumber(row, "_yscale", scaleY);
				for (const auto name : { "Item", "Value" }) {
					RE::GFxValue field;
					if (GetMember(row, name, field)) {
						AddTextField(field, x, y, scaleX / 100.0, scaleY / 100.0, box);
					}
				}
			}
			return box;
		}

		// Finds our background clip, creating it if needed. Never replaces anything the
		// interface itself has placed at that depth.
		bool GetBackgroundClip(RE::GFxValue& a_description, RE::GFxValue& a_clip, bool a_create)
		{
			if (GetMember(a_description, BACKGROUND_NAME, a_clip) && a_clip.IsDisplayObject()) {
				return true;
			}
			if (!a_create || !a_description.IsDisplayObject()) {
				return false;
			}
			RE::GFxValue existing;
			RE::GFxValue depth{ static_cast<double>(BACKGROUND_DEPTH) };
			if (a_description.Invoke("getInstanceAtDepth", &existing, &depth, 1) && !existing.IsUndefined() && !existing.IsNull()) {
				return false;  // depth taken by something else
			}
			if (!a_description.CreateEmptyMovieClip(&a_clip, BACKGROUND_NAME, BACKGROUND_DEPTH) || !a_clip.IsDisplayObject()) {
				return false;
			}
			a_clip.SetMember("hitTestDisable", RE::GFxValue(true));  // never intercept the mouse
			return true;
		}

		void Call(RE::GFxValue& a_clip, const char* a_method, std::initializer_list<double> a_args)
		{
			std::vector<RE::GFxValue> args;
			args.reserve(a_args.size());
			for (const auto value : a_args) {
				args.emplace_back(value);
			}
			a_clip.Invoke(a_method, nullptr, args.data(), args.size());
		}

		// Rounded rectangle from straight segments (moveTo/lineTo are what SkyUI itself uses).
		void DrawBackground(RE::GFxValue& a_clip, const HoverState::BackgroundShape& a_shape)
		{
			a_clip.Invoke("clear");
			if (a_shape.opacity <= 0 || a_shape.w <= 0.0 || a_shape.h <= 0.0) {
				return;
			}

			const double r = (std::min)({ BACKGROUND_RADIUS, a_shape.w / 2.0, a_shape.h / 2.0 });
			const double left = a_shape.x;
			const double top = a_shape.y;
			const double right = a_shape.x + a_shape.w;
			const double bottom = a_shape.y + a_shape.h;

			// Corner centres and start angles (degrees, y pointing down), clockwise from top-left.
			const std::array<std::array<double, 3>, 4> corners{ {
				{ left + r, top + r, 180.0 },
				{ right - r, top + r, 270.0 },
				{ right - r, bottom - r, 0.0 },
				{ left + r, bottom - r, 90.0 },
			} };
			constexpr int STEPS = 4;

			Call(a_clip, "beginFill", { 0.0, static_cast<double>(a_shape.opacity) });  // black
			bool first = true;
			for (const auto& [cx, cy, start] : corners) {
				for (int i = 0; i <= STEPS; ++i) {
					const double angle = (start + 90.0 * i / STEPS) * std::numbers::pi / 180.0;
					const double px = cx + r * std::cos(angle);
					const double py = cy + r * std::sin(angle);
					Call(a_clip, first ? "moveTo" : "lineTo", { px, py });
					first = false;
				}
			}
			Call(a_clip, "lineTo", { left, top + r });  // close the shape
			a_clip.Invoke("endFill");
		}

		// Caller holds g_stateLock.
		void ClearBackground(RE::GFxValue& a_description)
		{
			if (!g_state.background) {
				return;
			}
			RE::GFxValue clip;
			if (GetBackgroundClip(a_description, clip, false)) {
				clip.Invoke("clear");
			}
			g_state.background.reset();
		}

		// Caller holds g_stateLock.
		void UpdateBackground(RE::GFxValue& a_description, const Settings& a_settings)
		{
			if (!a_settings.enabled || !a_settings.showBackground || a_settings.backgroundOpacity <= 0) {
				ClearBackground(a_description);
				return;
			}
			const auto box = MeasureFlyout(a_description);
			if (!box.valid) {
				return;
			}
			const HoverState::BackgroundShape shape{
				std::round(box.left - BACKGROUND_PAD_X),
				std::round(box.top - BACKGROUND_PAD_Y),
				std::round(box.right - box.left + 2.0 * BACKGROUND_PAD_X),
				std::round(box.bottom - box.top + 2.0 * BACKGROUND_PAD_Y),
				a_settings.backgroundOpacity
			};

			RE::GFxValue clip;
			const bool   existed = GetBackgroundClip(a_description, clip, false);
			if (existed && g_state.background == shape) {
				return;  // already drawn exactly like this
			}
			if (!existed && !GetBackgroundClip(a_description, clip, true)) {
				if (!g_state.warnedNoBackground) {
					g_state.warnedNoBackground = true;
					logs::warn("Could not add the flyout background (the map interface may be from another mod)");
				}
				return;
			}
			DrawBackground(clip, shape);
			g_state.background = shape;
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

		// Rows we added: current labels, plus labels used earlier for this marker (in case the
		// settings changed while the flyout was showing). Caller holds g_stateLock.
		bool IsOurLabel(const Settings& a_settings, const std::string& a_item)
		{
			if (a_item == a_settings.travelLabel || a_item == a_settings.arrivalLabel || a_item == a_settings.distanceLabel) {
				return true;
			}
			const auto matches = [&](const auto& a_line) { return a_line.item == a_item; };
			return std::ranges::any_of(g_state.lines, matches) || std::ranges::find(g_state.staleItems, a_item) != g_state.staleItems.end();
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

		void ComputeLines(const Settings& settings, const RE::MapMenu::RUNTIME_DATA2& a_data, std::int32_t a_selected, const std::string& a_title)
		{
			if (!settings.enabled) {
				return;
			}
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
			if (REL::Module::IsVR()) {
				return;
			}
			auto* data = a_menu->GetRuntimeData2();
			auto* movie = a_menu->uiMovie.get();
			if (!data || !movie) {
				return;
			}

			const auto settings = Settings::Current();

			std::lock_guard guard{ g_stateLock };

			if (g_resetRequested.exchange(false)) {
				const bool warned = g_state.warnedNoDescription;
				const bool warnedBackground = g_state.warnedNoBackground;
				g_state = HoverState{};
				g_state.warnedNoDescription = warned;
				g_state.warnedNoBackground = warnedBackground;
				g_state.settingsVersion = Settings::Version();
			}

			// Settings changed (in-game menu)? Recompute, and remember the old labels so the
			// rows they produced can be removed.
			const auto version = Settings::Version();
			const bool settingsChanged = version != g_state.settingsVersion;
			g_state.settingsVersion = version;

			if (!settings->enabled && !settingsChanged && g_state.lines.empty() && g_state.staleItems.empty() && !g_state.background) {
				return;  // switched off and nothing of ours left on screen
			}
			if (settings->enabled) {
				TryArmTrip(*data);
			}

			const auto selected = data->selectedMarker;
			if (selected < 0) {
				g_state.selected = -1;
				g_state.title.clear();
				g_state.lines.clear();
				g_state.staleItems.clear();
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
				// New marker: the game has redrawn the flyout, so nothing of ours is on it.
				g_state.selected = selected;
				g_state.title = title;
				g_state.lines.clear();
				g_state.staleItems.clear();
				g_state.destPos.reset();
				g_state.failedApplies = 0;
				ComputeLines(*settings, *data, selected, title);
			} else if (settingsChanged) {
				for (const auto& line : g_state.lines) {
					g_state.staleItems.push_back(line.item);
				}
				g_state.lines.clear();
				g_state.destPos.reset();
				g_state.failedApplies = 0;
				ComputeLines(*settings, *data, selected, title);
			}

			// Compare what the flyout shows with what it should show.
			const auto        visible = ReadVisibleLines(description);
			std::vector<Line> foreign;
			for (const auto& line : visible) {
				if (!IsOurLabel(*settings, line.item)) {
					foreign.push_back(line);
				}
			}
			auto wanted = foreign;
			wanted.insert(wanted.end(), g_state.lines.begin(), g_state.lines.end());

			// Re-check straight away: if the game later overwrites the flyout we simply
			// re-apply next frame, but if SetDescription itself does not produce our rows
			// (a replaced map.swf, say) we give up after a few tries instead of looping.
			if (visible != wanted && g_state.failedApplies < MAX_FAILED_APPLIES) {
				if (ApplyLines(movie, description, title, wanted) && ReadVisibleLines(description) == wanted) {
					g_state.failedApplies = 0;
				} else if (++g_state.failedApplies == MAX_FAILED_APPLIES) {
					logs::warn("Could not add rows to the flyout for \"{}\"", title);
				}
			}

			// Custom line spacing (100% = leave the game's layout alone). SetDescription resets
			// the layout whenever it runs, so this is re-checked every frame.
			if (settings->enabled && settings->lineSpacing != 100) {
				ApplySpacing(description, settings->lineSpacing / 100.0);
			}

			// Measured after the spacing, so the panel fits the final layout.
			UpdateBackground(description, *settings);
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
