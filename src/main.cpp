#include "pch.h"

#include "Estimator.h"
#include "MapMenuHook.h"
#include "Menu.h"
#include "Settings.h"

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}
		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// Every SKSE plugin is loaded by now, so the optional SKSE Menu Framework
			// can be found regardless of plugin load order.
			FTI::Menu::Register();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			FTI::Estimator::Get().LoadCalibration();
			FTI::MapMenuHook::Install();
			break;
		default:
			break;
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

	// bEnabled is checked live, so it can also be switched on and off from the in-game menu.
	FTI::Settings::Reload();
	if (!FTI::Settings::Current()->enabled) {
		logs::info("Display is switched off in the INI (bEnabled=0)");
	}

	const auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
		logs::error("Could not register for SKSE messages");
		return false;
	}
	return true;
}
