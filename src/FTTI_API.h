#pragma once

// Fast Travel Time Info - interface for other SKSE plugins
// ---------------------------------------------------------
// FTTI hands this interface to Fast Travel Arrival Planner (FTAP) through SKSE's
// messaging between plugins: when every plugin has loaded (kPostLoad), FTTI sends one
// message of type MESSAGE_INTERFACE to the plugin named PLANNER_NAME, whose data is a
// pointer to an Interface that stays valid for the whole session.
//
// Rules for changing this file: only ever ADD members at the end of Interface and raise
// INTERFACE_VERSION. Never reorder or remove members, so an older planner keeps working
// with a newer FTTI. The same file is copied into FTAP's source.

#include <cstdint>

namespace RE
{
	class TESObjectREFR;
}

namespace FTTI_API
{
	inline constexpr const char*   PLUGIN_NAME = "FastTravelTimeInfo";
	inline constexpr const char*   PLANNER_NAME = "FastTravelArrivalPlanner";
	inline constexpr std::uint32_t MESSAGE_INTERFACE = 0x46545449;  // 'FTTI'
	inline constexpr std::uint32_t INTERFACE_VERSION = 3;
	inline constexpr std::uint32_t FLYOUT_STYLE_VERSION = 2;  // first version with GetFlyoutStyle
	inline constexpr std::uint32_t FLYOUT_TEXT_VERSION = 3;   // first version with GetFlyoutText

	struct TripInfo
	{
		bool   valid{ false };            // an estimate exists
		bool   calibrated{ false };       // learned from real trips (false = still the starting guess)
		bool   otherWorldspace{ false };  // the destination is in another worldspace: no estimate
		double hours{ 0.0 };              // estimated in-game hours at the current fFastTravelSpeedMult
		double distance{ 0.0 };           // straight-line distance in game units
		float  speedMult{ 1.0f };         // the fFastTravelSpeedMult the estimate used
	};

	// How the world map's marker flyout looks, so another panel can match it (version 2).
	struct FlyoutStyle
	{
		bool          colorsKnown{ false };       // read from the flyout this session
		std::uint32_t titleColor{ 0xFFFFFF };     // 0xRRGGBB: the location name
		std::uint32_t labelColor{ 0xBBBBBB };     // row labels, such as "Travel time:"
		std::uint32_t valueColor{ 0xFFFFFF };     // row values
		bool          background{ true };         // FTTI draws a background behind the flyout
		std::uint32_t backgroundColor{ 0x000000 };
		float         backgroundOpacity{ 0.6f };  // 0 to 1
		float         cornerRadius{ 3.0f };       // in the map's own pixels (1280 x 720 layout)
	};

	// How the flyout's text is drawn: font, on-screen size and shadow (version 3). Only ever
	// add members at the end: GetFlyoutText copies no more than the caller's size.
	// Fast Travel Time Info 0.3.5 and later no longer measure this (the planner reads the
	// flyout's look itself): known is always false.
	struct FlyoutText
	{
		bool          known{ false };            // read from the flyout this session
		char          titleFont[64]{};           // font face after the game's font configuration ("" = unknown)
		char          rowFont[64]{};
		float         titleSize{ 0.0f };         // line height in screen pixels, which is how ImGui sizes fonts (0 = unknown)
		float         rowSize{ 0.0f };
		bool          shadow{ false };           // the text has a drop shadow or glow
		std::uint32_t shadowColor{ 0x000000 };   // 0xRRGGBB
		float         shadowAlpha{ 0.0f };       // 0 to 1
		float         shadowDistance{ 0.0f };    // screen pixels (0 = a glow around the text)
		float         shadowAngle{ 45.0f };      // degrees: 0 = right, 90 = down
		float         shadowBlur{ 0.0f };        // screen pixels
		bool          shadowKnown{ false };      // FTTI 0.3.1+: a shadow was found (0.3.2+: false means "could not tell")
	};

	struct Interface
	{
		std::uint32_t version;        // INTERFACE_VERSION of the FTTI that sent it
		std::uint32_t size;           // sizeof(Interface) on the FTTI side
		const char*   pluginVersion;  // e.g. "0.1.4"

		// Estimate for a fast travel from the player's current position to a map marker
		// reference. Returns a_out->valid.
		bool (*GetTripTo)(RE::TESObjectREFR* a_destination, TripInfo* a_out);

		// The next fast travel confirmed on the world map is not used for calibration.
		// The request expires after two minutes if no fast travel follows.
		void (*ExcludeNextTrip)();

		// Text exactly as FTTI shows it, using FTTI's own settings (clock style, units, date).
		// Both return the length written (0 on failure); the text is always 0-terminated.
		std::uint32_t (*FormatDuration)(double a_hours, bool a_approx, char* a_buffer, std::uint32_t a_size);
		std::uint32_t (*FormatArrival)(double a_hoursFromNow, char* a_buffer, std::uint32_t a_size);

		// ---- Version 2 ----

		// The flyout's colours (read from the map while the game runs) and FTTI's background
		// settings. Returns a_out->colorsKnown; the background fields are always filled.
		bool (*GetFlyoutStyle)(FlyoutStyle* a_out);

		// ---- Version 3 ----

		// The flyout's font, text sizes and shadow. Since 0.3.5 always "unknown" (see
		// FlyoutText). a_size is sizeof(FlyoutText) on the caller's side. Returns a_out->known.
		bool (*GetFlyoutText)(FlyoutText* a_out, std::uint32_t a_size);
	};
}
