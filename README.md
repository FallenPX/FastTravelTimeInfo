# Fast Travel Time Info

An SKSE plugin for Skyrim Special Edition / Anniversary Edition. When you hover the cursor over a location on the world map, the marker's flyout gains two extra lines:

```
Whiterun
Travel time: 3 h 5 min
Arrival: Tirdas, 16:45, 18th of Last Seed
```

The estimate uses your current position, the game's own fast-travel speed setting (`fFastTravelSpeedMult`) and your current **timescale**, so it stays correct at 20, 10, 8, 1 or any other value.

No ESP, no MCM, no SkyUI dependency. It only needs the two frameworks almost every modded setup already has:

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

The plugin has to be compiled into `FastTravelTimeInfo.dll`. GitHub can do this for free on its own Windows machines:

1. Sign in to <https://github.com> (a free account is enough).
2. Click **+** (top right) → **New repository**. Name it `FastTravelTimeInfo`, choose Public or Private, and click **Create repository**.
3. On the new repository's page, click **uploading an existing file**. Drag in **everything inside** this folder (`src`, `dist`, `.github`, `xmake.lua`, `README.md`, `.gitignore`) and click **Commit changes**.
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

Edit `FastTravelTimeInfo.ini` (in Vortex: right-click the mod → *Open in File Manager*). Options include 24-hour or 12-hour clock, whether to show the date, an optional straight-line distance line in metres or kilometres, and the label text.

## If the lines do not appear

Check `Documents\My Games\Skyrim Special Edition\SKSE\FastTravelTimeInfo.log`:

- **"Hooked MapMenu::AdvanceMovie"** missing → SKSE or Address Library is not installed or is the wrong version.
- **"Map marker flyout not found"** → another mod has replaced the world map interface file (`Interface\map.swf`), and its flyout works differently.

## Building locally (optional)

Windows with Visual Studio 2022 (C++ workload) and xmake 3.x:

```
git clone --depth 1 --branch v15.0.1 --recurse-submodules --shallow-submodules https://github.com/alandtse/CommonLibSSE-NG.git lib/commonlibsse-ng
xmake config -m releasedbg --yes
xmake build --yes
```

## Credits

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) (GPL-3.0-or-later with plugin exceptions)
- The SkyUI team, whose reconstructed vanilla interface source documents how the world-map marker flyout works.
