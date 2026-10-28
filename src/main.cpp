#include "pch.h"

#include "Estimator.h"
#include "MapMenuHook.h"
#include "Settings.h"

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (a_message && a_message->type == SKSE::MessagingInterface::kDataLoaded) {
			FTI::Estimator::Get().LoadCalibration();
			FTI::MapMenuHook::Install();
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logs::info("Fast Travel Time Info {} loading", SKSE::GetPluginVersion().string());

	if (REL::Module::IsVR()) {
		logs::warn("Skyrim VR is not supported; the plugin will stay inactive");
		return true;
	}

	FTI::Settings::Get().Load();
	if (!FTI::Settings::Get().enabled) {
		logs::info("Disabled in the INI (bEnabled=0)");
		return true;
	}

	const auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
		logs::error("Could not register for SKSE messages");
		return false;
	}
	return true;
}
