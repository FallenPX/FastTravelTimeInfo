#include "pch.h"

#include "Estimator.h"
#include "Settings.h"

namespace FTI
{
	namespace
	{
		constexpr std::size_t MAX_SAMPLES = 15;
		constexpr double      MIN_SAMPLE_DISTANCE = 8192.0;  // two exterior cells; shorter trips are too noisy
		constexpr double      MIN_PLAUSIBLE_SPEED = 5.0;
		constexpr double      MAX_PLAUSIBLE_SPEED = 5000.0;
		constexpr double      METRES_PER_UNIT = 1.8288 / 128.0;  // 128 units = 6 ft (Creation Kit scale)
		constexpr auto        PENDING_TIMEOUT = std::chrono::minutes(15);

		double Distance(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b)
		{
			const double dx = static_cast<double>(a_a.x) - a_b.x;
			const double dy = static_cast<double>(a_a.y) - a_b.y;
			const double dz = static_cast<double>(a_a.z) - a_b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		std::optional<std::filesystem::path> CalibrationPath()
		{
			auto dir = logs::log_directory();
			if (!dir) {
				return std::nullopt;
			}
			return *dir / "FastTravelTimeInfo_Calibration.txt";
		}

		std::string GameString(const char* a_setting, const char* a_fallback)
		{
			if (auto gmst = RE::GameSettingCollection::GetSingleton()) {
				if (auto setting = gmst->GetSetting(a_setting)) {
					if (auto str = setting->GetString(); str && str[0] != '\0') {
						return str;
					}
				}
			}
			return a_fallback;
		}

		std::string DayName(std::uint32_t a_dayOfWeek)
		{
			// Calendar::Day order: Sundas = 0 ... Loredas = 6
			static constexpr std::array<std::pair<const char*, const char*>, 7> days{ {
				{ "sDaySunday", "Sundas" },
				{ "sDayMonday", "Morndas" },
				{ "sDayTuesday", "Tirdas" },
				{ "sDayWednesday", "Middas" },
				{ "sDayThursday", "Turdas" },
				{ "sDayFriday", "Fredas" },
				{ "sDaySaturday", "Loredas" },
			} };
			const auto& [setting, fallback] = days[a_dayOfWeek % 7];
			return GameString(setting, fallback);
		}

		std::string MonthName(std::uint32_t a_month)
		{
			static constexpr std::array<std::pair<const char*, const char*>, 12> months{ {
				{ "sMonthJanuary", "Morning Star" },
				{ "sMonthFebruary", "Sun's Dawn" },
				{ "sMonthMarch", "First Seed" },
				{ "sMonthApril", "Rain's Hand" },
				{ "sMonthMay", "Second Seed" },
				{ "sMonthJune", "Midyear" },
				{ "sMonthJuly", "Sun's Height" },
				{ "sMonthAugust", "Last Seed" },
				{ "sMonthSeptember", "Hearthfire" },
				{ "sMonthOctober", "Frostfall" },
				{ "sMonthNovember", "Sun's Dusk" },
				{ "sMonthDecember", "Evening Star" },
			} };
			const auto& [setting, fallback] = months[a_month % 12];
			return GameString(setting, fallback);
		}

		std::string OrdinalSuffix(int a_day)
		{
			// Same rule the game uses for its own date string.
			switch (a_day) {
			case 1:
			case 21:
			case 31:
				return GameString("sFirstOrdSuffix", "st");
			case 2:
			case 22:
				return GameString("sSecondOrdSuffix", "nd");
			case 3:
			case 23:
				return GameString("sThirdOrdSuffix", "rd");
			default:
				return GameString("sDefaultOrdSuffix", "th");
			}
		}

		// Today's weekday exactly as the game itself prints it (the date line at the bottom
		// of the map), so the arrival day can never disagree with the game's own display.
		std::uint32_t CurrentDayOfWeek(RE::Calendar& a_calendar)
		{
			std::array<char, 256> buffer{};
			a_calendar.GetTimeDateString(buffer.data(), static_cast<std::uint32_t>(buffer.size()), false);
			const std::string_view text{ buffer.data() };
			for (std::uint32_t i = 0; i < 7; ++i) {
				const auto name = DayName(i);
				if (!name.empty() && text.starts_with(name)) {
					return i;
				}
			}
			return a_calendar.GetDayOfWeek() % 7;  // fallback: GameDaysPassed-based
		}

		std::string ClockString(int a_minuteOfDay)
		{
			const int hour = a_minuteOfDay / 60;
			const int minute = a_minuteOfDay % 60;
			if (Settings::Current()->use24HourClock) {
				return std::format("{:02}:{:02}", hour, minute);
			}
			const int hour12 = hour % 12 == 0 ? 12 : hour % 12;
			return std::format("{}:{:02} {}", hour12, minute, hour < 12 ? "AM" : "PM");
		}
	}

