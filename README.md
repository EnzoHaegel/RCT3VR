# RCT3 VR

**Ride the attractions of RollerCoaster Tycoon 3 in virtual reality.**

🇫🇷 [Version française](LISEZ-MOI.md)

When you open a ride camera (coaster view), the image goes to your VR headset in true stereo 3D and you look around by turning your head. The rest of the game is unchanged and played on the monitor as usual.

No game file is modified: the mod is a `d3d9.dll` placed next to `RCT3.exe` and works in memory while the game runs.

## Requirements

- **RollerCoaster Tycoon 3 Complete Edition** (tested with Epic Games; Steam and GOG ship the same game)
- Windows 10 or 11, a recent graphics card
- A PC VR headset with one of:
  - **Virtual Desktop** (Quest, Pico…), tested
  - **SteamVR** (Index, Vive, Quest through SteamVR…)
  - **Meta Quest Link** (Meta Quest app)

## Install

1. Download `RCT3VR-x.y.z.zip` from the [latest release](https://github.com/EnzoHaegel/RCT3VR/releases/latest).
2. Extract the **whole** zip.
3. Double-click **`Installer.bat`**. It finds the game (Epic, Steam, GOG) or asks for the folder that contains `RCT3.exe`.
   If Windows warns about a downloaded file: *More info* → *Run anyway*.

Running `Installer.bat` again updates the mod and keeps your settings.

## Play

1. Put the headset on and start streaming (Virtual Desktop, Quest Link, SteamVR…).
2. Launch the game and load a park.
3. Open a ride camera: **VR switches on by itself**, and off when you leave the camera.

## Controls

| Input | Action |
|---|---|
| **A** (right controller, lower button) | Next camera |
| **B** (right controller, upper button) | Leave the ride camera |
| **Right thumbstick click** | Recenter the view |
| Right thumbstick, outside VR (overhead park view) | Move around the park |
| **F8** | Recenter the view |
| **F9** | Turn the mod off / on |

X and Y (left controller) are free: assign them in `rct3vr.ini`. Controller buttons press game keys for you, so the game window must have focus (click into it if a button does nothing).

Game keys you can assign to a button: `N` next camera · `M` previous camera · `B` camera type · `Shift+N` next ride · `Ctrl+C` leave the ride camera.

## Settings

`rct3vr.ini` in the game folder (open with Notepad):

| Setting | Default | Effect |
|---|---|---|
| `[Controller] ButtonA/B/X/Y`, `RightStick`, `LeftStick` | `N`, `Ctrl+C`, –, –, `Recenter`, – | A game key combo, `Recenter`, or empty. Example: `ButtonX=M` |
| `WorldScale` | `1.0` | World size (bigger = you feel smaller) |
| `GameFov` | `140` | Lower it if distant detail disappears, raise it if objects vanish at the sides |
| `MSAA` | `4` | Anti-aliasing: 0, 2, 4 or 8 |
| `RenderScale` | `1.0` | Resolution (1.0 = headset default) |
| `AutoEnable` | `1` | `0` = VR always on, F9 toggles it |
| `ToggleKey` / `RecenterKey` | `0x78` / `0x77` | Keyboard keys ([virtual-key codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)) |
| `ForceWindowed` | `1` | Borderless window (otherwise the game freezes when it loses focus). `0` = classic fullscreen |

## Troubleshooting

- **Nothing in the headset**: start VR streaming *before* opening the ride camera.
- **Stutter**: open `rct3vr.log` in the game folder; `GPU texture sharing available` means the fast path works.
- **Antivirus warning about `d3d9.dll`** (as with ReShade): that is the mod.
- Asking for help? Attach `rct3vr.log`.

## Uninstall

Double-click **`Uninstall.bat`** (or `Installer.bat` → *Uninstall*). The game is exactly as before; a previous `d3d9.dll` (ReShade…) is put back.

## Build from source

Requires Visual Studio 2022 with the C++ workload (CMake and Ninja included). The game is 32-bit, so the mod is too.

```bat
git clone --recursive https://github.com/EnzoHaegel/RCT3VR.git
cd RCT3VR
build.cmd
```

- `build.cmd` → `build\d3d9.dll`
- `build.cmd install` → also copies it into the game folder (path set at the top of `build.cmd`)
- `package.cmd` → `dist\RCT3VR-<version>.zip`, ready to share (version in `src/version.h`)

`build/` also contains test tools: `xrtest.exe` (test pattern in the headset, no game needed) and `sharetest.exe` (checks D3D9 ↔ D3D11 texture sharing).

---

Unofficial mod, not affiliated with Frontier or Atari.
