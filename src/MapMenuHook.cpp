#include "pch.h"

#include "MapMenuHook.h"
#include "Estimator.h"
#include "FTTI_API.h"
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

		struct UnlearnedTrip
		{
			RE::NiPoint3             to;  // the marker's own position
			const RE::TESWorldSpace* world{ nullptr };
			std::string              title;
			std::optional<double>    hours;  // the estimate shown, if any
		};

		struct HoverState
		{
			std::int32_t                selected{ -1 };
			std::string                 title;
			std::vector<Line>           lines;    // our rows for the current marker
			std::optional<RE::NiPoint3> destPos;  // current marker position (for calibration)
			std::optional<UnlearnedTrip> unlearned;  // a trip not used for calibration (log only)
			std::vector<std::string>    staleItems;  // labels used earlier for this marker
			std::uint32_t               settingsVersion{ 0 };
			int                         failedApplies{ 0 };
			bool                        tripArmed{ false };
			int                         lookReads{ 0 };       // attempts to read the flyout's look this map opening
			bool                        lookComplete{ false };
			double                      lookUnits{ 0.0 };     // the flyout's scale, and since when it has been that
			std::chrono::steady_clock::time_point lookUnitsSince{};
			bool                        warnedNoDescription{ false };
			bool                        warnedNoBackground{ false };

			struct BackgroundShape
			{
				double x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
				double radius{ 0 };
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

		// ---- Flyout look, for Fast Travel Arrival Planner's panel
		//
		// The interface mod (vanilla, SkyUI, Dear Diary...) decides how the flyout's text looks, so
		// it is read from the flyout itself: colours, font, on-screen size and shadow. Read once
		// per map opening (until a row has been seen), and logged whenever it changes.

		struct FlyoutColors
		{
			std::optional<std::uint32_t> title;
			std::optional<std::uint32_t> label;
			std::optional<std::uint32_t> value;

			bool operator==(const FlyoutColors&) const = default;
		};

		struct TextShadow
		{
			std::uint32_t color{ 0 };
			double        alpha{ 0.0 };
			double        distance{ 0.0 };  // screen pixels
			double        angle{ 45.0 };    // degrees, 0 = right, 90 = down
			double        blur{ 0.0 };      // screen pixels
		};

		struct TextInfo
		{
			std::string               font;      // face, after the game's font configuration
			std::string               fontAsSet;  // as the flyout names it, e.g. "$EverywhereMediumFont"
			double                    sizePx{ 0.0 };  // line height on screen (what ImGui calls the font size)
			double                    units{ 0.0 };   // screen pixels per text unit when measured
			bool                      shadowKnown{ false };
			std::optional<TextShadow> shadow;
			std::string               how;           // how the size was worked out (log)
			std::string               shadowSource;  // where the shadow was read (log)
		};

		std::mutex              g_lookLock;  // guards the three below
		FlyoutColors            g_colors;
		std::optional<TextInfo> g_titleText;
		std::optional<TextInfo> g_rowText;

		std::optional<std::uint32_t> ReadColor(const RE::GFxValue& a_textField)
		{
			RE::GFxValue color;
			if (GetMember(a_textField, "textColor", color) && color.IsNumber() && color.GetNumber() >= 0.0) {
				return static_cast<std::uint32_t>(color.GetNumber()) & 0xFFFFFFu;
			}
			return std::nullopt;
		}

		// Interface\fontconfig.txt maps the names the menus use ("$EverywhereMediumFont") to font
		// faces in the font libraries. Interface mods that change the font change this file.
		const std::map<std::string, std::string>& FontConfig()
		{
			static std::map<std::string, std::string> map;  // lower-case "$name" -> face
			static std::once_flag                     once;
			std::call_once(once, [] {
				RE::BSResourceNiBinaryStream stream("Interface/fontconfig.txt");
				if (!stream.good()) {
					logs::info("Interface\\fontconfig.txt not found; font names are used as the flyout gives them");
					return;
				}
				std::string text;  // a small text file: read it character by character
				for (char c = 0; text.size() < 256 * 1024 && stream.read(&c, 1);) {
					text.push_back(c);
				}
				std::istringstream lines{ text };
				for (std::string line; std::getline(lines, line);) {
					const auto start = line.find_first_not_of(" \t");
					if (start == std::string::npos || line.compare(start, 3, "map") != 0) {
						continue;
					}
					std::vector<std::string> quoted;
					for (auto open = line.find('"'); open != std::string::npos && quoted.size() < 2; open = line.find('"', open + 1)) {
						const auto close = line.find('"', open + 1);
						if (close == std::string::npos) {
							break;
						}
						quoted.push_back(line.substr(open + 1, close - open - 1));
						open = close;
					}
					if (quoted.size() == 2 && !quoted[0].empty() && !quoted[1].empty()) {
						auto key = quoted[0];
						std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						map[key] = quoted[1];
					}
				}
			});
			return map;
		}

		std::string ResolveFont(const std::string& a_name)
		{
			if (a_name.empty() || a_name.front() != '$') {
				return a_name;
			}
			auto key = a_name;
			std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const auto& map = FontConfig();
			const auto  it = map.find(key);
			return it != map.end() ? it->second : a_name;
		}

		// Stage (the movie's own pixels) to screen pixels. The game scales its menus to fit the
		// screen keeping their proportions, so the smaller of the two ratios applies. Measured
		// from the movie's own size and the game's screen size; the movie's viewport and visible
		// area are logged alongside, as a cross-check.
		double MovieToScreen(RE::GFxMovieView* a_movie, std::string& a_how)
		{
			double stageW = 0.0;
			double stageH = 0.0;
			if (const auto def = a_movie->GetMovieDef()) {
				stageW = def->GetWidth();
				stageH = def->GetHeight();
			}
			double screenW = 0.0;
			double screenH = 0.0;
			if (const auto state = RE::BSGraphics::State::GetSingleton()) {
				screenW = state->screenWidth;
				screenH = state->screenHeight;
			}
			RE::GViewport viewport;
			a_movie->GetViewport(&viewport);
			const auto visible = a_movie->GetVisibleFrameRect();
			const double visibleW = static_cast<double>(visible.right) - visible.left;
			const double visibleH = static_cast<double>(visible.bottom) - visible.top;
			if (screenW <= 0.0 || screenH <= 0.0) {  // fall back on the movie's viewport
				screenW = viewport.width;
				screenH = viewport.height;
			}

			double scale = 0.0;
			if (stageW > 0.0 && stageH > 0.0 && screenW > 0.0 && screenH > 0.0) {
				scale = (std::min)(screenW / stageW, screenH / stageH);
			} else if (visibleH > 0.0 && screenH > 0.0) {
				scale = screenH / visibleH;
			}
			if (!(scale > 0.05 && scale < 20.0)) {
				scale = 1.0;
			}
			a_how = std::format("movie {:.0f} x {:.0f} on screen {:.0f} x {:.0f} = x{:.2f} (viewport {} x {}, visible area {:.0f} x {:.0f})",
				stageW, stageH, screenW, screenH, scale, viewport.width, viewport.height, visibleW, visibleH);
			return scale;
		}

		// How much one unit inside a text field measures on the stage: the scale of everything
		// it sits in, and its own. Two ways of reading the scale of what it sits in, the second
		// for when the first gives nothing.
		double StageUnits(RE::GFxMovieView* a_movie, const RE::GFxValue& a_field, std::string& a_how)
		{
			double       toStage = 0.0;
			const char*  method = "unknown, taken as x1";
			RE::GFxValue parent;
			if (GetMember(a_field, "_parent", parent) && parent.IsDisplayObject()) {
				RE::GFxValue top;
				RE::GFxValue bottom;
				a_movie->CreateObject(&top);
				a_movie->CreateObject(&bottom);
				top.SetMember("x", RE::GFxValue(0.0));
				top.SetMember("y", RE::GFxValue(0.0));
				bottom.SetMember("x", RE::GFxValue(0.0));
				bottom.SetMember("y", RE::GFxValue(100.0));
				double y0 = 0.0;
				double y1 = 0.0;
				if (parent.Invoke("localToGlobal", nullptr, &top, 1) && parent.Invoke("localToGlobal", nullptr, &bottom, 1) &&
					GetNumber(top, "y", y0) && GetNumber(bottom, "y", y1) && y1 > y0) {
					toStage = (y1 - y0) / 100.0;
					method = "localToGlobal";
				}
				if (!(toStage > 0.0)) {
					// The parent's height on the stage against its own height.
					RE::GFxValue root;
					RE::GFxValue onStage;
					RE::GFxValue own;
					double       a0 = 0.0, a1 = 0.0, b0 = 0.0, b1 = 0.0;
					if (a_movie->GetVariable(&root, "_root") && parent.Invoke("getBounds", &onStage, &root, 1) &&
						parent.Invoke("getBounds", &own, &parent, 1) && GetNumber(onStage, "yMin", a0) && GetNumber(onStage, "yMax", a1) &&
						GetNumber(own, "yMin", b0) && GetNumber(own, "yMax", b1) && b1 > b0 && a1 > a0) {
						toStage = (a1 - a0) / (b1 - b0);
						method = "getBounds";
					}
				}
			}
			if (!(toStage > 0.01 && toStage < 100.0)) {
				toStage = 1.0;
			}
			double fieldScale = 100.0;
			GetNumber(a_field, "_yscale", fieldScale);
			a_how = std::format("parent x{:.2f} ({}), field x{:.2f}", toStage, method, fieldScale / 100.0);
			return toStage * (fieldScale / 100.0);
		}

		struct ShadowRead
		{
			bool                      known{ false };  // the flyout said whether its text has a shadow
			std::optional<TextShadow> shadow;
			std::string               source;          // for the log
		};

		// A drop shadow or glow, searched for in the text's filters and those of everything it
		// sits in, then in Scaleform's text-shadow properties. Many interfaces draw the shadow in
		// a way none of these report (an empty filter list proves nothing), so "not found" means
		// "not known", never "no shadow".
		ShadowRead ReadShadow(const RE::GFxValue& a_field, double a_units)
		{
			ShadowRead   result;
			RE::GFxValue owner = a_field;
			for (int level = 0; level < 8 && owner.IsObject(); ++level) {
				RE::GFxValue filters;
				if (GetMember(owner, "filters", filters) && filters.IsArray()) {
					for (std::uint32_t i = 0; i < filters.GetArraySize(); ++i) {
						RE::GFxValue filter;
						double       color = 0.0;
						double       alpha = 0.0;
						if (!filters.GetElement(i, &filter) || !GetNumber(filter, "color", color) || !GetNumber(filter, "alpha", alpha)) {
							continue;
						}
						TextShadow shadow;
						shadow.color = static_cast<std::uint32_t>(color) & 0xFFFFFFu;
						shadow.alpha = std::clamp(alpha, 0.0, 1.0);
						double value = 0.0;
						if (GetNumber(filter, "distance", value)) {
							shadow.distance = value * a_units;
						}
						if (GetNumber(filter, "angle", value)) {
							shadow.angle = value;
						}
						if (GetNumber(filter, "blurX", value)) {
							shadow.blur = value * a_units;
						}
						result.known = true;
						result.shadow = shadow;
						result.source = level == 0 ? "the text's filters"s : std::format("filters {} level(s) up", level);
						return result;
					}
				}
				RE::GFxValue parent;
				if (!GetMember(owner, "_parent", parent) || !parent.IsDisplayObject()) {
					break;
				}
				owner = parent;
			}
			// Scaleform's text-shadow properties.
			double alpha = 0.0;
			double distance = 0.0;
			if (GetNumber(a_field, "shadowAlpha", alpha) && alpha > 0.0 && GetNumber(a_field, "shadowDistance", distance)) {
				TextShadow shadow;
				double     value = 0.0;
				if (GetNumber(a_field, "shadowColor", value)) {
					shadow.color = static_cast<std::uint32_t>(value) & 0xFFFFFFu;
				}
				shadow.alpha = std::clamp(alpha, 0.0, 1.0);
				shadow.distance = distance * a_units;
				if (GetNumber(a_field, "shadowAngle", value)) {
					shadow.angle = value;
				}
				if (GetNumber(a_field, "shadowBlurX", value)) {
					shadow.blur = value * a_units;
				}
				result.known = true;
				result.shadow = shadow;
				result.source = "Scaleform's text-shadow properties";
				return result;
			}
			// Scaleform's text shadow style, e.g. shadowStyle = "s{1,1}t{0,0}" (offsets in pixels).
			RE::GFxValue style;
			if (GetMember(a_field, "shadowStyle", style) && style.IsString() && style.GetString() && *style.GetString()) {
				const std::string text = style.GetString();
				const auto        mark = text.find('s');
				const auto        open = mark == std::string::npos ? std::string::npos : text.find('{', mark);
				const auto        close = open == std::string::npos ? std::string::npos : text.find('}', open);
				double            x = 0.0;
				double            y = 0.0;
				if (close != std::string::npos &&
					std::sscanf(text.substr(open + 1, close - open - 1).c_str(), "%lf,%lf", &x, &y) == 2 && (x != 0.0 || y != 0.0)) {
					TextShadow shadow;
					if (double c = 0.0; GetNumber(a_field, "shadowColor", c)) {
						shadow.color = static_cast<std::uint32_t>(c) & 0xFFFFFFu;
					}
					shadow.alpha = 0.85;
					shadow.distance = std::hypot(x, y) * a_units;
					shadow.angle = std::atan2(y, x) * 180.0 / 3.14159265358979;
					result.known = true;
					result.shadow = shadow;
					result.source = std::format("Scaleform text shadow \"{}\"", text);
					return result;
				}
			}
			result.source = "not found (the interface may draw it in a way that cannot be read)";
			return result;
		}

		std::optional<TextInfo> ReadTextInfo(RE::GFxMovieView* a_movie, const RE::GFxValue& a_field)
		{
			RE::GFxValue format;
			if (!a_field.IsObject() || !const_cast<RE::GFxValue&>(a_field).Invoke("getTextFormat", &format) || !format.IsObject()) {
				return std::nullopt;
			}
			TextInfo     info;
			RE::GFxValue font;
			if (GetMember(format, "font", font) && font.IsString() && font.GetString()) {
				info.fontAsSet = font.GetString();
				info.font = ResolveFont(info.fontAsSet);
			}
			std::string  screenHow;
			std::string  stageHow;
			const double toScreen = MovieToScreen(a_movie, screenHow);
			const double stage = StageUnits(a_movie, a_field, stageHow);
			// ImGui sizes a font by its line height (ascent + descent), Flash by its em size, so
			// the line height is what matches: the field's text height for one line, or 1.2 em.
			double size = 0.0;
			double textHeight = 0.0;
			GetNumber(format, "size", size);
			GetNumber(a_field, "textHeight", textHeight);
			double line = 0.0;
			if (size > 0.0) {
				line = textHeight >= size && textHeight <= size * 1.6 ? textHeight : size * 1.2;
			}
			info.units = stage * toScreen;
			info.sizePx = line * info.units;
			info.how = std::format("text size {:.1f}, line {:.1f}; {}; {}", size, textHeight, stageHow, screenHow);
			const auto shadow = ReadShadow(a_field, stage * toScreen);
			info.shadowKnown = shadow.known;
			info.shadow = shadow.shadow;
			info.shadowSource = shadow.source;
			return info;
		}

		std::string DescribeText(const std::optional<TextInfo>& a_info)
		{
			if (!a_info) {
				return "unknown";
			}
			auto text = std::format("\"{}\"", a_info->font);
			if (!a_info->fontAsSet.empty() && a_info->fontAsSet != a_info->font) {
				text += std::format(" ({})", a_info->fontAsSet);
			}
			text += std::format(" {:.1f} px [{}]", a_info->sizePx, a_info->how);
			if (const auto& s = a_info->shadow) {
				text += std::format(", shadow #{:06X} {:.0f}% {:.1f} px at {:.0f} deg, blur {:.1f} px (from {})", s->color, s->alpha * 100.0,
					s->distance, s->angle, s->blur, a_info->shadowSource);
			} else {
				text += a_info->shadowKnown ? ", no shadow"s : std::format(", shadow {}", a_info->shadowSource);
			}
			return text;
		}

		bool SameText(const std::optional<TextInfo>& a, const std::optional<TextInfo>& b)
		{
			if (a.has_value() != b.has_value()) {
				return false;
			}
			if (!a) {
				return true;
			}
			return a->font == b->font && std::abs(a->sizePx - b->sizePx) < 0.5 && a->shadow.has_value() == b->shadow.has_value() &&
			       a->shadowKnown == b->shadowKnown;
		}

		// Returns true once both the title and a row have been read at a steady size. The flyout
		// grows into place when it appears (from 20 %, in steps that each last two or more game
		// frames), so a size counts only once it has stayed the same for a fifth of a second.
		bool ReadFlyoutLook(RE::GFxMovieView* a_movie, const RE::GFxValue& a_description, double& a_lastUnits,
			std::chrono::steady_clock::time_point& a_unitsSince)
		{
			FlyoutColors            colors;
			std::optional<TextInfo> titleText;
			std::optional<TextInfo> rowText;
			RE::GFxValue            title;
			if (GetMember(a_description, "Title", title)) {
				colors.title = ReadColor(title);
				titleText = ReadTextInfo(a_movie, title);
			}
			RE::GFxValue list;
			if (GetMember(a_description, "DescriptionList", list) && list.IsArray()) {
				const auto size = list.GetArraySize();
				for (std::uint32_t i = 0; i < size && !rowText; ++i) {
					RE::GFxValue entry;
					RE::GFxValue visible;
					if (!list.GetElement(i, &entry) || !GetMember(entry, "_visible", visible) || !visible.IsBool() || !visible.GetBool()) {
						continue;
					}
					RE::GFxValue field;
					if (GetMember(entry, "Item", field)) {
						colors.label = ReadColor(field);
						rowText = ReadTextInfo(a_movie, field);
					}
					if (GetMember(entry, "Value", field)) {
						colors.value = ReadColor(field);
					}
				}
			}
			if (!colors.title && !colors.label && !colors.value && !titleText && !rowText) {
				return false;
			}
			const double units = titleText ? titleText->units : 0.0;
			const auto   now = std::chrono::steady_clock::now();
			if (std::abs(units - a_lastUnits) > (std::max)(units, a_lastUnits) * 0.005) {
				a_lastUnits = units;
				a_unitsSince = now;
			}
			const bool steady = units > 0.0 && now - a_unitsSince >= std::chrono::milliseconds(200);
			if (!steady) {
				titleText.reset();  // still growing: colours only this time
				rowText.reset();
			}

			std::lock_guard guard{ g_lookLock };
			// Keep earlier readings for anything not visible this time (a flyout without rows).
			if (!colors.label) {
				colors.label = g_colors.label;
			}
			if (!colors.value) {
				colors.value = g_colors.value;
			}
			if (!rowText) {
				rowText = g_rowText;
			}
			if (!titleText) {
				titleText = g_titleText;
			}
			if (!(colors == g_colors)) {
				g_colors = colors;
				const auto hex = [](const std::optional<std::uint32_t>& a) { return a ? std::format("#{:06X}", *a) : "unknown"s; };
				logs::info("Flyout colours: title {}, labels {}, values {}", hex(colors.title), hex(colors.label), hex(colors.value));
			}
			if (!SameText(titleText, g_titleText) || !SameText(rowText, g_rowText)) {
				g_titleText = titleText;
				g_rowText = rowText;
				logs::info("Flyout text: title {}; rows {}", DescribeText(titleText), DescribeText(rowText));
			}
			return steady && titleText.has_value() && rowText.has_value();
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
		//
		// The clip is created through ActionScript's own createEmptyMovieClip: Scaleform's C++
		// shortcut for it treats any negative depth as "on top", which put the panel over the text.

		constexpr auto   BACKGROUND_NAME = "FTIBackground";
		constexpr int    BACKGROUND_DEPTH = -16384;
		constexpr double BACKGROUND_PAD_X = 10.0;
		constexpr double BACKGROUND_PAD_TOP = 0.0;  // the title's own line height already leaves room above
		constexpr double BACKGROUND_PAD_BOTTOM = 5.0;

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

		bool IsDepthFree(RE::GFxValue& a_description, int a_depth)
		{
			RE::GFxValue existing;
			RE::GFxValue depth{ static_cast<double>(a_depth) };
			return a_description.Invoke("getInstanceAtDepth", &existing, &depth, 1) && (existing.IsUndefined() || existing.IsNull());
		}

		// Makes sure the clip is on the bottom layer, below every text field. Returns false if
		// it cannot be, in which case nothing is drawn (the panel must never cover the text).
		bool EnsureBottomLayer(RE::GFxValue& a_description, RE::GFxValue& a_clip)
		{
			RE::GFxValue depth;
			if (!a_clip.Invoke("getDepth", &depth) || !depth.IsNumber()) {
				return false;
			}
			if (static_cast<int>(depth.GetNumber()) == BACKGROUND_DEPTH) {
				return true;
			}
			if (!IsDepthFree(a_description, BACKGROUND_DEPTH)) {
				return false;
			}
			RE::GFxValue target{ static_cast<double>(BACKGROUND_DEPTH) };
			a_clip.Invoke("swapDepths", nullptr, &target, 1);
			return a_clip.Invoke("getDepth", &depth) && depth.IsNumber() && static_cast<int>(depth.GetNumber()) == BACKGROUND_DEPTH;
		}

		// Finds our background clip, creating it if needed. Never replaces anything the
		// interface itself has placed at that depth.
		bool GetBackgroundClip(RE::GFxValue& a_description, RE::GFxValue& a_clip, bool a_create)
		{
			if (GetMember(a_description, BACKGROUND_NAME, a_clip) && a_clip.IsDisplayObject()) {
				return true;
			}
			if (!a_create || !a_description.IsDisplayObject() || !IsDepthFree(a_description, BACKGROUND_DEPTH)) {
				return false;
			}
			std::array<RE::GFxValue, 2> args{ RE::GFxValue(BACKGROUND_NAME), RE::GFxValue(static_cast<double>(BACKGROUND_DEPTH)) };
			if (!a_description.Invoke("createEmptyMovieClip", &a_clip, args.data(), args.size()) || !a_clip.IsDisplayObject()) {
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

			const double r = (std::max)(0.0, (std::min)({ a_shape.radius, a_shape.w / 2.0, a_shape.h / 2.0 }));
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
			const int steps = r < 0.5 ? 0 : (r < 4.0 ? 3 : 4);  // 0 = square corners

			Call(a_clip, "beginFill", { 0.0, static_cast<double>(a_shape.opacity) });  // black
			bool first = true;
			for (const auto& [cx, cy, start] : corners) {
				for (int i = 0; i <= steps; ++i) {
					const double angle = (start + (steps > 0 ? 90.0 * i / steps : 0.0)) * std::numbers::pi / 180.0;
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
				std::round(box.top - BACKGROUND_PAD_TOP),
				std::round(box.right - box.left + 2.0 * BACKGROUND_PAD_X),
				std::round(box.bottom - box.top + BACKGROUND_PAD_TOP + BACKGROUND_PAD_BOTTOM),
				static_cast<double>(a_settings.backgroundCornerRadius),
				a_settings.backgroundOpacity
			};

			RE::GFxValue clip;
			const bool   existed = GetBackgroundClip(a_description, clip, false);
			if (existed && g_state.background == shape) {
				return;  // already drawn exactly like this
			}
			if ((!existed && !GetBackgroundClip(a_description, clip, true)) || !EnsureBottomLayer(a_description, clip)) {
				if (existed) {
					clip.Invoke("clear");  // never leave a panel on top of the text
				}
				g_state.background.reset();
				if (!g_state.warnedNoBackground) {
					g_state.warnedNoBackground = true;
					logs::warn("Could not place the flyout background behind the text (the map interface may be from another mod)");
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

		// a_data may be null when the map is not open.
		std::optional<RE::NiPoint3> PlayerOrigin(const RE::MapMenu::RUNTIME_DATA2* a_data)
		{
			if (auto player = RE::PlayerCharacter::GetSingleton()) {
				if (auto cell = player->GetParentCell(); cell && !cell->IsInteriorCell()) {
					return player->GetPosition();
				}
			}
			// Interior (only possible with mods that allow fast travel indoors):
			// use the position the map draws the player marker at.
			if (a_data) {
				const auto& pos = a_data->playerMarkerPosition;
				if (pos.x != 0.0f || pos.y != 0.0f) {
					return pos;
				}
			}
			return std::nullopt;
		}

		// ---- Worldspaces
		//
		// The game measures a fast travel between where both places are drawn on the world map.
		// A child worldspace drawn on its parent's map (Fort Dawnguard and the Forgotten Vale on
		// Skyrim's) places its markers at position * map scale + map offset; the walled cities are
		// built on Skyrim's own coordinates and need no conversion. Trips into and out of such
		// areas are estimated from those map positions, which matched the game within a few
		// percent (Fort Dawnguard both ways, out of the Vale). They are not used for calibration.
		// Exceptions, with no estimate:
		// - areas drawn at a different scale (the Forgotten Vale): the game passed no time going
		//   in, whatever the distance, so trips into them (or within them) are not estimated;
		// - places on different maps (Solstheim and Skyrim).
		// (0.1.4-0.1.5 compared raw positions across worlds; 0.1.6-0.1.8 gave no estimate at all
		// for Fort Dawnguard, the Vale and similar areas.)

		bool HasIdentityMap(const RE::TESWorldSpace* a_world)
		{
			const auto& map = a_world->worldMapOffsetData;
			return std::abs(map.mapScale - 1.0f) < 1.0e-4f && std::abs(map.mapOffsetX) < 1.0f &&
			       std::abs(map.mapOffsetY) < 1.0f && std::abs(map.mapOffsetZ) < 1.0f;
		}

		// Built on the parent's coordinates: takes its land or distant terrain (Whiterun, Riften,
		// Windhelm, Solitude), or is drawn on its map unmoved and unscaled (Markarth).
		bool SharesParentCoordinates(const RE::TESWorldSpace* a_world)
		{
			using Flag = RE::TESWorldSpace::ParentUseFlag;
			if (!a_world || !a_world->parentWorld) {
				return false;
			}
			if (a_world->parentUseFlags.any(Flag::kUseLandData, Flag::kUseLODData)) {
				return true;
			}
			return a_world->parentUseFlags.any(Flag::kUseMapData) && HasIdentityMap(a_world);
		}

		struct Placement
		{
			const RE::TESWorldSpace* map{ nullptr };  // the world whose map shows this place
			RE::NiPoint3             pos;             // the position on that map
			bool                     moved{ false };   // converted with a map offset or scale
			bool                     scaled{ false };  // drawn at a different scale
		};

		Placement PlaceOnMap(const RE::TESWorldSpace* a_world, const RE::NiPoint3& a_pos)
		{
			using Flag = RE::TESWorldSpace::ParentUseFlag;
			Placement result{ a_world, a_pos };
			for (int depth = 0; result.map && result.map->parentWorld && depth < 16; ++depth) {
				const auto* world = result.map;
				if (!SharesParentCoordinates(world)) {
					if (!world->parentUseFlags.any(Flag::kUseMapData)) {
						break;  // a world with a map of its own
					}
					const auto& map = world->worldMapOffsetData;
					result.pos = { result.pos.x * map.mapScale + map.mapOffsetX, result.pos.y * map.mapScale + map.mapOffsetY,
						result.pos.z * map.mapScale + map.mapOffsetZ };
					result.moved = true;
					if (std::abs(map.mapScale - 1.0f) >= 1.0e-4f) {
						result.scaled = true;
					}
				}
				result.map = world->parentWorld;
			}
			return result;
		}

		enum class TripKind
		{
			kPlain,        // same coordinates: estimate, and learn from it
			kOnMap,        // estimated from map positions, not learned from
			kIntoScaled,   // no estimate: into an area drawn at a different scale
			kOtherMap,     // no estimate: the two places are on different maps
		};

		struct TripPlan
		{
			TripKind     kind{ TripKind::kPlain };
			RE::NiPoint3 from;  // where to measure from and to
			RE::NiPoint3 to;
			RE::NiPoint3 fromMap;  // the same on the map (diagnostics)
			RE::NiPoint3 toMap;
			const RE::TESWorldSpace* originWorld{ nullptr };
			const RE::TESWorldSpace* destinationWorld{ nullptr };

			[[nodiscard]] bool HasEstimate() const { return kind == TripKind::kPlain || kind == TripKind::kOnMap; }
		};

		std::string WorldName(const RE::TESWorldSpace* a_world)
		{
			if (!a_world) {
				return "unknown";
			}
			const char* id = a_world->GetFormEditorID();
			return std::format("{} ({:08X})", id && *id ? id : "?", a_world->GetFormID());
		}

		std::string DescribeWorld(const RE::TESWorldSpace* a_world)
		{
			auto text = WorldName(a_world);
			if (a_world && a_world->parentWorld) {
				const auto& map = a_world->worldMapOffsetData;
				text += std::format(" [parent {}, parent flags {:02X}: {}; map scale {:g}, map offset ({:.0f}, {:.0f}, {:.0f})]",
					WorldName(a_world->parentWorld), a_world->parentUseFlags.underlying(),
					SharesParentCoordinates(a_world) ? "same coordinates" : "own coordinates",
					map.mapScale, map.mapOffsetX, map.mapOffsetY, map.mapOffsetZ);
			}
			return text;
		}

		const char* Describe(TripKind a_kind)
		{
			switch (a_kind) {
			case TripKind::kPlain:
				return "same coordinates, estimate given";
			case TripKind::kOnMap:
				return "estimate from the map positions, not used for calibration";
			case TripKind::kIntoScaled:
				return "destination drawn at a different scale on the map, no estimate";
			default:
				return "different maps, no estimate";
			}
		}

		// Logs the decision once per pair of worlds and game session, so it can be checked.
		// Trips within one worldspace, the usual case, are not logged.
		void LogWorlds(const RE::TESWorldSpace* a_origin, const RE::TESWorldSpace* a_destination, TripKind a_kind)
		{
			if (a_origin && a_origin == a_destination) {
				return;
			}
			static std::mutex                                       lock;
			static std::vector<std::pair<RE::FormID, RE::FormID>> logged;
			const std::pair key{ a_origin ? a_origin->GetFormID() : 0u, a_destination ? a_destination->GetFormID() : 0u };
			{
				std::scoped_lock guard{ lock };
				if (std::find(logged.begin(), logged.end(), key) != logged.end()) {
					return;
				}
				logged.push_back(key);
			}
			logs::info("Worlds: from {} to {}: {}", DescribeWorld(a_origin), DescribeWorld(a_destination), Describe(a_kind));
		}

		// The world the trip starts in: the player's own when outdoors, otherwise the one the
		// map is showing (indoor fast travel, only possible with mods).
		RE::TESWorldSpace* OriginWorldspace(const RE::MapMenu::RUNTIME_DATA2* a_data)
		{
			if (auto player = RE::PlayerCharacter::GetSingleton()) {
				if (auto cell = player->GetParentCell(); cell && !cell->IsInteriorCell()) {
					if (auto world = player->GetWorldspace()) {
						return world;
					}
				}
			}
			return a_data ? a_data->worldSpace : nullptr;
		}

		TripPlan PlanTrip(const RE::NiPoint3& a_origin, const RE::TESObjectREFR& a_destination, const RE::MapMenu::RUNTIME_DATA2* a_data)
		{
			TripPlan plan;
			plan.originWorld = OriginWorldspace(a_data);
			plan.destinationWorld = a_destination.GetWorldspace();
			plan.from = a_origin;
			plan.to = a_destination.GetPosition();
			plan.fromMap = plan.from;
			plan.toMap = plan.to;

			// Unknown worlds: measure as given, as before the worldspace checks.
			if (plan.originWorld && plan.destinationWorld && plan.originWorld != plan.destinationWorld) {
				const auto origin = PlaceOnMap(plan.originWorld, plan.from);
				const auto destination = PlaceOnMap(plan.destinationWorld, plan.to);
				plan.fromMap = origin.pos;
				plan.toMap = destination.pos;
				if (origin.map != destination.map) {
					plan.kind = TripKind::kOtherMap;
				} else if (destination.scaled) {
					plan.kind = TripKind::kIntoScaled;
				} else if (origin.moved || destination.moved) {
					plan.kind = TripKind::kOnMap;
					plan.from = origin.pos;
					plan.to = destination.pos;
				}
			} else if (plan.originWorld && plan.originWorld == plan.destinationWorld && PlaceOnMap(plan.originWorld, plan.from).scaled) {
				plan.kind = TripKind::kIntoScaled;  // within the Vale: not known how the game measures it
			}
			LogWorlds(plan.originWorld, plan.destinationWorld, plan.kind);
			return plan;
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
			const auto origin = PlayerOrigin(&a_data);
			if (!origin) {
				return;
			}
			const auto plan = PlanTrip(*origin, *ref, &a_data);
			std::optional<TripEstimate> estimate;
			if (plan.HasEstimate()) {
				estimate = Estimator::Get().Estimate(plan.from, plan.to);
			}
			if (plan.kind != TripKind::kPlain) {
				// Never used for calibration; kept to log what the game charges if the player goes.
				g_state.unlearned = UnlearnedTrip{ ref->GetPosition(), plan.destinationWorld, a_title,
					estimate ? std::optional<double>{ estimate->hours } : std::nullopt };
			}
			if (!estimate) {
				logs::debug("No estimate for \"{}\": {}", a_title, Describe(plan.kind));
				return;
			}

			if (plan.kind == TripKind::kPlain) {
				g_state.destPos = plan.to;
			}
			const bool approximate = !estimate->calibrated || plan.kind != TripKind::kPlain;
			g_state.lines.push_back({ settings.travelLabel, FormatDuration(estimate->hours, approximate) });
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
			const auto origin = PlayerOrigin(&a_data);
			if (!origin) {
				return;
			}
			if (g_state.destPos) {
				Estimator::Get().ArmTrip(*origin, *g_state.destPos);
				return;
			}
			if (!g_state.unlearned) {
				return;
			}
			const auto& trip = *g_state.unlearned;
			const auto  originWorld = OriginWorldspace(&a_data);
			logs::info("Trip to \"{}\" ({}): from ({:.0f}, {:.0f}, {:.0f}) in {} to ({:.0f}, {:.0f}, {:.0f}) in {}", trip.title,
				trip.hours ? "estimated from the map positions" : "no estimate", origin->x, origin->y, origin->z,
				WorldName(originWorld), trip.to.x, trip.to.y, trip.to.z, WorldName(trip.world));
			Estimator::Get().NoteUnlearnedTrip(*origin, trip.to, PlaceOnMap(originWorld, *origin).pos,
				PlaceOnMap(trip.world, trip.to).pos, trip.title, trip.hours);
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
				g_state.unlearned.reset();
				g_state.failedApplies = 0;
				ComputeLines(*settings, *data, selected, title);
			} else if (settingsChanged) {
				for (const auto& line : g_state.lines) {
					g_state.staleItems.push_back(line.item);
				}
				g_state.lines.clear();
				g_state.destPos.reset();
				g_state.unlearned.reset();
				g_state.failedApplies = 0;
				ComputeLines(*settings, *data, selected, title);
			}

			if (!g_state.lookComplete && g_state.lookReads < 3000) {
				++g_state.lookReads;
				g_state.lookComplete = ReadFlyoutLook(movie, description, g_state.lookUnits, g_state.lookUnitsSince);
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

	bool EstimateTripTo(const RE::TESObjectREFR& a_destination, FTTI_API::TripInfo& a_out)
	{
		a_out = FTTI_API::TripInfo{};
		if (REL::Module::IsVR()) {
			return false;
		}
		RE::GPtr<RE::MapMenu>               menu;
		const RE::MapMenu::RUNTIME_DATA2* data = nullptr;
		if (auto ui = RE::UI::GetSingleton()) {
			menu = ui->GetMenu<RE::MapMenu>();
			if (menu) {
				data = menu->GetRuntimeData2();
			}
		}
		const auto origin = PlayerOrigin(data);
		if (!origin) {
			return false;
		}
		const auto plan = PlanTrip(*origin, a_destination, data);
		if (!plan.HasEstimate()) {
			a_out.otherWorldspace = true;
			return false;
		}
		const auto estimate = Estimator::Get().Estimate(plan.from, plan.to);
		if (!estimate) {
			return false;
		}
		a_out.valid = true;
		a_out.calibrated = estimate->calibrated && plan.kind == TripKind::kPlain;
		a_out.hours = estimate->hours;
		a_out.distance = estimate->distance;
		a_out.speedMult = GetFastTravelSpeedMult();
		return true;
	}

	bool GetFlyoutStyle(FTTI_API::FlyoutStyle& a_out)
	{
		a_out = FTTI_API::FlyoutStyle{};
		const auto settings = Settings::Current();
		a_out.background = settings->showBackground;
		a_out.backgroundColor = 0x000000;  // as drawn in DrawBackground
		a_out.backgroundOpacity = static_cast<float>(settings->backgroundOpacity) / 100.0f;
		a_out.cornerRadius = static_cast<float>(settings->backgroundCornerRadius);

		std::lock_guard guard{ g_lookLock };
		if (g_colors.title) {
			a_out.titleColor = *g_colors.title;
		}
		if (g_colors.label) {
			a_out.labelColor = *g_colors.label;
		}
		if (g_colors.value) {
			a_out.valueColor = *g_colors.value;
		} else if (g_colors.title) {
			a_out.valueColor = *g_colors.title;
		}
		a_out.colorsKnown = g_colors.title.has_value() || g_colors.label.has_value() || g_colors.value.has_value();
		return a_out.colorsKnown;
	}

	bool GetFlyoutText(FTTI_API::FlyoutText& a_out)
	{
		a_out = FTTI_API::FlyoutText{};
		const auto copy = [](char* a_dst, std::size_t a_size, const std::string& a_src) {
			const auto length = (std::min)(a_src.size(), a_size - 1);
			std::memcpy(a_dst, a_src.data(), length);
			a_dst[length] = '\0';
		};
		std::lock_guard guard{ g_lookLock };
		if (g_titleText) {
			copy(a_out.titleFont, sizeof(a_out.titleFont), g_titleText->font);
			a_out.titleSize = static_cast<float>(g_titleText->sizePx);
		}
		const auto& row = g_rowText ? g_rowText : g_titleText;
		if (row) {
			copy(a_out.rowFont, sizeof(a_out.rowFont), row->font);
			a_out.rowSize = static_cast<float>(row->sizePx);
		}
		const auto& shadowSource = g_titleText && g_titleText->shadow ? g_titleText : row;
		a_out.shadowKnown = (g_titleText && g_titleText->shadowKnown) || (row && row->shadowKnown);
		if (shadowSource && shadowSource->shadow) {
			const auto& shadow = *shadowSource->shadow;
			a_out.shadow = true;
			a_out.shadowColor = shadow.color;
			a_out.shadowAlpha = static_cast<float>(shadow.alpha);
			a_out.shadowDistance = static_cast<float>(shadow.distance);
			a_out.shadowAngle = static_cast<float>(shadow.angle);
			a_out.shadowBlur = static_cast<float>(shadow.blur);
		}
		a_out.known = g_titleText.has_value() || g_rowText.has_value();
		return a_out.known;
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
