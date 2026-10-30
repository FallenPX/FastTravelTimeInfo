#include "pch.h"

#include "Settings.h"

namespace FTI
{
	namespace
	{
		constexpr auto INI_PATH = "Data/SKSE/Plugins/FastTravelTimeInfo.ini"sv;

		std::mutex                      g_lock;
		std::shared_ptr<const Settings> g_current = std::make_shared<const Settings>();
		std::atomic<std::uint32_t>      g_version{ 1 };

		std::string Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_str.find_last_not_of(" \t\r\n");
			return std::string(a_str.substr(first, last - first + 1));
		}

		std::string Lower(std::string a_str)
		{
			std::ranges::transform(a_str, a_str.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_str;
		}

		// Values may be wrapped in double quotes so that leading/trailing spaces survive,
		// e.g. sMonthJoiner = " of ".
		std::string Unquote(std::string a_str)
		{
			if (a_str.size() >= 2 && a_str.front() == '"' && a_str.back() == '"') {
				return a_str.substr(1, a_str.size() - 2);
			}
			return a_str;
		}

		// Position of a ';' or '#' comment that is not inside double quotes, or npos.
		std::size_t FindComment(std::string_view a_line, std::size_t a_from = 0)
		{
			bool inQuotes = false;
			for (std::size_t i = a_from; i < a_line.size(); ++i) {
				if (a_line[i] == '"') {
					inQuotes = !inQuotes;
				} else if (!inQuotes && (a_line[i] == ';' || a_line[i] == '#')) {
					return i;
				}
			}
			return std::string_view::npos;
		}

		bool ToBool(const std::string& a_value, bool a_default)
		{
			const auto v = Lower(a_value);
			if (v == "1" || v == "true" || v == "yes" || v == "on") {
				return true;
			}
			if (v == "0" || v == "false" || v == "no" || v == "off") {
				return false;
			}
			return a_default;
		}

		float ToFloat(const std::string& a_value, float a_default)
		{
			try {
				return std::stof(a_value);
			} catch (...) {
				return a_default;
			}
		}

		// One INI key: where it lives, how to read it and how to write it.
		struct Field
		{
			std::string_view                                    name;     // as written in the INI
			std::string_view                                    section;  // without brackets
			std::function<void(Settings&, const std::string&)> parse;    // raw value, quotes already removed
			std::function<std::string(const Settings&)>        format;   // value as it should be written
		};

		Field MakeBool(std::string_view a_name, bool Settings::* a_member)
		{
			return Field{
				a_name, "General",
				[a_member](Settings& a_s, const std::string& a_v) { a_s.*a_member = ToBool(a_v, a_s.*a_member); },
				[a_member](const Settings& a_s) { return std::string(a_s.*a_member ? "1" : "0"); }
			};
		}

		Field MakeSeedSpeed()
		{
			return Field{
				"fSeedSpeed", "General",
				[](Settings& a_s, const std::string& a_v) {
					const auto v = ToFloat(a_v, a_s.seedSpeed);
					if (v > Settings::MIN_SEED_SPEED && v < Settings::MAX_SEED_SPEED) {
						a_s.seedSpeed = v;
					}
				},
				[](const Settings& a_s) { return std::format("{:.1f}", a_s.seedSpeed); }
			};
		}

		// Whole number, held within [a_min, a_max]; an unreadable value keeps the default.
		Field MakeInt(std::string_view a_name, int Settings::* a_member, int a_min, int a_max)
		{
			return Field{
				a_name, "General",
				[a_member, a_min, a_max](Settings& a_s, const std::string& a_v) {
					try {
						const auto v = static_cast<int>(std::lround(std::stod(a_v)));
						a_s.*a_member = std::clamp(v, a_min, a_max);
					} catch (...) {
					}
				},
				[a_member](const Settings& a_s) { return std::to_string(a_s.*a_member); }
			};
		}

		// a_required: an empty value is ignored (row labels must not be empty, or the plugin
		// could not tell its own rows apart from the game's).
		Field MakeString(std::string_view a_name, std::string Settings::* a_member, bool a_required = false)
		{
			return Field{
				a_name, "Text",
				[a_member, a_required](Settings& a_s, const std::string& a_v) {
					if (!a_required || !a_v.empty()) {
						a_s.*a_member = a_v;
					}
				},
				[a_member](const Settings& a_s) { return "\"" + a_s.*a_member + "\""; }
			};
		}

		const std::vector<Field>& Fields()
		{
			static const std::vector<Field> fields{
				MakeBool("bEnabled", &Settings::enabled),
				MakeBool("bUse24HourClock", &Settings::use24HourClock),
				MakeBool("bShowArrivalDate", &Settings::showArrivalDate),
				MakeBool("bShowDistance", &Settings::showDistance),
				MakeInt("iLineSpacing", &Settings::lineSpacing, Settings::MIN_LINE_SPACING, Settings::MAX_LINE_SPACING),
				MakeBool("bShowBackground", &Settings::showBackground),
				MakeInt("iBackgroundOpacity", &Settings::backgroundOpacity, 0, 100),
				MakeInt("iBackgroundCornerRadius", &Settings::backgroundCornerRadius, 0, Settings::MAX_CORNER_RADIUS),
				MakeBool("bOnlyFastTravelMarkers", &Settings::onlyFastTravelMarkers),
				MakeBool("bAutoCalibrate", &Settings::autoCalibrate),
				MakeSeedSpeed(),
				MakeString("sTravelLabel", &Settings::travelLabel, true),
				MakeString("sArrivalLabel", &Settings::arrivalLabel, true),
				MakeString("sDistanceLabel", &Settings::distanceLabel, true),
				MakeString("sApproxPrefix", &Settings::approxPrefix),
				MakeString("sDayUnit", &Settings::dayUnit),
				MakeString("sHourUnit", &Settings::hourUnit),
				MakeString("sMinuteUnit", &Settings::minuteUnit),
				MakeString("sMonthJoiner", &Settings::monthJoiner),
			};
			return fields;
		}

		const Field* FindField(const std::string& a_lowerKey)
		{
			for (const auto& field : Fields()) {
				if (Lower(std::string(field.name)) == a_lowerKey) {
					return &field;
				}
			}
			return nullptr;
		}

		// An active "key = value" line, split into its parts. Positions refer to the original line.
		struct KeyLine
		{
			std::string key;  // lower case
			std::string value;
			std::size_t valueStart{ 0 };
			std::size_t valueEnd{ 0 };
		};

		std::optional<KeyLine> ParseKeyLine(const std::string& a_line)
		{
			const auto trimmed = Trim(a_line);
			if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' || trimmed.front() == '[') {
				return std::nullopt;
			}
			const auto eq = a_line.find('=');
			if (eq == std::string::npos) {
				return std::nullopt;
			}
			const auto comment = FindComment(a_line);
			if (comment != std::string::npos && comment < eq) {
				return std::nullopt;  // the '=' is inside a comment
			}

			KeyLine result;
			result.key = Lower(Trim(std::string_view(a_line).substr(0, eq)));
			if (result.key.empty()) {
				return std::nullopt;
			}
			const auto regionEnd = comment == std::string::npos ? a_line.size() : comment;
			auto       start = eq + 1;
			while (start < regionEnd && (a_line[start] == ' ' || a_line[start] == '\t')) {
				++start;
			}
			auto end = regionEnd;
			while (end > start && (a_line[end - 1] == ' ' || a_line[end - 1] == '\t' || a_line[end - 1] == '\r')) {
				--end;
			}
			result.valueStart = start;
			result.valueEnd = end;
			result.value = Unquote(a_line.substr(start, end - start));
			return result;
		}

		std::optional<std::string> SectionName(const std::string& a_line)
		{
			auto trimmed = Trim(a_line);
			if (const auto comment = FindComment(trimmed); comment != std::string::npos) {
				trimmed = Trim(std::string_view(trimmed).substr(0, comment));
			}
			if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
				return Lower(Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2)));
			}
			return std::nullopt;
		}

		struct IniFile
		{
			std::vector<std::string> lines;
			bool                     crlf{ true };
			bool                     exists{ false };
		};

		IniFile ReadIni()
		{
			IniFile       ini;
			std::ifstream file{ std::filesystem::path(INI_PATH), std::ios::binary };
			if (!file) {
				return ini;
			}
			ini.exists = true;
			bool sawLf = false;
			bool sawCrLf = false;
			for (std::string line; std::getline(file, line);) {
				sawLf = true;
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
					sawCrLf = true;
				}
				ini.lines.push_back(std::move(line));
			}
			ini.crlf = sawCrLf || !sawLf;
			return ini;
		}
	}

	std::shared_ptr<const Settings> Settings::Current()
	{
		std::lock_guard guard{ g_lock };
		return g_current;
	}

	std::uint32_t Settings::Version()
	{
		return g_version.load();
	}

	void Settings::Set(Settings a_settings)
	{
		auto next = std::make_shared<const Settings>(std::move(a_settings));
		{
			std::lock_guard guard{ g_lock };
			g_current = std::move(next);
		}
		++g_version;
	}

	Settings Settings::LoadFromIni()
	{
		Settings result;
		const auto ini = ReadIni();
		if (!ini.exists) {
			logs::info("No INI found at {}, using defaults", INI_PATH);
			return result;
		}
		for (const auto& line : ini.lines) {
			if (const auto parsed = ParseKeyLine(line)) {
				if (const auto field = FindField(parsed->key)) {
					field->parse(result, parsed->value);
				}
			}
		}
		logs::info("Settings: enabled={} 24h={} date={} distance={} lineSpacing={}% background={} ({}%, corners {}px) onlyFT={} autoCalibrate={} seedSpeed={}",
			result.enabled, result.use24HourClock, result.showArrivalDate, result.showDistance, result.lineSpacing,
			result.showBackground, result.backgroundOpacity, result.backgroundCornerRadius, result.onlyFastTravelMarkers, result.autoCalibrate, result.seedSpeed);
		return result;
	}

	void Settings::Reload()
	{
		Set(LoadFromIni());
	}

	bool Settings::SaveToIni(const Settings& a_settings)
	{
		auto           ini = ReadIni();
		const Settings defaults;
		const auto&    fields = Fields();

		std::vector<bool>                  present(fields.size(), false);
		std::map<std::string, std::size_t> sectionLastLine;  // lower-case section -> last line index
		std::string                        section;
		bool                               changed = false;

		for (std::size_t i = 0; i < ini.lines.size(); ++i) {
			auto& line = ini.lines[i];
			if (const auto name = SectionName(line)) {
				section = *name;
				sectionLastLine[section] = i;
				continue;
			}
			if (!Trim(line).empty()) {
				sectionLastLine[section] = i;
			}

			const auto parsed = ParseKeyLine(line);
			if (!parsed) {
				continue;
			}
			const auto field = FindField(parsed->key);
			if (!field) {
				continue;
			}
			present[static_cast<std::size_t>(field - fields.data())] = true;

			// Leave the line alone if it already means the same thing (e.g. "true" vs "1").
			Settings existing = defaults;
			field->parse(existing, parsed->value);
			const auto wanted = field->format(a_settings);
			if (field->format(existing) == wanted) {
				continue;
			}
			std::string rebuilt = line.substr(0, parsed->valueStart);
			if (parsed->valueStart == parsed->valueEnd && !rebuilt.empty() && rebuilt.back() == '=') {
				rebuilt += ' ';
			}
			rebuilt += wanted;
			rebuilt += line.substr(parsed->valueEnd);
			line = std::move(rebuilt);
			changed = true;
		}

		// Keys that are not in the file yet: add them only if they differ from the default,
		// so a deliberately commented-out line keeps meaning "use the default".
		std::map<std::string, std::vector<std::string>> additions;  // section -> new lines
		std::vector<std::string>                        sectionOrder;
		for (std::size_t i = 0; i < fields.size(); ++i) {
			if (present[i]) {
				continue;
			}
			const auto value = fields[i].format(a_settings);
			if (value == fields[i].format(defaults)) {
				continue;
			}
			const auto key = Lower(std::string(fields[i].section));
			if (!additions.contains(key)) {
				sectionOrder.push_back(key);
			}
			additions[key].push_back(std::format("{} = {}", fields[i].name, value));
		}

		if (!additions.empty()) {
			changed = true;

			// Insert into existing sections, last section first so earlier indices stay valid.
			std::vector<std::pair<std::size_t, std::string>> inserts;
			for (const auto& key : sectionOrder) {
				if (const auto it = sectionLastLine.find(key); it != sectionLastLine.end() && !key.empty()) {
					inserts.emplace_back(it->second, key);
				}
			}
			std::ranges::sort(inserts, std::greater{});
			for (const auto& [after, key] : inserts) {
				auto& lines = additions[key];
				ini.lines.insert(ini.lines.begin() + static_cast<std::ptrdiff_t>(after + 1), lines.begin(), lines.end());
				additions.erase(key);
			}

			// Sections that do not exist yet go at the end.
			for (const auto& key : sectionOrder) {
				const auto it = additions.find(key);
				if (it == additions.end()) {
					continue;
				}
				const auto& field = *std::ranges::find_if(fields, [&](const Field& a_f) { return Lower(std::string(a_f.section)) == key; });
				if (!ini.lines.empty() && !Trim(ini.lines.back()).empty()) {
					ini.lines.emplace_back();
				}
				ini.lines.push_back(std::format("[{}]", field.section));
				ini.lines.insert(ini.lines.end(), it->second.begin(), it->second.end());
			}
		}

		if (!changed) {
			return true;
		}

		// Write in place (not via a temporary file) so that a mod manager's hard link to
		// this file stays intact and its copy of the INI stays in sync.
		std::ofstream file{ std::filesystem::path(INI_PATH), std::ios::binary | std::ios::trunc };
		if (!file) {
			logs::warn("Could not write {}", INI_PATH);
			return false;
		}
		const std::string_view eol = ini.crlf ? "\r\n" : "\n";
		for (const auto& line : ini.lines) {
			file << line << eol;
		}
		file.flush();
		if (!file) {
			logs::warn("Writing {} failed", INI_PATH);
			return false;
		}
		logs::info("Settings saved to {}", INI_PATH);
		return true;
	}
}