	float GetFastTravelSpeedMult()
	{
		if (auto gmst = RE::GameSettingCollection::GetSingleton()) {
			if (auto setting = gmst->GetSetting("fFastTravelSpeedMult")) {
				const float value = setting->GetFloat();
				if (value > 0.0f) {
					return value;
				}
			}
		}
		return 1.0f;
	}

	float GetTimescale()
	{
		if (auto calendar = RE::Calendar::GetSingleton(); calendar && calendar->timeScale) {
			const float value = calendar->GetTimescale();
			if (value > 0.0f) {
				return value;
			}
		}
		return 20.0f;
	}

	void Estimator::LoadCalibration()
	{
		std::lock_guard guard{ _lock };
		_samples.clear();

		const auto path = CalibrationPath();
		if (!path) {
			return;
		}
		std::ifstream file{ *path };
		if (!file) {
			logs::info("No calibration yet; using seed speed {}", Settings::Current()->seedSpeed);
			return;
		}
		std::string line;
		while (std::getline(file, line)) {
			if (!line.starts_with("speed=")) {
				continue;
			}
			try {
				const double value = std::stod(line.substr(6));
				if (value >= MIN_PLAUSIBLE_SPEED && value <= MAX_PLAUSIBLE_SPEED) {
					_samples.push_back(value);
				}
			} catch (...) {
			}
		}
		while (_samples.size() > MAX_SAMPLES) {
			_samples.pop_front();
		}
		logs::info("Loaded {} calibration sample(s); effective speed {:.1f}", _samples.size(), SpeedLocked());
	}

	void Estimator::SaveCalibrationLocked() const
	{
		const auto path = CalibrationPath();
		if (!path) {
			return;
		}
		std::ofstream file{ *path, std::ios::trunc };
		if (!file) {
			logs::warn("Could not write calibration file {}", path->string());
			return;
		}
		file << "# Fast Travel Time Info - learned travel speeds (game units per real second).\n";
		file << "# Delete this file to reset calibration.\n";
		for (const auto speed : _samples) {
			file << std::format("speed={:.3f}\n", speed);
		}
	}

	void Estimator::ResetCalibration()
	{
		std::lock_guard guard{ _lock };
		_samples.clear();
		_pending.reset();
		if (const auto path = CalibrationPath()) {
			std::error_code ec;
			std::filesystem::remove(*path, ec);
		}
		logs::info("Calibration reset");
	}

	CalibrationStatus Estimator::GetStatus() const
	{
		std::lock_guard guard{ _lock };
		return CalibrationStatus{ SpeedLocked(), _samples.size() };
	}

	double Estimator::SpeedLocked() const
	{
		if (_samples.empty()) {
			return Settings::Current()->seedSpeed;
		}
		std::vector<double> sorted(_samples.begin(), _samples.end());
		std::ranges::sort(sorted);
		const auto n = sorted.size();
		return n % 2 == 1 ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
	}

	std::optional<TripEstimate> Estimator::Estimate(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to) const
	{
		std::lock_guard guard{ _lock };

		const double distance = Distance(a_from, a_to);
		const double speed = SpeedLocked();
		const double mult = GetFastTravelSpeedMult();
		const double timescale = GetTimescale();
		if (speed <= 0.0 || mult <= 0.0 || timescale <= 0.0) {
			return std::nullopt;
		}

		TripEstimate result;
		result.distance = distance;
		result.hours = distance / (speed * mult) * timescale / 3600.0;
		result.calibrated = !_samples.empty();
		return result;
	}

	void Estimator::ArmTrip(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		std::lock_guard guard{ _lock };
		_pending = PendingTrip{ a_from, a_to, GetFastTravelSpeedMult(), GetTimescale(), std::chrono::steady_clock::now() };
		logs::debug("Trip armed: {:.0f} units", Distance(a_from, a_to));
	}

