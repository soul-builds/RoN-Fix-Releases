# RoN-Fix

Unofficial fixes for **Rise of Nations: Extended Edition** on Steam. A fan project, not affiliated
with or endorsed by Microsoft or the game's developers.

**Status:** in testing with one group of players. The download will appear under
[Releases](../../releases) once the one-click installer is ready.

## What it fixes

- The game sometimes freezing on startup before its window appears.
- The crash when quitting the game.
- Several crashes during matches and in lobbies.
- Memory: lets the game use 4 GB instead of 2 GB (the same change as the well-known "4GB patch",
  which Steam updates remove).
- Frozen players: a player whose game data stops arriving is reported as lost after 20 seconds,
  so the match can drop them and carry on. This only works when everyone in the match has RoN-Fix,
  because the game drops a player only once every other player has reported them.
- Optional logging of lobbies and match timing, to track down lag and disconnects.

## Compatibility

- You can still play with people who don't have RoN-Fix, in their lobbies and yours.
- Made for the current Steam build. If Steam updates the game to a build RoN-Fix doesn't know,
  RoN-Fix switches itself off and the game runs as normal.
- Uninstalling restores the original files. Steam's "Verify integrity of game files" does too.
