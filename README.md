# Fast Travel Time Info

An SKSE plugin for Skyrim Special Edition / Anniversary Edition. When you hover the cursor over a location on the world map, the marker's flyout gains two extra lines:

```
Whiterun
Travel time: 3 h 5 min
Arrival: Tirdas, 16:45, 18th of Last Seed
```

The estimate uses your current position, the game's own fast-travel speed setting (`fFastTravelSpeedMult`) and your current **timescale**, so it stays correct at 20, 10, 8, 1 or any other value.

No ESP, no MCM, no SkyUI dependency. It only needs the two frameworks almost every modded setup already has (SKSE Menu Framework is optional, see *Settings*):

- **SKSE64** matching your game version
- **Address Library for SKSE Plugins** (All in one)

## How accurate is it?

Bethesda never published the exact fast-travel formula; UESP notes that the game estimates the time from your movement speed and that the result is rounded. The plugin therefore **learns** the real number from your own game:

1. Before your first fast travel, the time is a starting guess and is shown with a leading `~` (for example `~3 h 5 min`).
2. Each time you fast travel from the map, the game reports how many hours actually passed. The plugin compares that with the distance travelled and adjusts its speed figure.
3. After one trip the `~` disappears. It keeps the last 15 trips and uses the middle value, so one odd trip cannot skew it.

The learned values are saved in `Documents\My Games\Skyrim Special Edition\SKSE\FastTravelTimeInfo_Calibration.txt`. Delete that file to start over (for example after installing a mod that changes fast-travel speed).

Carriage rides are not used for learning.

## Getting the DLL (no programming needed)

The plugin has to be compiled into `FastTravelTimeInfo.dll`. There are two ways: on your own PC (option A) or on GitHub's machines (option B). Both produce the same `FastTravelTimeInfo-mod.zip`.

### Option A: build on your own PC

**One-time setup** (needs an internet connection and roughly 15 GB of free disk space):

1. **Microsoft C++ build tools.** Go to <https://visualstudio.microsoft.com/downloads/>, scroll to *Tools for Visual Studio* and download **Build Tools for Visual Studio 2026**. Run it, tick **Desktop development with C++**, and click **Install**. Leave the ticked items on the right as they are.
2. **Git and xmake.** Open the Start menu, type `PowerShell`, open it, and paste these two lines (press Enter after each):
   ```
   winget install --id Git.Git -e --source winget
   winget install --id Xmake-io.Xmake -e
   ```
3. Sign out of Windows and back in, so the new tools are found.

**Building:**

1. Extract this project to a simple folder outside OneDrive, for example `C:\Mods\FastTravelTimeInfo`.
2. Double-click **`build.bat`**. If Windows asks whether to run it, choose **Run** / **More info → Run anyway**.
3. The first build downloads its libraries and compiles them, so it can take 10 to 30 minutes. Later builds take about a minute.
4. When it says **Done**, `FastTravelTimeInfo-mod.zip` is in the same folder. Install it as described under *Installing with Vortex*.

If it stops with an error, the window stays open. Copy the first lines that mention `error` and send them to whoever is helping you.

### Option B: build on GitHub

GitHub can do the same thing for free on its own Windows machines:

1. Sign in to <https://github.com> (a free account is enough).
2. Click **+** (top right) → **New repository**. Name it `FastTravelTimeInfo`, choose Public or Private, and click **Create repository**.
3. On the new repository's page, click **uploading an existing file**. Drag in **everything inside** this folder (including the `.github` folder) and click **Commit changes**.
4. Open the **Actions** tab. A run called **Build plugin** starts by itself and takes roughly 5 to 15 minutes. Wait for the green tick.
   - If the Actions tab shows nothing, the `.github` folder was probably skipped during upload. Click **Add file → Create new file**, type `.github/workflows/build.yml` as the name, paste in the contents of that file from this folder, and commit.
5. Click the finished run and download **FastTravelTimeInfo-mod** under *Artifacts*. That zip is the mod.

## Installing with Vortex

Drag `FastTravelTimeInfo-mod.zip` onto Vortex's **Mods** page, enable it and deploy. It places:

```
Data\SKSE\Plugins\FastTravelTimeInfo.dll
Data\SKSE\Plugins\FastTravelTimeInfo.ini
```

To uninstall, disable or remove it in Vortex. It does not touch save games.

## Settings

Options include 24-hour or 12-hour clock, whether to show the date, an optional straight-line distance line in metres or kilometres, the line spacing of the flyout, an optional translucent dark background behind it (with adjustable opacity and corner rounding), and the label text. There are two ways to change them:

- **INI file:** edit `FastTravelTimeInfo.ini` (in Vortex: right-click the mod → *Open in File Manager*). Changes take effect the next time you start the game.
- **In-game menu (optional):** if [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) is installed, a **Fast Travel Time Info → Settings** page appears in its menu. Changes there apply straight away and are saved to the INI automatically, so the INI always holds your current settings. The page also shows the calibration status, a live preview, a **Reset calibration** button and a **Reload INI** button (for INI edits made while the game is running).

SKSE Menu Framework is not required. Without it, the plugin works exactly the same and is configured through the INI only.

When the menu saves, it changes only the values you changed. Comments and lines you have disabled with `;` are left as they are.

## If the lines do not appear

Check `Documents\My Games\Skyrim Special Edition\SKSE\FastTravelTimeInfo.log`:

- **"Hooked MapMenu::AdvanceMovie"** missing → SKSE or Address Library is not installed or is the wrong version.
- **"Map marker flyout not found"** → another mod has replaced the world map interface file (`Interface\map.swf`), and its flyout works differently.

## For developers

`build.bat` runs the equivalent of:

```
git clone --depth 1 --branch v15.0.1 --recurse-submodules --shallow-submodules https://github.com/alandtse/CommonLibSSE-NG.git lib/commonlibsse-ng
xmake config -m releasedbg -a x64 --yes
xmake build --yes FastTravelTimeInfo
powershell -ExecutionPolicy Bypass -File tools\package.ps1
```

## Credits

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) (GPL-3.0-or-later with plugin exceptions)
- [spdlog](https://github.com/gabime/spdlog) (MIT), used through CommonLibSSE-NG for the log file
- [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) by Thiago099 / QTR-Modding: its API header is included in `extern/SKSEMenuFramework` (GPL-3.0) for the optional in-game menu
- The SkyUI team, whose reconstructed vanilla interface source documents how the world-map marker flyout works.

## Changelog

- **0.1.3**: The flyout background now sits behind the text (it covered it before); less space above the location name; adjustable corner rounding (`iBackgroundCornerRadius`, also in the in-game menu).
- **0.1.2**: Optional translucent dark background behind the map flyout, with adjustable opacity (`bShowBackground`, `iBackgroundOpacity`, also in the in-game menu).
- **0.1.1**: Adjustable line spacing for the map flyout (`iLineSpacing`, also in the in-game menu).
- **0.1.0**: Optional in-game settings page for SKSE Menu Framework (live changes, saved to the INI), calibration status and reset, Reload INI. `bEnabled` now also works while the game is running.
- **0.0.1**: First release.
