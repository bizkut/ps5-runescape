# Catalog notes

## Record description (`apps/PPSA99225.json`, `description`)

Jagex Launcher on PS5 for Old School RuneScape, with the official client or RuneLite; press L3+R3 for the on-screen keyboard.

## Release notes (GitHub release description, shown by the catalog)

### 01.000.022

- A fresh install now reaches a working launcher: the launcher restarts
  automatically after the first-start window timeout, and the out-of-memory
  crash at sign-in is fixed.
- The "process capacity reached" error is fixed.
- `/data/runescape` can now be deleted completely over FTP and the title
  reinstalled without restarting the PS5 (the Play handoff folder is no longer
  private; its session files stay unreadable by other apps).
- The Jagex Launcher download is faster (parallel download).
- RuneLite mouse pointer fix.
- Docs: the README controls table is corrected; the title has no Create+Options
  menu.

Known limits: the first install takes about 9-10 minutes; RuneScape 3 and HDOS
are not supported.

### 01.000.021 — first public RuneScape prerelease

Unofficial, experimental PS5 homebrew (title `PPSA99225`) that runs the official
Jagex Launcher, the official Old School RuneScape client and RuneLite on a
jailbroken PS5 through Wine. **No Jagex files are included**: the official Jagex
Launcher is downloaded from Jagex on the first start, and you sign in with your
own Jagex account.

- Jagex Launcher start-up, sign-in and OSRS install/launch, with Play handing
  the game to a separate GPU process (Wine WGL/Zink over RADV, or DXVK).
- RuneLite, installed through the Jagex Launcher, with `launch_mode` and
  `hw_accel` selectable in `/data/runescape/runelite.properties`.
- A DualSense mapping for OSRS (mouse, clicks, camera, zoom, F-keys, on-screen
  keyboard), editable in `/data/runescape/pad-mapping.properties`; the full
  keybind tables are in the README.
- USB keyboard and mouse pass through to the client.
- Persistent data in `/data/runescape`; app folder `/data/homebrew/PPSA99225`.

Known limits: extended play sessions, complete game caches and return to the
launcher are not qualified; DNS beyond the normal path and ICMP are not
qualified; game sound works in both clients; Discord is untested; HDOS and RuneScape 3 are not
supported yet; some inherited WoW/Battle.net menu entries remain. Not affiliated
with or endorsed by Jagex or Sony Interactive Entertainment.