	void Estimator::OnFastTravelEnd(float a_hours)
	{
		std::lock_guard guard{ _lock };
		if (!_pending) {
			return;  // carriage ride or a fast travel not started from the world map
		}
		const auto trip = *_pending;
		_pending.reset();

		logs::info("Fast travel ended: game reports {:.3f} h", a_hours);

		if (!Settings::Current()->autoCalibrate) {
			return;
		}
		if (std::chrono::steady_clock::now() - trip.armedAt > PENDING_TIMEOUT) {
			logs::info("Ignoring sample: trip was armed too long ago");
			return;
		}

		// If the player clearly arrived somewhere other than the marker we tracked
		// (and has actually moved), trust the player's real position instead.
		auto to = trip.to;
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			const auto pos = player->GetPosition();
			if (Distance(pos, trip.to) > MIN_SAMPLE_DISTANCE && Distance(pos, trip.from) > MIN_SAMPLE_DISTANCE) {
				logs::info("Arrival point differs from tracked marker; using player position");
				to = pos;
			}
		}

		const double distance = Distance(trip.from, to);
		if (distance < MIN_SAMPLE_DISTANCE || a_hours <= 0.01f) {
			logs::info("Ignoring sample: trip too short ({:.0f} units, {:.3f} h)", distance, a_hours);
			return;
		}

		const double speed = distance * trip.timescale / (3600.0 * static_cast<double>(a_hours) * trip.mult);
		if (speed < MIN_PLAUSIBLE_SPEED || speed > MAX_PLAUSIBLE_SPEED) {
			logs::info("Ignoring implausible sample: speed {:.1f}", speed);
			return;
		}

		_samples.push_back(speed);
		while (_samples.size() > MAX_SAMPLES) {
			_samples.pop_front();
		}
		SaveCalibrationLocked();

		logs::info("Calibration sample: {:.0f} units, {:.3f} h, mult {}, timescale {} -> speed {:.1f}; effective speed now {:.1f} ({} sample(s))",
			distance, a_hours, trip.mult, trip.timescale, speed, SpeedLocked(), _samples.size());
	}

	std::string FormatDuration(double a_hours, bool a_approx)
	{
		const auto  settings = Settings::Current();
		const auto& s = *settings;
		const auto  totalMinutes = std::max<long long>(1, std::llround(a_hours * 60.0));
		const auto  days = totalMinutes / 1440;
		const auto  hours = (totalMinutes % 1440) / 60;
		const auto  minutes = totalMinutes % 60;

		std::vector<std::string> parts;
		if (days > 0) {
			parts.push_back(std::format("{} {}", days, s.dayUnit));
		}
		if (hours > 0) {
			parts.push_back(std::format("{} {}", hours, s.hourUnit));
		}
		if (minutes > 0 || parts.empty()) {
			parts.push_back(std::format("{} {}", minutes, s.minuteUnit));
		}

		std::string out = a_approx ? s.approxPrefix : std::string{};
		for (std::size_t i = 0; i < parts.size(); ++i) {
			if (i > 0) {
				out += ' ';
			}
			out += parts[i];
		}
		return out;
	}

	std::string FormatArrival(double a_hours)
	{
		auto calendar = RE::Calendar::GetSingleton();
		if (!calendar) {
			return {};
		}

		const double nowHour = calendar->GetHour();
		auto         day = static_cast<int>(calendar->GetDay());
		auto         month = calendar->GetMonth() % 12;
		auto         year = calendar->GetYear();
		const auto   startYear = year;
		const auto   dayOfWeek = CurrentDayOfWeek(*calendar);

		const auto arrivalMinutes = std::llround((nowHour + a_hours) * 60.0);
		const auto daysForward = arrivalMinutes / 1440;
		const auto minuteOfDay = static_cast<int>(arrivalMinutes % 1440);

		for (long long i = 0; i < daysForward; ++i) {
			if (++day > RE::Calendar::DAYS_IN_MONTH[month]) {
				day = 1;
				if (++month >= 12) {
					month = 0;
					++year;
				}
			}
		}

		const auto  settings = Settings::Current();
		const auto& s = *settings;
		std::string out = std::format("{}, {}", DayName(static_cast<std::uint32_t>((dayOfWeek + daysForward) % 7)), ClockString(minuteOfDay));
		if (s.showArrivalDate) {
			out += std::format(", {}{}{}{}", day, OrdinalSuffix(day), s.monthJoiner, MonthName(month));
			if (year != startYear) {
				out += std::format(", 4E {}", year);
			}
		}
		return out;
	}

	std::string FormatDistance(double a_units)
	{
		const double metres = a_units * METRES_PER_UNIT;
		if (metres < 1000.0) {
			return std::format("{:.0f} m", metres);
		}
		return std::format("{:.1f} km", metres / 1000.0);
	}
}
