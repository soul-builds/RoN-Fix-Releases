# RoN-Fix

Unofficial fixes for **Rise of Nations: Extended Edition** on Steam. A fan project, not affiliated
with or endorsed by Microsoft or the game's developers.

## Install (about a minute)

1. Download **RoN-Fix-Setup.exe** from the [latest release](../../releases/latest).
2. Close Rise of Nations, then double-click the file.
3. Windows may show **"Windows protected your PC"**. Click **More info**, then **Run anyway**.
   RoN-Fix is not code-signed yet, and Windows warns about every unsigned program.
4. Press **Install**. That's it: start the game from Steam as usual.

New versions download by themselves while you play and are used from the next start.

If Windows says the app was **blocked** and offers no "Run anyway", your PC has *Smart App Control*
switched on, and RoN-Fix can't be installed on it for now.

## Back to the original game

- **For good:** open **RoN-Fix** from the Start menu and press **Restore original game**. Steam's
  *Properties > Installed Files > Verify integrity of game files* works too.
- **For one start:** hold **Shift** while the game starts, or add `-original` to the game's launch
  options in Steam.

## What it fixes

- The game sometimes freezing on startup before its window appears.
- The crash when quitting the game.
- Several crashes during matches and in lobbies.
- Memory: lets the game use 4 GB instead of 2 GB (the same change as the well-known "4GB patch",
  which Steam updates remove).
- Frozen players: a player whose game data stops arriving is reported as lost after 20 seconds,
  so the match can drop them and carry on. This only works when everyone in the match has RoN-Fix,
  because the game drops a player only once every other player has reported them.
- Logging of lobbies and match timing, to track down lag and disconnects.

## Compatibility

- You can still play with people who don't have RoN-Fix, in their lobbies and yours.
- Made for the current Steam build. If Steam updates the game to a build RoN-Fix doesn't know,
  RoN-Fix switches itself off and the game runs as normal.

## Logs (optional)

The setup asks once whether to send RoN-Fix's own logs to help fix lag, disconnects and crashes.
Player names and IDs are replaced with codes before anything is stored. Details:
[PRIVACY.md](PRIVACY.md). You can change your answer any time in RoN-Fix (Start menu).
