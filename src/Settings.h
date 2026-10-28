#pragma once

namespace FTI
{
	// User options, read from Data/SKSE/Plugins/FastTravelTimeInfo.ini.
	// Every value has a safe default, so a missing or partial INI is fine.
	struct Settings
	{
		bool  enabled{ true };
		bool  use24HourClock{ true };
		bool  showArrivalDate{ true };
		bool  showDistance{ false };
		bool  onlyFastTravelMarkers{ true };
		bool  autoCalibrate{ true };
		float seedSpeed{ 80.0f };  // game units per real-time second, used until calibrated

		std::string travelLabel{ "Travel time" };
		std::string arrivalLabel{ "Arrival" };
		std::string distanceLabel{ "Distance" };
		std::string approxPrefix{ "~" };
		std::string dayUnit{ "d" };
		std::string hourUnit{ "h" };
		std::string minuteUnit{ "min" };
		std::string monthJoiner{ " of " };

		static Settings& Get()
		{
			static Settings singleton;
			return singleton;
		}

		void Load();
	};
}
