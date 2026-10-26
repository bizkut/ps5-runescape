# RuneScape on PS5 (Jagex Launcher, Old School RuneScape, RuneLite)

Unofficial PS5 homebrew that runs the **official Windows Jagex Launcher**, the
**official Old School RuneScape (OSRS) client** and **RuneLite** on a jailbroken
PS5, using a PS5 port of Wine, a Vulkan graphics stack and a controller mapping
made for RuneScape. Title ID `PPSA99225`.

> **Prerelease.** This is the first public build. It is experimental and has been
> tested on a console only within the limits listed under
> [What works and known limits](#what-works-and-known-limits).

## What you need

- A **jailbroken PS5** that can run homebrew titles from `/data/homebrew`.
- A **Jagex account**. You sign in inside the official launcher.
- An internet connection. Nothing from Jagex is included in the download: the
  official Jagex Launcher is downloaded from Jagex on the first start, and the
  launcher then installs OSRS (and RuneLite, if you choose it) itself.
- A controller (DualSense), optionally a USB keyboard and mouse.

## Install

1. Download `PPSA99225.zip` (and its `.sha256` file) from the
   [latest release](https://github.com/bizkut/ps5-runescape/releases) and check the
   checksum.
2. Unpack it so the folder **`PPSA99225`** ends up at `/data/homebrew/PPSA99225`
   on the console (for example by FTP, or by whatever file transfer your
   homebrew loader provides). To update, replace that folder with the new one.
3. Start **RuneScape Launcher (PS5)** from your homebrew loader. The first start
   downloads and sets up the Jagex Launcher, which takes a while. Sign in with
   your Jagex account and press **Play**; the game starts in its own process.

Your data lives in **`/data/runescape`** (Wine prefix, launcher and game files,
settings). It is separate from the app folder, so updating the app keeps it.
This folder is deliberately world-readable and writable so FTP and other PS5
apps can reach it; it contains launcher session data, so only run apps you trust
and **never share it** or unreviewed logs/screenshots from it. The whole folder
can be deleted over FTP. Its short-lived `handoff` subfolder (Play request
files) is also open, but each file is readable only by the title; other apps can
delete it but not read it.

## Controls

The controller mapping is the same file for two situations ("profiles"):

- **Launcher profile** – the Jagex Launcher window.
- **OSRS profile** – the game itself, for both the official OSRS client and
  RuneLite. It is adapted from Brigsby Bear's Steam Input layout for OSRS
  ([Workshop item 3148826194](https://steamcommunity.com/sharedfiles/filedetails/?id=3148826194)).

### In the game (OSRS and RuneLite)

Holding **R2 half-way** (about 25 % pull) switches on a second *layer* of
bindings for the D-pad, listed separately below. A full pull of R2 is a normal
button press.

| Controller | Action |
| --- | --- |
| Right stick | Move the mouse pointer |
| R1 | Left click |
| R2, pulled fully | Right click |
| R3 (press right stick) | Ctrl + left click |
| Left stick | Arrow keys (camera); left/right is inverted |
| L3 (press left stick), tap | Tab |
| L3, hold ≥ 305 ms | Space |
| L1 | Mouse wheel up (zoom in), repeats while held |
| L2 | Mouse wheel down (zoom out), repeats while held |
| Cross | F4 |
| Circle | F5 |
| Square | F6 |
| Triangle | F7 |
| Triangle, double press | Toggle Shift on/off (stays held until toggled again). A single press sends F7 after a short delay (250 ms) to tell the two apart |
| Touchpad, left half | F2 |
| Touchpad, right half | F3 |
| Options, tap | F1 |
| Options, hold ≥ 305 ms | Space |
| Create, tap | F8 |
| Create, double press | Escape |
| D-pad Up / Down | Up arrow / Down arrow |
| D-pad Right | Left arrow |
| D-pad Left | Right arrow |
| R2 half-pulled + D-pad Up | F9 |
| R2 half-pulled + D-pad Right | F10 |
| R2 half-pulled + D-pad Left | F11 |
| R2 half-pulled + D-pad Down | F12 |
| L3 + R3 together | On-screen keyboard (text entry) |

The touchpad does not move the pointer in the game. The F-keys are the keys
OSRS and RuneLite use for their side-panel tabs by default, so rebind them in
the client's own settings if you change its keys. F12 is not taken by the
launcher; the R2 + D-pad Down binding sends it to the client.

### In the launcher

| Controller | Action |
| --- | --- |
| Right stick | Move the mouse pointer |
| Touchpad (slide a finger) | Move the mouse pointer (relative) |
| Cross or R1 | Left click |
| R2, pulled fully | Right click |
| Circle | Escape |
| L3 + R3 together | On-screen keyboard |

Buttons not listed do nothing in the launcher. The title has no menu of its
own.

### Keyboard and mouse

A USB keyboard and mouse are passed straight to the client: keys are sent as
pressed, mouse movement, buttons and wheel work as on a PC. Every key,
including F12, goes to the client.

### Changing the controller mapping

On the first start the title writes the defaults to

```text
/data/runescape/pad-mapping.properties
```

Edit this file (for example by FTP) and **restart the title**; it is read once
at start. If you break it, the whole file is rejected (the launcher shows the
file and line; the game logs it) and nothing is overwritten. Delete the file to
get the defaults back. Format:

- First non-comment line `version=1`. Lines starting with `#` are comments. The
  file may be at most 16 KiB; lines at most 255 bytes.
- Every property starts with `profile.launcher.` or `profile.osrs.`.
- `button.<name>=<action>` binds a button. Add `.tap`, `.long` or `.double` to
  the name for a short press, long press or double press, for example
  `profile.osrs.button.options.long=key.space`. Two buttons joined by `+`
  (`button.l1+r1`) make a chord. `layer.r2.button.<name>` is a binding that only
  applies while the R2 layer is active.
- Button names: `cross`, `circle`, `square`, `triangle`, `l1`, `r1`, `l2`, `r2`,
  `l2.full`, `r2.full`, `l3`, `r3`, `create`, `options`, `dpad_up`, `dpad_down`,
  `dpad_left`, `dpad_right`, `touch.left`, `touch.right`.
- Actions: `key.<name>` with `a`–`z`, `0`–`9`, `f1`–`f12`, `enter`, `escape`,
  `backspace`, `tab`, `space`, `up`, `down`, `left`, `right`, `ctrl`, `shift`,
  `alt`, `super`, `home`, `end`, `delete`, `pageup`, `pagedown`; `mouse.left`,
  `mouse.right`, `mouse.middle`; `wheel.up`, `wheel.down`; `toggle.shift`;
  `ime` (on-screen keyboard). Combine keys and mouse buttons with
  `+` (for example `key.ctrl+mouse.left`).
- `stick.left` / `stick.right` = `off`, `mouse`, `arrows` or `wheel`, optionally
  followed by `,invert_x` and/or `,invert_y`.
- `touch.pointer=on|off` (touchpad moves the pointer), `threshold.long_ms`
  (10–5000, default 305), `threshold.double_ms` (10–2000, default 250),
  `layer.r2=<1–95>%` (how far R2 must be pulled for the layer, default 25 %).
- **L3 + R3 (on-screen keyboard) and Create + Options are reserved** and cannot
  be rebound or reused in other chords; Create + Options currently does nothing.

## RuneLite options

RuneLite is **not bundled**; you install it through the Jagex Launcher like on a
PC. When you press Play for RuneLite, the title starts it with
`--launch-mode=REFLECT --hw-accel=AUTO`. To change that, create
`/data/runescape/runelite.properties`:

```properties
launch_mode=REFLECT
hw_accel=AUTO
```

- `launch_mode`: `REFLECT` (default), `AUTO`, `JVM`, `FORK`.
- `hw_accel` (Java 2D acceleration): `AUTO` (default; on Windows this means
  `DIRECTDRAW`, the DXVK route), `OFF`, `DIRECTDRAW`, `OPENGL` (the WGL/Zink
  route). `OFF` may paint nothing visible on this console.
- The file is limited to 511 bytes, accepts `#` comments, and rejects unknown,
  duplicate or invalid settings. Options you pass to the launcher yourself win
  over the file. It applies at the next Play. This is separate from RuneLite's
  own GPU plugin, which stays at its default until you enable it. The official
  OSRS client does not read this file. See the
  [launcher configuration](https://github.com/runelite/runelite/wiki/RuneLite-Launcher-Configuration).

## What works and known limits

- The Jagex Launcher installs, signs in and starts OSRS, which is handed to a
  separate GPU process (Wine with WGL/Zink over a patched RADV, or DXVK). The
  controller layout above, the on-screen keyboard and fresh-cache start-up were
  checked on a console in bounded runs. Extended play sessions, complete game
  caches and returning from the game to the launcher are **not** qualified.
- RuneLite was checked on a console (game view, sidebar, pointer, clicks and
  sound); its GPU plugin is untested.
- Network name resolution beyond the ordinary path, and ICMP (ping) support used
  by the client, are not qualified. Game sound works in both clients; Discord
  integration is untested.
- **HDOS and RuneScape 3 are not supported yet.** Only the official OSRS client
  and RuneLite are targeted.
- Some menu entries and files come from the project this one is derived from
  (WoW/Battle.net features, add-on tooling) and are not part of RuneScape.
- It can break at any time after a game or launcher update.

## Building

Everything is built on Linux with Docker from pinned, public sources; see
[BUILDING.md](BUILDING.md). Release builds run in GitHub Actions from a tag.

## Legal

Not affiliated with, approved, endorsed or supported by Jagex, Sony Interactive
Entertainment, Blizzard Entertainment or Valve. RuneScape, Old School RuneScape
and Jagex are trademarks of their respective owners. No Jagex Launcher,
RuneScape client, game content or account credentials are distributed; obtain
proprietary software from its publisher and follow its terms. Using this does not
imply Jagex allows unofficial software with your account: you use it at your own
risk.

This project as a whole is licensed GPL-3.0-or-later (see `LICENSE`). It
combines open-source components with their own licences; see
[NOTICE.md](NOTICE.md) and `licenses/` in the release ZIP.

## Credits

Based on the PS5 Wine/Vulkan work of the Forever project
([bizkut/ps5-forever](https://github.com/bizkut/ps5-forever)) and
mihawk-99's PS5 port, with Wine, Proton, vkd3d-proton, DXVK and Mesa RADV and
the PS5 payload SDK. Controller layout after Brigsby Bear's Steam Input mapping
for OSRS. Artwork by the project owner.
