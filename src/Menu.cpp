#include "pch.h"

#include "Menu.h"
#include "Estimator.h"
#include "Settings.h"

// SKSEMenuFramework.h includes <windows.h>, so it is kept to this one file.
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include "SKSEMenuFramework.h"

// Optional in-game settings page for SKSE Menu Framework.
//
// The framework's header looks the framework up at runtime (GetModuleHandle/GetProcAddress),
// so this plugin never links to it: without the framework, Register() returns early and
// nothing else in this file ever runs.
//
// Every change is applied to the live settings at once and written to the INI, keeping the
// INI the single source of truth for the next game start.

namespace FTI::Menu
{
	namespace
	{
		constexpr auto SECTION_NAME = "Fast Travel Time Info";
		constexpr auto PAGE_NAME = "Settings";
		constexpr double MESSAGE_SECONDS = 4.0;
		constexpr double PREVIEW_HOURS = 3.0 + 5.0 / 60.0;

		struct TextFieldDef
		{
			const char*              label;
			const char*              tooltip;
			std::string Settings::*  member;
			bool                     required;  // labels may not be empty
			std::array<char, 128>    buffer{};
		};

		std::array<TextFieldDef, 8> g_textFields{ {
			{ "Travel time label", "Name of the travel time row.", &Settings::travelLabel, true },
			{ "Arrival label", "Name of the arrival row.", &Settings::arrivalLabel, true },
			{ "Distance label", "Name of the distance row (only shown when distance is switched on).", &Settings::distanceLabel, true },
			{ "Estimate prefix", "Shown in front of the travel time until the first calibration, e.g. \"~\".", &Settings::approxPrefix, false },
			{ "Day unit", "Used for trips longer than 24 game hours, e.g. \"d\" or \"day\".", &Settings::dayUnit, false },
			{ "Hour unit", "e.g. \"h\" or \"hr\".", &Settings::hourUnit, false },
			{ "Minute unit", "e.g. \"min\".", &Settings::minuteUnit, false },
			{ "Month joiner", "Text between the day and the month. \" of \" gives \"18th of Last Seed\"; \" \" gives \"18th Last Seed\".", &Settings::monthJoiner, false },
		} };

		float         g_seedBuffer{ 80.0f };
		int           g_spacingBuffer{ 100 };
		int           g_opacityBuffer{ 60 };
		std::uint32_t g_syncedVersion{ 0 };
		bool          g_confirmReset{ false };
		std::string   g_message;
		double        g_messageUntil{ 0.0 };

		void ShowMessage(std::string a_text)
		{
			g_message = std::move(a_text);
			g_messageUntil = ImGuiMCP::GetTime() + MESSAGE_SECONDS;
		}

		// Text boxes keep their own buffers while typing; refresh them whenever the live
		// settings were replaced (by a commit here or by "Reload INI").
		void SyncBuffers(const Settings& a_settings)
		{
			const auto version = Settings::Version();
			if (version == g_syncedVersion) {
				return;
			}
			g_syncedVersion = version;
			for (auto& field : g_textFields) {
				const auto& value = a_settings.*(field.member);
				field.buffer.fill('\0');
				// (std::min) in brackets: <windows.h> may define a min() macro.
				std::memcpy(field.buffer.data(), value.data(), (std::min)(value.size(), field.buffer.size() - 1));
			}
			g_seedBuffer = a_settings.seedSpeed;
			g_spacingBuffer = a_settings.lineSpacing;
			g_opacityBuffer = a_settings.backgroundOpacity;
		}

		std::string Sanitize(const char* a_text)
		{
			std::string out;
			for (const char* p = a_text; *p; ++p) {
				if (*p != '"' && *p != '\r' && *p != '\n' && *p != '\t') {
					out += *p;
				}
			}
			return out;
		}

		void Commit(Settings a_settings)
		{
			Settings::Set(std::move(a_settings));
			if (!Settings::SaveToIni(*Settings::Current())) {
				ShowMessage("Applied, but could not save to FastTravelTimeInfo.ini.");
			}
		}

		void Tooltip(const char* a_text)
		{
			if (ImGuiMCP::IsItemHovered()) {
				ImGuiMCP::SetTooltip("%s", a_text);
			}
		}

		bool Toggle(const char* a_label, const char* a_tooltip, const Settings& a_current, bool Settings::* a_member)
		{
			bool value = a_current.*a_member;
			const bool changed = ImGuiMCP::Checkbox(a_label, &value);
			Tooltip(a_tooltip);
			if (changed) {
				Settings next = a_current;
				next.*a_member = value;
				Commit(std::move(next));
			}
			return changed;
		}

