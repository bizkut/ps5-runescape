# Building RuneScape Launcher (PS5) on Linux

The development implementation includes verified native SSN acquisition, an
integrity-synced maintenance bootstrap and an OSRS Play→GPU-parent handoff with
Windows session-environment preservation. The GPU profile stages native
WGL/Zink and DXVK over RADV. Host build qualification is **not** Jagex/OSRS
graphics, login or gameplay qualification. Jagex startup does not stage or start
AddonForge, Battle.net or WoW.

OSRS handoff uses `/data/runescape/handoff`: a `0777` directory (so FTP can delete
the whole `/data/runescape` tree; created that way and repaired at startup) with
exclusive `0600` one-shot requests (others can delete them but not read them).
Both startup repair walkers set the directory itself to `0777` like the rest of the
data root, but never widen or scan the session files, which are also excluded from
native/Wine shared creation modes. Requests are deleted on
consumption, failed game LoadExec and stale-start cleanup. `/download0` is not
used. Native cross-LoadExec persistence remains separate from host filesystem
and codec tests. Automatic closure uses own-PID `sceKernelGetAppInfo` followed
by typed `sceSystemServiceKillApp`; `LoadExec("exit")` is not a PS5 self-close.

RuneScape's broker endpoint is build-time `WB_PORT=9142`, with its own
`/data/runescape/.writebroker-token`; Forever's default remains 9141. Rebuild
the native autostart, payload and Wine modules together after changing it.
Both native roles log authenticated readiness; each Wine process reports its
connection and its first fully acknowledged write separately.

The GPU role aspect-fits client-size BGRA8 frames to the native output through
Wine's shared fullscreen compute blit, with a RuneScape-only rectangle-mask
specialization. The desktop and the client's fixed/resizable preference are
not rewritten. Pointer feedback and absolute input use the same fit and live
client/DPI mapping; raw relative motion stays unchanged. Native-size frames
avoid the extra blit.

The normal host suite includes the geometry, command, specialization, input and
cleanup contracts. An additional real CPU-Vulkan pixel oracle can be run on a
Linux host with `libvulkan.so.1` and lavapipe:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  make -C ps5/probes/psmkit-wine test-title-viewport-vulkan HOST_CC=cc
```

It reads every output pixel, including bars and edges, from the embedded shared
shader; it is not PS5 scanout qualification. The commented GLSL in
`ps5-wine/dlls/win32u/vulkan.c` is the shader source; regeneration uses
`glslangValidator -V --target-env vulkan1.0` and `spirv-val --target-env vulkan1.0`.

## Source layout

`ps5/release/deps.json` pins source revisions, tracked-content fingerprints,
nested graphics sources and downloaded tool/archive hashes:

```text
SOURCE_ROOT/
  WORKSPACE.json
  PS5_Vulkan/                 # private writable pinned clone (build writes here)
  PS5_Mesa/                   # read-only pinned dependency
  PS5_PayloadSDK/              # read-only pinned dependency
  pacbrew-repo/                # read-only pinned dependency
  AddonForge/                  # inherited, pending removal
  ps5-proton/
    ps5-wine/                 # shared Wine runescape worktree
    ps5-runescape/             # this product
      vkd3d-proton/            # pinned nested graphics dependencies
      dxvk/
