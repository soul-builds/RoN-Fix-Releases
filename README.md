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

This repository holds RoN-Fix's own source code, under the [MIT licence](LICENSE). Every release is built from
it by GitHub Actions, and anyone can rebuild a release and get byte-identical files.

### How it works

RoN-Fix doesn't change the game's files on disk except the "large address aware" flag (4 GB of memory).
Instead it puts two small stand-ins next to the game and keeps the originals beside them:

| File in the game folder | Built from | What it does |
|---|---|---|
| `CrossplayNetLib.dll` | `proxy/loader.c` | Loader. Forwards everything to the game's own `CrossplayNetLib_orig.dll`, loads `ronfix-core.dll`, and runs the updater. If Windows refuses the core, or the player holds Shift at start, the game runs as the original. |
| `ronfix-core.dll` | `proxy/netproxy.c` (+ `steamlink.c`, `lobbyfix.c`, `ghostroom.c`, `turndelay.c`) | The fixes. Each checks the exact game build first and switches itself off on any other build. |
| `ronfix-update.dll` | `proxy/update.c` | Updater. Fetches the release manifest, checks its signature and every file's SHA-256, and installs only what changed. |
| `dssl.dll` | `proxy/sndproxy.c` | Sound stand-in. Fixes the crash on quit, then forwards to the game's `dssl_orig.dll`. |
| `ronfix\RoN-Fix.exe` | `setup/setup.c` | Setup and the Start menu app: install, restore the original game, unban players. |

`logserver/` is the log service: a Cloudflare Worker that receives logs from players who agreed to send them,
and replaces names and IDs with codes before anything is stored.

### Building

You need [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (release 20261104, i686 target) and Windows
PowerShell. Set `LLVM_MINGW` to its folder, or put it in `%USERPROFILE%\Tools\llvm-mingw`.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-setup.ps1 -Release 99 -Name test -NoSign
```

This builds the four DLLs into `proxy\build\` and a complete release, setup included, into `dist\release-99\`.
The same source always gives byte-identical files, with every file carrying version information (product
"RoN-Fix", version = the release name).

### Tests

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-thunk-tests.ps1
```

- **What it covers:** the core, the updater (including an update over HTTP from a local server) and the log
  upload. GitHub Actions runs them on every push.
- **The setup test** (`tools\test-setup.ps1`) needs a Steam copy of the game, so it runs only on a developer PC.

### Releases

The [release workflow](.github/workflows/release.yml) runs on a GitHub-hosted Windows runner:

1. It builds the four DLLs from this repository, and SignPath code-signs them.
2. It builds the setup around the signed DLLs, because the setup installs them, and SignPath signs the setup.
3. It lists every file's SHA-256 in `manifest.txt`.
4. It creates the GitHub release as a draft, with every file attached and a link to the run.

The maintainer then runs `tools\finish-release.ps1`, which:

- checks the draft's files against the manifest;
- signs `manifest.txt` with the update key (ECDSA P-256);
- adds `manifest.txt.sig`, the only file not made by the workflow;
- publishes the release.

The update key never goes to GitHub: an installed copy only accepts files listed in a manifest signed by one of
the keys in `proxy/update_keys.h`. So even a stolen GitHub login cannot push an update to players.

### What is not here

The comments mention analysis notes (`reverse/...`). Those notes, the game's decompiled code and anything
derived from the game's data stay private, because they contain material from the game itself. This
repository holds only RoN-Fix's own code.

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by
[SignPath Foundation](https://signpath.org/).

- Committers and reviewers: [soul-builds](https://github.com/soul-builds)
- Approvers: [soul-builds](https://github.com/soul-builds)
- Privacy policy: [PRIVACY.md](PRIVACY.md)

Status: applied to SignPath Foundation in November 2026. Releases up to 0.13 are not code-signed; signing starts
with the first release after the project is accepted.

## Licence

RoN-Fix's own code is under the [MIT licence](LICENSE). Rise of Nations and its files belong to their owners
and are not part of this repository.
