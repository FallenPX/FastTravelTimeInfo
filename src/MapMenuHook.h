#pragma once

namespace FTTI_API
{
	struct TripInfo;
}

namespace FTI::MapMenuHook
{
	// Hooks MapMenu::AdvanceMovie and registers the menu / fast-travel event sinks.
	void Install();

	// Estimate for a fast travel from the player's position to a_destination, with the same
	// rules as the map flyout (including the worldspace check). Used by the plugin interface.
	bool EstimateTripTo(const RE::TESObjectREFR& a_destination, FTTI_API::TripInfo& a_out);
}