```

Dependency trees other than PS5_Vulkan may be symlinks to checkouts elsewhere;
treat them as read-only and do not build or fetch into them directly. The Docker
wrapper discovers such external source paths and mounts them read-only at their
resolved host paths so the symlinks work in the container. PS5_Vulkan is a separate
clone because SDK/native-tool setup writes its `.deps`, `build` and `runtime`
directories. Its source starts at the manifest pin; no Forever build outputs
were copied. Downloads, SDK exports, product outputs and `.cache` belong only to
the new root. Wine's worktree shares objects and refs, not working files; the
wrapper additionally exposes its common Git directory for generated-header
restoration, and the source root at its host path for worktree metadata.

On another machine, check out the product at the layout above, then run:

```sh
python3 ps5/release/fetch-sources.py /absolute/SOURCE_ROOT
```

Every pin is fetchable from a public URL. The sources that belong to this project
(Wine, PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK, pacbrew, vkd3d-proton and DXVK) are
pinned at commits that are also the tips of a branch named `runescape` in the
matching `bizkut/ps5-forever-*` repository, so fetching by commit ID succeeds.
Existing dependency checkouts must match their pins.
Source validation rejects modified tracked dependency files and mismatched
content fingerprints; the Wine lock must match the dependency manifest.

## Host checks (no console)

```sh
python3 ps5/title/tools/product-name.py
make -C ps5/probes/psmkit-wine test HOST_CC=clang-19 HOST_CXX=clang++-19
```

The suite needs a host C/C++ toolchain, Python 3, and llvm-mingw at
`$HOME/llvm-mingw` (or explicit `MINGW_CC` and `MINGW32_CC`). Source identities
can be checked with `python3 ps5/release/test-product-identity.py`.
These tests are host regressions, not evidence of Jagex/OSRS execution.

## Full build

From `SOURCE_ROOT/ps5-proton/ps5-runescape` on x86-64 Linux with Docker:

```sh
JOBS=24 bash ps5/release/build-linux.sh /path/to/SOURCE_ROOT 01.000.022
```

The version must match `ps5/title/sce_sys/param.json` and is the release tag
(`01.000.022` is the current public RuneScape prerelease).
`.github/workflows/release.yml` fetches the pinned sources and runs this same
build on a self-hosted Linux runner. To release, raise `contentVersion`, commit
and push, then open Actions → Release → Run workflow and tick **publish**: the
version defaults to `contentVersion`, the run refuses an existing tag, and on
success it creates the tag at the built commit and publishes the ZIP and its
`.sha256` (a pre-release unless **prerelease** is unticked). Without
**publish** it only builds and keeps the ZIP as a run artifact. Pushing a tag
`NN.NNN.NNN` still builds and publishes that tag. Expect a **multi-hour cold
build** (planning estimate: 2–4 hours; not measured for this fork). The pinned
container builds SDK/native tools, patched RADV, whisper/ggml, FreeType, GnuTLS,
Wine, vkd3d, DXVK, pinned PE64 Mesa WGL/Zink, both B1 roles and the archive. Host build
qualification is not console execution. Outputs live under `ps5/build/`, including
`release/PPSA99225.zip`, `release/logs/` and `release/logs/timings.tsv`.

`ps5/paths.sh` fixes the installed Wine path. Wine module SONAMEs embed the
absolute console title path, so PPSA99225 requires fresh Wine configuration and
rebuild (also FreeType/GnuTLS and staging); never reuse PPSA99030 binaries.
`ps5/include/product-wine.h` supplies the same broker root/title-ID defines to
Wine, native titles and the write broker. Shared Wine retains Forever's defaults
when those defines are absent. Display branding has one editable source,
`ps5/title/sce_sys/param.json`; the generated native header is refreshed by title
builds and relevant host tests. Cache keys include product identity inputs.

Host and container builds are not console qualification; see the README for the
known limits.

## R1 bootstrap and diagnostic modes

`ps5/probes/psmkit-wine/b1-product.json` selects maintenance mode and
`C:\RuneScape\runescape-bootstrap.exe`. `build-runtime.py` compiles that PE from
first-party source and emits its product paths/argv/publisher from
`runtime_profiles.bootstrap_header`. The immutable `/app0/wine-title/runescape/`
copies of the helper and digest-pinned DigiCert Trusted Root G4 are sources for
the existing managed prefix-file sync into `drive_c/RuneScape/`; the title has
no `Z:` drive. Both security assets are re-hashed on every start, repaired if
missing or modified, and synced before Wine on fresh and existing prefixes.
The PE separately checks the root's pinned digest before using it. The release
inventory deliberately excludes `wine-title/addonforge/` and WoW `Config.wtf`.

Default `startup_mode=ssn` retrieves the current official x64 production SSN
manifests/pieces natively, stages a complete tree, then uses the PE trust gate
for publisher checks, transactional publication, Win32 registry registration and
maintenance launch. The gate requires current-time validity and hard-fail
revocation against its immutable exclusive root; writable-prefix root additions,
cached receipts and unverified timestamps are not trust bypasses.

Private profile copies may select `installer-trial` (verified official
`--silent --install`) or `launcher-trial` (independently staged current launcher).
Both trials are bounded to 120 seconds and job-contained. No account sign-in,
console run, release build or qualification is implied by choosing a mode.

### Fresh `/data/runescape` sequence (code contract, not a console receipt)

The title creates its private prefix/home/cache, synchronizes the managed
prefix files, and initializes Windows with `wineboot -i`.
Expect `b1 maintenance first-start prefix setup command=wineboot -i`
then `forever: first-start prefix setup finished=1 ready=1 status=0`; successful
setup automatically restarts `/app0/eboot.bin`. Failure stays visible rather than
proceeding into Jagex code.

After restart, default `ssn` downloads the official installer as a verification
input, reconstructs the current official launcher tree natively, and invokes the
managed PE gate. Expect `runescape: installer acquired bytes=...`,
`runescape: reconstruction complete id=... files=... bytes=...`, then
`runescape: bootstrap begin role=maintenance mode=ssn argc=...`. Reconstruction
alone is **not** publisher acceptance. The PE emits
`runescape: verify kind=installer ... publisher_match=1 ... chain=valid ...`
and corresponding launcher verification, publishes/registers the SSN tree, and
emits `runescape: install complete method=ssn ...` followed by
`runescape: launcher begin role=maintenance backend=inherited`. The normal
launcher is not subject to the diagnostic trials' 120-second timeout.

Any maintenance-parent Wine error (including a missing bootstrap or trust-gate
rejection) shows a native title-screen error and stays idle until the user
closes the title; it must not call native `exit()`/`_exit()`.

Trust failure instead emits `runescape: verify kind=... accepted=0 ...` and
`runescape: bootstrap rejected policy=...`, with an installation-blocked dialog.
Network/reconstruction failures emit `runescape: acquisition failed ...` or
`runescape: reconstruction failed ...` and hold an error message. No Jagex
executable is bundled or executed before the publisher gate accepts it.

Select trials at **build time** using `startup_mode` in a private copy of
`b1-product.json`, passed to `make runtime-b1 B1_PROFILE_CONFIG=/absolute/copy.json`
from `ps5/probes/psmkit-wine`; no runtime file or menu automatically selects a
trial. The full dual release target explicitly selects the checked-in product
profile. `installer-trial` verifies and runs the
official installer with `--silent --install`, logging `runescape: installer begin
silent=1 install=1` and `runescape: installer end status=... installed=...
completed=...`. `launcher-trial` reconstructs and verifies an independent staging
tree and runs it without SSN publication/registration, logging
`runescape: launcher begin role=maintenance trial=1 backend=inherited`. Each
trial is job-contained and bounded to 120 seconds. Neither trial is selected by
the default package, and neither proves working UI, login or gameplay.

The host suite includes strict parser/path/digest contracts, actual SSN
reconstruction with deterministic HTTPS inputs and interruption cases, restricted
redirect tests, and `test-runescape-bootstrap.py`, which cross-compiles the real
PE into a temporary directory inside the checkout. Use the pinned container's
`/opt/llvm-mingw`; host compilation is **not** Authenticode/revocation validation.
The helper opts into MinGW's extended `CERT_CHAIN_PARA` and
`CERT_REVOCATION_PARA` layouts before including Windows headers, preserving
the explicit 10-second retrieval timeout fields and full structure `cbSize`.
Root review, real positive/hostile trust receipts, SDK TLS-negative cases, and a
visibly responsive launcher/mapper/IME still require separate qualification.

