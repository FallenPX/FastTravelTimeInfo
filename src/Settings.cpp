#include "pch.h"

#include "Settings.h"

namespace FTI
{
	namespace
	{
		constexpr auto INI_PATH = "Data/SKSE/Plugins/FastTravelTimeInfo.ini"sv;

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
	}

	void Settings::Load()
	{
		std::ifstream file{ std::filesystem::path(INI_PATH) };
		if (!file) {
			logs::info("No INI found at {}, using defaults", INI_PATH);
			return;
		}

		std::string line;
		while (std::getline(file, line)) {
			// strip comments (a ';' or '#' inside double quotes is kept)
			bool inQuotes = false;
			for (std::size_t i = 0; i < line.size(); ++i) {
				if (line[i] == '"') {
					inQuotes = !inQuotes;
				} else if (!inQuotes && (line[i] == ';' || line[i] == '#')) {
					line.erase(i);
					break;
				}
			}
			const auto trimmed = Trim(line);
			if (trimmed.empty() || trimmed.front() == '[') {
				continue;
			}
			const auto eq = trimmed.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const auto key = Lower(Trim(std::string_view(trimmed).substr(0, eq)));
			const auto value = Unquote(Trim(std::string_view(trimmed).substr(eq + 1)));

			if (key == "benabled") {
				enabled = ToBool(value, enabled);
			} else if (key == "buse24hourclock") {
				use24HourClock = ToBool(value, use24HourClock);
			} else if (key == "bshowarrivaldate") {
				showArrivalDate = ToBool(value, showArrivalDate);
			} else if (key == "bshowdistance") {
				showDistance = ToBool(value, showDistance);
			} else if (key == "bonlyfasttravelmarkers") {
				onlyFastTravelMarkers = ToBool(value, onlyFastTravelMarkers);
			} else if (key == "bautocalibrate") {
				autoCalibrate = ToBool(value, autoCalibrate);
			} else if (key == "fseedspeed") {
				const auto v = ToFloat(value, seedSpeed);
				if (v > 1.0f && v < 10000.0f) {
					seedSpeed = v;
				}
			} else if (key == "stravellabel") {
				travelLabel = value;
			} else if (key == "sarrivallabel") {
				arrivalLabel = value;
			} else if (key == "sdistancelabel") {
				distanceLabel = value;
			} else if (key == "sapproxprefix") {
				approxPrefix = value;
			} else if (key == "sdayunit") {
				dayUnit = value;
			} else if (key == "shourunit") {
				hourUnit = value;
			} else if (key == "sminuteunit") {
				minuteUnit = value;
			} else if (key == "smonthjoiner") {
				monthJoiner = value;
			}
		}

		logs::info("Settings: enabled={} 24h={} date={} distance={} onlyFT={} autoCalibrate={} seedSpeed={}",
			enabled, use24HourClock, showArrivalDate, showDistance, onlyFastTravelMarkers, autoCalibrate, seedSpeed);
	}
}
