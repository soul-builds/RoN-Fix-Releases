# RoN-Fix privacy policy

RoN-Fix can send its own log files to its developer, to find and fix lag, disconnects and crashes in
Rise of Nations: Extended Edition. **It only does this if you choose "Send logs" in the RoN-Fix setup.**
You can change your answer at any time: open RoN-Fix from the Start menu, or set `Consent=0` under
`[upload]` in `ronfix.ini` in the game folder.

## What is sent

The new parts of RoN-Fix's own log files (`ronfix-net.log`, `ronfix-update.log`, `ronfix-sound.log`), at
most 1 MB of each per game start:

- lobby events: players joining and leaving, ready clicks, boots, lobby updates;
- match timing: command delay, and per player ping, waiting time, lag and frame time;
- loading-screen timing: when each player finished each loading step;
- problems RoN-Fix caught (crashes it prevented, missing players), and update results;
- RoN-Fix's version, and a random install code that groups uploads from the same installation.

## What is not sent

Your files, saved games, chat messages, passwords, or anything that is not in RoN-Fix's own log files.
RoN-Fix's log service does not store your IP address.

## Names and IDs

The logs mention players by name and by Steam and PlayFab ID, including other players in your lobbies.
Before an upload leaves your PC, RoN-Fix removes the game folder and your Windows profile folder from the
text, so your Windows user name never leaves your PC. The upload travels encrypted (HTTPS) to RoN-Fix's
log service on Cloudflare. **Before anything is stored**, the service replaces every player name and every
Steam or PlayFab ID with a code such as `P-3fa91c07d2`. The same player always gets the same code, so a
problem can be followed across games, but the name or ID cannot be read back from the code.

## Other network traffic

RoN-Fix also does two things that send no logs and no personal data:

- **Update checks:** about 20 seconds after the game starts, RoN-Fix downloads its update list from GitHub
  (github.com/soul-builds/RoN-Fix-Releases), and new files from there when there is a new version. GitHub sees
  your IP address, as with any download. To switch this off, untick "Keep RoN-Fix up to date" when you install, or
  set `Enabled=0` under `[update]` in `ronfix.ini`.
- **Connection checks with other players:** in a lobby or match, RoN-Fix sends small test messages over Steam's
  network to the other players in the same game who also run RoN-Fix. The messages contain RoN-Fix's version
  number, counters and timings, and which player the game is waiting for. They measure whether Steam's network
  would carry the game better. To switch this off, set `Shadow=0` under `[steam]` in `ronfix.ini`.

## Who sees it, and for how long

Only the RoN-Fix developer. Uploads are deleted after 180 days. Nothing is sold or shared.

## Questions

Open an issue at https://github.com/soul-builds/RoN-Fix-Releases/issues
