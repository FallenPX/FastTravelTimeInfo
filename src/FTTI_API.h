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
	inline constexpr std::uint32_t INTERFACE_VERSION = 1;

	struct TripInfo
	{
		bool   valid{ false };            // an estimate exists
		bool   calibrated{ false };       // learned from real trips (false = still the starting guess)
		bool   otherWorldspace{ false };  // the destination is in another worldspace: no estimate
		double hours{ 0.0 };              // estimated in-game hours at the current fFastTravelSpeedMult
		double distance{ 0.0 };           // straight-line distance in game units
		float  speedMult{ 1.0f };         // the fFastTravelSpeedMult the estimate used
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
	};
}
