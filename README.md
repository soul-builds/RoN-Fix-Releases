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
switched on, and RoN-Fix can't be installed on it for now. Smart App Control decides anew for every version:
if it refuses a new version's files, RoN-Fix stays on the version you have, and Windows may show a "blocked"
notice a few seconds after the game starts. Nothing breaks; the notice is harmless.

## Back to the original game

- **For good:** open **RoN-Fix** from the Start menu and press **Restore original game**. Steam's
  *Properties > Installed Files > Verify integrity of game files* works too.
- **For one start:** hold **Shift** while the game starts, or add `-original` to the game's launch
  options in Steam.

## If the game doesn't start

Start it once more: if Windows refused part of RoN-Fix, RoN-Fix steps aside by itself and the game starts
as the original. If it still won't start, use Steam's *Verify integrity of game files* (see above).

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

## Lobby tools (when you host)

Only the host needs RoN-Fix for these; everyone else simply sees the player leave.

- **Boot Player really removes the player.** The game's own boot only emptied the slot and could leave a
  "ghost" behind. Now the booted player leaves the lobby for everyone and can't rejoin that lobby.
- **Shift + Boot Player bans.** The player is added to `ronfix-bans.txt` in the game folder and is removed
  whenever they join one of your lobbies again. You can also add PlayFab IDs or Steam IDs (64-bit numbers)
  to that file by hand, one per line.
- **Unban:** open **RoN-Fix** from the Start menu and press **Unban...**, pick the player and press **Unban**.
  It works within a few seconds, also while the game runs. Someone you removed from the room that is open
  right now can join again from your next room.
- **Ready-click spam:** a player who toggles ready 12 times within 5 seconds is removed.
- **Ghost rooms:** sometimes the game's **Create** gives up and drops you back in the Game Browser while your
  new room stays listed with you as host, so you can't host again. RoN-Fix leaves that ghost room for you a
  few seconds later; just press Create again.

These can be switched off under `[lobby]` in `ronfix.ini`.

## Shorter command delay (test, when you host)

The game never makes a turn shorter than 400 ms, or 600 ms with 8 players, however fast everyone's connection
is. That is how long a click takes to become an order. RoN-Fix can lower that to 200-300 ms:

- The host adds `ShortDelay=1` under `[netfix]` in `ronfix.ini` (in the game folder).
- It is only used when **every** player in the lobby has RoN-Fix 0.11 or later. Otherwise the game's own
  delay stays, so you can still play with anyone.
- Slow connections still get longer turns, as before.

## Better graphics (removed in 0.13)

RoN-Fix 0.12's setup had a **Better graphics** option, made with [ReShade](https://reshade.me) (by crosire) and
the SweetFX shaders (by CeeJay.dk). With Steam's in-game overlay switched on it could stop the game from
starting, so 0.13 removes it. If you ticked it in 0.12, open **RoN-Fix** from the Start menu and press
**Reinstall** once: that takes it out. Automatic updates don't.

## Logs (optional)

The setup asks once whether to send RoN-Fix's own logs to help fix lag, disconnects and crashes.
Player names and IDs are replaced with codes before anything is stored. Details:
[PRIVACY.md](PRIVACY.md). You can change your answer any time in RoN-Fix (Start menu).

## Source code

RoN-Fix's own code is open source under the MIT licence:
[soul-builds/RoN-Fix-Source](https://github.com/soul-builds/RoN-Fix-Source). Releases are built from it on GitHub,
and anyone can rebuild them and get byte-identical files.

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by
[SignPath Foundation](https://signpath.org/).

- Committers and reviewers: [soul-builds](https://github.com/soul-builds)
- Approvers: [soul-builds](https://github.com/soul-builds)
- Privacy policy: [PRIVACY.md](PRIVACY.md)

Status: applying to SignPath Foundation (November 2026). Releases up to 0.13 are not code-signed; signing starts
with the first release after the project is accepted.
