#include "pch.h"

#include "Api.h"
#include "Estimator.h"
#include "FTTI_API.h"
#include "MapMenuHook.h"

// The interface FTTI offers to Fast Travel Arrival Planner. Without the planner nothing
// here is ever called, and FTTI behaves exactly as before.

namespace FTI::Api
{
	namespace
	{
		std::string         g_versionText;
		FTTI_API::Interface g_interface{};

		std::uint32_t CopyOut(const std::string& a_text, char* a_buffer, std::uint32_t a_size)
		{
			if (!a_buffer || a_size == 0) {
				return 0;
			}
			const auto length = (std::min)(a_text.size(), static_cast<std::size_t>(a_size - 1));
			std::memcpy(a_buffer, a_text.data(), length);
			a_buffer[length] = '\0';
			return static_cast<std::uint32_t>(length);
		}

		bool GetTripTo(RE::TESObjectREFR* a_destination, FTTI_API::TripInfo* a_out)
		{
			if (!a_out) {
				return false;
			}
			*a_out = FTTI_API::TripInfo{};
			if (!a_destination) {
				return false;
			}
			try {
				return MapMenuHook::EstimateTripTo(*a_destination, *a_out);
			} catch (const std::exception& e) {
				logs::error("GetTripTo failed: {}", e.what());
			} catch (...) {
				logs::error("GetTripTo failed: unknown exception");
			}
			*a_out = FTTI_API::TripInfo{};
			return false;
		}

		void ExcludeNextTrip()
		{
			Estimator::Get().ExcludeNextTrip();
		}

		std::uint32_t FormatDurationText(double a_hours, bool a_approx, char* a_buffer, std::uint32_t a_size)
		{
			try {
				return CopyOut(FormatDuration(a_hours, a_approx), a_buffer, a_size);
			} catch (...) {
				return CopyOut({}, a_buffer, a_size);
			}
		}

		bool GetFlyoutStyle(FTTI_API::FlyoutStyle* a_out)
		{
			if (!a_out) {
				return false;
			}
			try {
				return MapMenuHook::GetFlyoutStyle(*a_out);
			} catch (...) {
				*a_out = FTTI_API::FlyoutStyle{};
				return false;
			}
		}

		bool GetFlyoutText(FTTI_API::FlyoutText* a_out, std::uint32_t a_size)
		{
			if (!a_out || a_size == 0) {
				return false;
			}
			FTTI_API::FlyoutText text;
			try {
				MapMenuHook::GetFlyoutText(text);
			} catch (...) {
				text = FTTI_API::FlyoutText{};
			}
			std::memcpy(a_out, &text, (std::min)(static_cast<std::size_t>(a_size), sizeof(text)));
			return text.known;
		}

				std::uint32_t FormatArrivalText(double a_hoursFromNow, char* a_buffer, std::uint32_t a_size)
		{
			try {
				return CopyOut(FormatArrival(a_hoursFromNow), a_buffer, a_size);
			} catch (...) {
				return CopyOut({}, a_buffer, a_size);
			}
		}
	}

	void SendToPlanner()
	{
		if (REL::Module::IsVR()) {
			return;
		}
		g_versionText = SKSE::GetPluginVersion().string();
		g_interface.version = FTTI_API::INTERFACE_VERSION;
		g_interface.size = sizeof(FTTI_API::Interface);
		g_interface.pluginVersion = g_versionText.c_str();
		g_interface.GetTripTo = GetTripTo;
		g_interface.ExcludeNextTrip = ExcludeNextTrip;
		g_interface.FormatDuration = FormatDurationText;
		g_interface.FormatArrival = FormatArrivalText;
		g_interface.GetFlyoutStyle = GetFlyoutStyle;
		g_interface.GetFlyoutText = GetFlyoutText;

		const auto messaging = SKSE::GetMessagingInterface();
		if (!messaging) {
			return;
		}
		if (messaging->Dispatch(FTTI_API::MESSAGE_INTERFACE, &g_interface, sizeof(g_interface), FTTI_API::PLANNER_NAME)) {
			logs::info("Interface (version {}) offered to Fast Travel Arrival Planner", FTTI_API::INTERFACE_VERSION);
		} else {
			logs::info("Fast Travel Arrival Planner is not installed");
		}
	}
}