		void RenderDisplay(const Settings& a_s)
		{
			ImGuiMCP::SeparatorText("Display");
			Toggle("Show travel time and arrival on the world map", "Master switch for the extra flyout lines.", a_s, &Settings::enabled);
			ImGuiMCP::BeginDisabled(!a_s.enabled);
			Toggle("24-hour clock", "On: 16:45. Off: 4:45 PM.", a_s, &Settings::use24HourClock);
			Toggle("Show the date on the arrival line", "e.g. \"Tirdas, 16:45, 18th of Last Seed\".", a_s, &Settings::showArrivalDate);
			Toggle("Show straight-line distance", "Adds a distance row in metres or kilometres.", a_s, &Settings::showDistance);
			Toggle("Only on markers you can fast travel to", "Off: also show estimates on undiscovered locations.", a_s, &Settings::onlyFastTravelMarkers);

			ImGuiMCP::SetNextItemWidth(220.0f);
			ImGuiMCP::SliderInt("Line spacing", &g_spacingBuffer, Settings::MIN_LINE_SPACING, Settings::MAX_LINE_SPACING, "%d%%");
			const bool spacingCommitted = ImGuiMCP::IsItemDeactivatedAfterEdit();
			Tooltip("Spacing between the location name and the rows below it, in percent of the game's own. 100% = unchanged, lower = tighter. Ctrl+click to type a value.");
			if (spacingCommitted) {
				Settings next = a_s;
				next.lineSpacing = std::clamp(g_spacingBuffer, Settings::MIN_LINE_SPACING, Settings::MAX_LINE_SPACING);
				Commit(std::move(next));
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Default##spacing")) {
				Settings next = a_s;
				next.lineSpacing = 100;
				Commit(std::move(next));
			}

			Toggle("Dark background behind the flyout", "A translucent dark panel behind the location name and rows, for easier reading on bright parts of the map.", a_s, &Settings::showBackground);
			ImGuiMCP::BeginDisabled(!a_s.showBackground);
			ImGuiMCP::SetNextItemWidth(220.0f);
			ImGuiMCP::SliderInt("Background opacity", &g_opacityBuffer, 0, 100, "%d%%");
			const bool opacityCommitted = ImGuiMCP::IsItemDeactivatedAfterEdit();
			Tooltip("0% = invisible, 100% = solid black. Ctrl+click to type a value.");
			if (opacityCommitted) {
				Settings next = a_s;
				next.backgroundOpacity = std::clamp(g_opacityBuffer, 0, 100);
				Commit(std::move(next));
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Default##opacity")) {
				Settings next = a_s;
				next.backgroundOpacity = Settings{}.backgroundOpacity;
				Commit(std::move(next));
			}
			ImGuiMCP::EndDisabled();
			ImGuiMCP::EndDisabled();
		}

		void RenderText(const Settings& a_s)
		{
			ImGuiMCP::SeparatorText("Text");
			ImGuiMCP::TextDisabled("Changes are applied when you press Enter or click elsewhere.");
			for (std::size_t i = 0; i < g_textFields.size(); ++i) {
				auto& field = g_textFields[i];
				ImGuiMCP::PushID(static_cast<int>(i));
				ImGuiMCP::SetNextItemWidth(220.0f);
				ImGuiMCP::InputText(field.label, field.buffer.data(), field.buffer.size());
				const bool committed = ImGuiMCP::IsItemDeactivatedAfterEdit();
				Tooltip(field.tooltip);
				ImGuiMCP::PopID();

				if (committed) {
					auto value = Sanitize(field.buffer.data());
					Settings next = a_s;
					if (field.required && value.empty()) {
						value = Settings{}.*(field.member);  // a label cannot be empty
						ShowMessage(std::format("{} cannot be empty; restored the default.", field.label));
					}
					next.*(field.member) = std::move(value);
					Commit(std::move(next));
				}
			}
		}

		void RenderPreview(const Settings& a_s)
		{
			ImGuiMCP::SeparatorText("Preview");
			auto calendar = RE::Calendar::GetSingleton();
			if (!calendar || !calendar->gameHour || !calendar->gameDay || !calendar->gameMonth || !calendar->gameYear || !calendar->gameDaysPassed) {
				ImGuiMCP::TextDisabled("Available once a game is loaded.");
				return;
			}
			const auto status = Estimator::Get().GetStatus();
			ImGuiMCP::TextDisabled("A 3 h 5 min trip starting now would show:");
			ImGuiMCP::BulletText("%s: %s", a_s.travelLabel.c_str(), FormatDuration(PREVIEW_HOURS, status.samples == 0).c_str());
			ImGuiMCP::BulletText("%s: %s", a_s.arrivalLabel.c_str(), FormatArrival(PREVIEW_HOURS).c_str());
			if (a_s.showDistance) {
				ImGuiMCP::BulletText("%s: %s", a_s.distanceLabel.c_str(), "12.3 km");
			}
		}

