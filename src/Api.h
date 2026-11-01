#pragma once

namespace FTI::Api
{
	// Hands the plugin interface (FTTI_API.h) to Fast Travel Arrival Planner, if it is
	// installed. Call once every plugin has loaded (SKSE kPostLoad).
	void SendToPlanner();
}
