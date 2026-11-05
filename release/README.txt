================================================================================
  LIKE A DRAGON: INFINITE WEALTH VR  (experimental, unfinished)
================================================================================

READ THIS FIRST

  This mod does NOT work in a VR headset yet. It makes the game engine render
  real stereo 3D, shown on your monitor as a side-by-side image (left eye in
  the left half, right eye in the right half). Getting that image into a
  headset is the unfinished part.

  It only works on the exact Steam build of the game it was made for. It does
  not check the game version; on another build it will most likely crash.

WHAT YOU NEED

  - Like a Dragon: Infinite Wealth (Steam, PC).
  - ReShade 6.7.1 WITH FULL ADD-ON SUPPORT, from https://reshade.me
    (the download labelled "with full add-on support", not the standard one).
    It is not included with this mod.

INSTALLING

  1. Close the game.

  2. Find the game's executable folder. In Steam, right click the game, then
     Manage, then Browse local files. From there open  runtime\media
     (the folder that contains LikeADragon8.exe).

  3. Run the ReShade 6.7.1 full add-on support installer, pick
     LikeADragon8.exe in that folder, and choose DirectX 10/11/12. ReShade
     installs itself there as dxgi.dll.

  4. Copy LAD8VR.addon64 (from the builds folder) into the same runtime\media
     folder, next to dxgi.dll.

  5. Start the game normally from Steam. The ReShade overlay (Home key) should
     list "LAD8VR native stereo (v37)" on its Add-ons tab.

USING IT

  Load a save and wait until you are actually in gameplay. Then:

  F9    arms native stereo. Press it ONCE, in gameplay only, never on the
        title or loading screens. A second press is ignored. To turn stereo
        off, quit the game.
  F10   turns the eye separation (64 mm) on or off. It starts OFF, so press
        F10 after F9 to get depth.
  F11   cycles the HUD mode: DUPLICATE (default, HUD in both eyes),
        HIDE (no HUD), NORMAL (game's own HUD, unmodified).

  The keys are read through ReShade's effect runtime. If F9 does nothing,
  check that ReShade's effects have not been toggled off.

  The HUD takes a few frames to settle after arming and when a battle or menu
  first opens. The minimap only appears in the left eye; that is known.

  For the cleanest result, turn off DLSS/FSR/XeSS, frame generation, motion
  blur, SSAO, reflections and depth of field. The mod was only tested that way.

LOGS

  %LOCALAPPDATA%\LAD8VR\LAD8VR_Stereo.txt
  %LOCALAPPDATA%\LAD8VR\LAD8VR_Stereo_Crash.txt  (only if something faulted)

UNINSTALLING

  Delete LAD8VR.addon64 from runtime\media. To remove ReShade as well, run its
  installer again and choose Uninstall, or delete dxgi.dll and the ReShade
  files it created in that folder.