		void RenderCalibration(const Settings& a_s)
		{
			ImGuiMCP::SeparatorText("Calibration");
			Toggle("Learn from my fast travels", "Compares each map fast travel with the time the game actually recorded, and adjusts.", a_s, &Settings::autoCalibrate);

			const auto status = Estimator::Get().GetStatus();
			if (status.samples > 0) {
				ImGuiMCP::Text("Calibrated from %u recent trip(s).", static_cast<unsigned>(status.samples));
			} else {
				ImGuiMCP::Text("Not calibrated yet: using the starting guess.");
			}
			ImGuiMCP::TextDisabled("Speed in use: %.1f units/s   Timescale: %.1f   fFastTravelSpeedMult: %.2f",
				status.speed, static_cast<double>(GetTimescale()), static_cast<double>(GetFastTravelSpeedMult()));

			ImGuiMCP::SetNextItemWidth(120.0f);
			ImGuiMCP::InputFloat("Starting guess (units/s)", &g_seedBuffer, 0.0f, 0.0f, "%.1f");
			const bool seedCommitted = ImGuiMCP::IsItemDeactivatedAfterEdit();
			Tooltip("Travel speed used until the first calibration. The default of 80 suits most setups.");
			if (seedCommitted) {
				Settings next = a_s;
				next.seedSpeed = std::clamp(g_seedBuffer, Settings::MIN_SEED_SPEED + 1.0f, Settings::MAX_SEED_SPEED - 1.0f);
				Commit(std::move(next));
			}

			if (!g_confirmReset) {
				if (ImGuiMCP::Button("Reset calibration")) {
					g_confirmReset = true;
				}
				Tooltip("Forget all learned trips, e.g. after installing a mod that changes fast-travel speed.");
			} else {
				ImGuiMCP::Text("Forget all learned trips?");
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("Yes, reset")) {
					Estimator::Get().ResetCalibration();
					g_confirmReset = false;
					ShowMessage("Calibration reset. Your next fast travel recalibrates it.");
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("Cancel")) {
					g_confirmReset = false;
				}
			}
		}

		void RenderFile()
		{
			ImGuiMCP::SeparatorText("Settings file");
			ImGuiMCP::TextWrapped("%s", "Changes made here are saved to Data\\SKSE\\Plugins\\FastTravelTimeInfo.ini straight away.");
			if (ImGuiMCP::Button("Reload INI")) {
				Settings::Reload();
				ShowMessage("Settings reloaded from FastTravelTimeInfo.ini.");
			}
			Tooltip("Use this after editing the INI while the game is running.");
		}

		void __stdcall Render()
		{
			try {
				const auto settings = Settings::Current();
				SyncBuffers(*settings);

				RenderDisplay(*settings);
				ImGuiMCP::Spacing();
				RenderText(*Settings::Current());
				ImGuiMCP::Spacing();
				RenderPreview(*Settings::Current());
				ImGuiMCP::Spacing();
				RenderCalibration(*Settings::Current());
				ImGuiMCP::Spacing();
				RenderFile();

				if (!g_message.empty() && ImGuiMCP::GetTime() < g_messageUntil) {
					ImGuiMCP::Spacing();
					ImGuiMCP::TextWrapped("%s", g_message.c_str());
				}
			} catch (const std::exception& e) {
				logs::error("Menu render failed: {}", e.what());
			} catch (...) {
				logs::error("Menu render failed: unknown exception");
			}
		}
	}

	void Register()
	{
		if (REL::Module::IsVR()) {
			return;
		}
		if (!SKSEMenuFramework::IsInstalled()) {
			logs::info("SKSE Menu Framework not installed; settings stay INI-only");
			return;
		}
		if (!GetMenuFrameworkModule()) {
			logs::warn("SKSE Menu Framework is installed but not loaded; settings stay INI-only");
			return;
		}
		SKSEMenuFramework::SetSection(SECTION_NAME);
		SKSEMenuFramework::AddSectionItem(PAGE_NAME, Render);
		logs::info("Added settings page to SKSE Menu Framework {:.2f}", static_cast<double>(SKSEMenuFramework::GetMenuFrameworkVersion()));
	}
}
