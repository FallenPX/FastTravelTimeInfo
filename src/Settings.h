#pragma once

namespace FTI
{
	// User options, read from Data/SKSE/Plugins/FastTravelTimeInfo.ini.
	// Every value has a safe default, so a missing or partial INI is fine.
	//
	// The live settings are an immutable snapshot that is swapped as a whole, so the
	// in-game menu (render thread) can change them while the map menu (UI thread) reads them.
	struct Settings
	{
		bool  enabled{ true };
		bool  use24HourClock{ true };
		bool  showArrivalDate{ true };
		bool  showDistance{ false };
		bool  onlyFastTravelMarkers{ true };
		bool  autoCalibrate{ true };
		float seedSpeed{ 80.0f };  // game units per real-time second, used until calibrated
		int   lineSpacing{ 100 };  // flyout line spacing, percent of the game's own (100 = unchanged)

		std::string travelLabel{ "Travel time" };
		std::string arrivalLabel{ "Arrival" };
		std::string distanceLabel{ "Distance" };
		std::string approxPrefix{ "~" };
		std::string dayUnit{ "d" };
		std::string hourUnit{ "h" };
		std::string minuteUnit{ "min" };
		std::string monthJoiner{ " of " };

		static constexpr float MIN_SEED_SPEED = 1.0f;
		static constexpr float MAX_SEED_SPEED = 10000.0f;
		static constexpr int   MIN_LINE_SPACING = 50;
		static constexpr int   MAX_LINE_SPACING = 150;

		// The settings currently in use. Cheap; safe to call from any thread.
		[[nodiscard]] static std::shared_ptr<const Settings> Current();
		// Increases every time the live settings are replaced.
		[[nodiscard]] static std::uint32_t Version();
		// Replaces the live settings.
		static void Set(Settings a_settings);

		// Defaults overlaid with whatever the INI contains.
		[[nodiscard]] static Settings LoadFromIni();
		// LoadFromIni() + Set().
		static void Reload();
		// Writes the given settings into the INI. Comments, layout and commented-out lines
		// are kept; only values that actually differ are changed. Returns false on failure.
		static bool SaveToIni(const Settings& a_settings);
	};
}
