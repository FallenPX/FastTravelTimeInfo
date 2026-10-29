#pragma once

namespace FTI
{
	struct TripEstimate
	{
		double hours{ 0.0 };     // in-game hours the fast travel is expected to take
		double distance{ 0.0 };  // straight-line distance in game units
		bool   calibrated{ false };
	};

	struct CalibrationStatus
	{
		double      speed{ 0.0 };  // game units per real-time second currently in use
		std::size_t samples{ 0 };  // learned trips behind it (0 = still the INI starting guess)
	};

	// Estimates fast-travel duration from straight-line distance, the game setting
	// fFastTravelSpeedMult and the TimeScale global:
	//
	//   hours = distance / (speed * fFastTravelSpeedMult) * TimeScale / 3600
	//
	// "speed" starts at the INI seed value and is then learned from real trips:
	// after every map fast travel the game reports how many hours actually passed
	// (TESFastTravelEndEvent), and the plugin solves the formula above for speed.
	class Estimator
	{
	public:
		static Estimator& Get()
		{
			static Estimator singleton;
			return singleton;
		}

		void LoadCalibration();

		[[nodiscard]] std::optional<TripEstimate> Estimate(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to) const;

		// Called when the player confirms a fast travel from the world map.
		void ArmTrip(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);
		// Called when the game reports the end of a fast travel.
		void OnFastTravelEnd(float a_hours);

		// Forgets every learned trip and deletes the calibration file.
		void ResetCalibration();
		[[nodiscard]] CalibrationStatus GetStatus() const;

	private:
		[[nodiscard]] double SpeedLocked() const;
		void                 SaveCalibrationLocked() const;

		struct PendingTrip
		{
			RE::NiPoint3                          from;
			RE::NiPoint3                          to;
			float                                 mult{ 1.0f };
			float                                 timescale{ 20.0f };
			std::chrono::steady_clock::time_point armedAt;
		};

		mutable std::mutex         _lock;
		std::deque<double>         _samples;  // learned speeds, newest last
		std::optional<PendingTrip> _pending;
	};

	[[nodiscard]] float GetFastTravelSpeedMult();
	[[nodiscard]] float GetTimescale();

	[[nodiscard]] std::string FormatDuration(double a_hours, bool a_approx);
	[[nodiscard]] std::string FormatArrival(double a_hours);
	[[nodiscard]] std::string FormatDistance(double a_units);
}
