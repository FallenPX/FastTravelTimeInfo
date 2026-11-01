#pragma once

namespace FTI
{
	// User options. The player's own settings file is
	//   Documents/My Games/Skyrim Special Edition/SKSE/FastTravelTimeInfo.ini
	// (next to the log and the calibration file). It is created from the INI that comes with
	// the mod (Data/SKSE/Plugins/FastTravelTimeInfo.ini, the defaults) on first start, so
	// installing an update never replaces it; options a newer version adds are copied into it.
	// Every value has a safe default, so a missing or partial file is fine.
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
		bool  showBackground{ false };  // translucent dark panel behind the flyout text
		int   backgroundOpacity{ 60 };  // percent: 0 = invisible, 100 = solid
		int   backgroundCornerRadius{ 3 };  // pixels: 0 = square corners

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
		static constexpr int   MAX_CORNER_RADIUS = 12;

		// The settings currently in use. Cheap; safe to call from any thread.
		[[nodiscard]] static std::shared_ptr<const Settings> Current();
		// Increases every time the live settings are replaced.
		[[nodiscard]] static std::uint32_t Version();
		// Replaces the live settings.
		static void Set(Settings a_settings);

		// Where the player's settings file is.
		[[nodiscard]] static std::string FilePath();
		// Defaults overlaid with whatever the player's settings file contains.
		[[nodiscard]] static Settings LoadFromIni();
		// Creates the player's settings file if needed, adds options new in this version,
		// then LoadFromIni() + Set().
		static void Reload();
		// Writes the given settings into the player's settings file. Comments, layout and
		// commented-out lines are kept; only values that actually differ are changed.
		// Returns false on failure.
		static bool SaveToIni(const Settings& a_settings);
	};
}
