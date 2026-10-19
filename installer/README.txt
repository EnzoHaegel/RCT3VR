RCT3 VR — RollerCoaster Tycoon 3 ride cameras in virtual reality
================================================================

When you ride an attraction (coaster cam / ride camera), the view goes to your VR
headset: true stereo 3D, look around by turning your head. The rest of the game is
unchanged and played on the monitor as usual.

REQUIREMENTS
- RollerCoaster Tycoon 3 Complete Edition (tested with the Epic Games version; the
  Steam and GOG versions normally ship the same game).
- Windows 10 or 11, a recent graphics card.
- A PC VR headset with one of:
    * Virtual Desktop (Quest, Pico...) — tested;
    * SteamVR (Index, Vive, Quest through SteamVR...);
    * Meta Quest Link (Meta Quest app).

INSTALL
1. Extract the whole zip.
2. Double-click Installer.bat. It finds the game (Epic, Steam, GOG) or asks for the
   folder that contains RCT3.exe.
   Windows may warn about a downloaded file: "More info" then "Run anyway".

USE
1. Put the headset on and start streaming (Virtual Desktop, Quest Link, SteamVR...).
2. Start the game as usual and load a park.
3. Open a ride camera: VR switches on by itself, and off when you leave the camera.

Controllers (in VR):
- A (right controller, lower button): next camera
- B (right controller, upper button): leave the ride camera
- Right thumbstick click: recenter the view
Controller outside VR (overhead park view):
- Right thumbstick: move around the park (up, left, down, right)
Keyboard: F8 recenters the view, F9 turns the mod off / on.

Controller buttons press the game's keys for you: the game window must have focus
(click into it if a button does nothing).

SETTINGS — rct3vr.ini in the game folder (open with Notepad)
- [Controller]: what each button does. Game default keys: N next camera, M previous
  camera, B camera type, Shift+N next ride, Ctrl+C leave. Example: ButtonX=M
- WorldScale: world size (bigger = you feel smaller).
- GameFov: field of view the game uses to decide what to draw (140 by default). Lower
  it if distant detail disappears, raise it if objects vanish at the sides when you
  turn your head.
- MSAA: anti-aliasing (0, 2, 4 or 8). RenderScale: resolution (1.0 = headset default).
- AutoEnable=0: VR stays on all the time (F9 toggles it).

THE GAME RUNS IN A BORDERLESS WINDOW
On purpose (otherwise it freezes whenever it loses focus). For classic fullscreen set
ForceWindowed=0 in rct3vr.ini.

TROUBLESHOOTING
- Nothing in the headset: start VR streaming BEFORE opening the ride camera.
- Stutter: check rct3vr.log in the game folder; "GPU texture sharing available" means
  the fast transfer works.
- Antivirus warnings about d3d9.dll (as with ReShade): that is the mod.
- Attach rct3vr.log when asking for help.

UNINSTALL
Double-click Uninstall.bat (or Installer.bat, then "Uninstall"). The game is exactly
as before; a previous d3d9.dll (ReShade...) is put back.

Unofficial mod, not affiliated with Frontier or Atari. No game file is modified:
everything happens in memory while the game runs.
