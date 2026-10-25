# Third-party notices

RuneScape Launcher (PS5) as a whole is distributed under the GNU General Public License,
version 3 or later (see `LICENSE`). It combines the components below, which keep
their own licences. External sources are listed at the exact revision in
`ps5/release/deps.json`; repository-contained third-party sources keep
content hashes and provenance beside their code. The ZIP includes the GPL text
and component licence/copyright notices in `licenses/`; the public source
repositories and pinned source archives provide corresponding source and build
instructions.

| Component | Licence | Used for |
| --- | --- | --- |
| [Proton](https://github.com/ValveSoftware/Proton) (Valve) | per component, see Proton's `LICENSE` and `dist.LICENSE` | starting point of the build system and Wine tree |
| [Wine](https://www.winehq.org/), PS5 fork | LGPL-2.1-or-later | maintenance Jagex compatibility runtime and parked game libraries; files below `wine-title/` except those listed below |
| [AddonForge](https://github.com/Topher121/AddonForge) 0.3.0 and its Rust dependencies | MIT and dependency licences retained in `licenses/AddonForge/` | inherited source/build tooling, not staged or started by R1 |
| [Forever's github-wow-addon-catalogue fork](https://github.com/bizkut/github-wow-addon-catalogue), derived from [layday's catalogue](https://github.com/layday/github-wow-addon-catalogue) | GPL-3.0 | retained build data, not a runtime catalogue in R1; copyright layday, bizkut and contributors |
| [miniz](https://github.com/richgel999/miniz) 11.0.2, commit `293d4db1b7d0` | MIT | bounded SSN gzip inflation in the maintenance parent and parked ZIP extraction in `wine-title/addon-worker.elf`; licence in `licenses/miniz/` |
| [DigiCert Trusted Root G4](https://cacerts.digicert.com/DigiCertTrustedRootG4.crt) | public CA certificate, data only | immutable `wine-title/runescape/DigiCertTrustedRootG4.der`; SHA-256 `552f7bdcf1a7af9e6ce672017f4f12abf77240c78e761ac203d1d9d20ac89988`; bootstrap policy review/qualification is separate |
| [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton), PS5 fork | Combined frame-generation build: GPL-3.0-or-later; underlying vkd3d-proton code retains LGPL-2.1-or-later | `d3d12.dll`, `d3d12core.dll` |
| [Eden](https://eden-emu.dev/) frame generation (commit `67bada77`), via [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden) | GPL-3.0-or-later | `vkd3d-proton/libs/vkd3d/wineps5_fg*` |
| [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) | GPL-3.0-or-later | the Lossless Scaling frame-generation code Eden's port is based on |
| [ps5-framegen](https://github.com/swordpdf/ps5-framegen) (swordpdf), revision `64362101757c` | GPL-3.0-or-later | `vkd3d-proton/libs/vkd3d/wineps5_fsr3fg/` |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.3.0 shaders | MIT | FSR 3 optical flow and interpolation shaders |
| [DXVK](https://github.com/doitsujin/dxvk), PS5 fork, pinned `9f8db7dae7e9218eb6f0197ac7b3a61367e3044d` | zlib | 64-bit `d3d9.dll`, `d3d10core.dll`, `d3d11.dll`, `dxgi.dll` (OSRS game role and RuneLite Java2D experiment); source/bundled notices in `licenses/DXVK/` |
| [Mesa](https://mesa3d.org/) RADV and Windows WGL/Zink, pinned PS5 fork `f10c4bac9b71c45a4c794ecc3c227eb6f6a56361` (upstream WGL revision `15a5e9933e14f6493a7853e8cf1df973dd093eeb`) | MIT and per-file licences in `licenses/Mesa/` | Vulkan driver linked into the app; PE64 `opengl32.dll` and `libgallium_wgl.dll` for OSRS and the RuneLite experiment; no Windows Vulkan loader/ICD; RuneLite/client JVM not bundled |
| [whisper.cpp](https://github.com/ggml-org/whisper.cpp) 1.9.5 | MIT | Voice-to-Text speech recognition, statically linked into the game parent |
| [ggml](https://github.com/ggml-org/ggml) 0.26.0, with PS5 allocation limits | MIT | whisper.cpp CPU and Vulkan inference backends |
| [OpenAI Whisper](https://github.com/openai/whisper) speech model weights | MIT | Voice-to-Text model, downloaded at runtime on first use; not included in the release ZIP |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (mihawk-99) | GPL-3.0-or-later; bundled components retain their notices | Vulkan loader integration and title build tools |
| [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) (mihawk-99), from [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), RuneScape SDK pin `b1fc4139f2fa266580a62e34ab95c88d949657c3` | GPL-3.0-or-later; libc and toolchain components retain their notices | toolchain, libc and `sce_module/libc.prx` |
| [FreeType](https://freetype.org/) | FTL or GPL-2.0-or-later | `wine-title/lib/libfreetype.so`; this distribution is based in part on the work of the FreeType Team |
| [GnuTLS](https://www.gnutls.org/) | LGPL-2.1-or-later | `wine-title/lib/libgnutls.so` |
| [Nettle](https://www.lysator.liu.se/~nisse/nettle/) | LGPL-3.0-or-later or GPL-2.0-or-later | linked into GnuTLS |
| [libtasn1](https://www.gnu.org/software/libtasn1/) | LGPL-2.1-or-later | linked into GnuTLS |
| [pacbrew](https://github.com/OpenAGC/pacbrew-repo) recipes | see the repository | build recipes for GnuTLS, Nettle and libtasn1 |
| Mozilla root certificates (`cacert.pem`) | MPL-2.0 | `wine-title/share/wine/cacert.pem`, trusted roots for HTTPS |
| Noto and Droid fonts | OFL-1.1 / Apache-2.0 | `wine-title/share/wine/fonts/`; Noto Sans menu glyph rasters in `ps5/title/menu-font.h`; notices in `ps5/release/licenses/fonts/` |
| Wine built-in fonts | LGPL-2.1-or-later; Tahoma also retains the Bitstream Vera copyright/licence | `wine-title/share/wine/fonts/`; original font notices are included in `licenses/fonts/` |
| [Liberation Fonts](https://github.com/liberationfonts/liberation-fonts) 2.1.5 (Sans, Serif, Mono; unmodified) | OFL-1.1 | `wine-title/share/forever/fonts/`, copied into the prefix's `C:\windows\Fonts` under the Arial/Times New Roman/Courier New file names Java expects; licence in `licenses/fonts/Liberation-LICENSE` |
| `ps5/title/compositor.c` (adapted part, lines 67-68) and `ps5/title/tools/make-title-background.py`, from ps5-forever by Mihawk | MIT, notices kept in the files | title screen drawing and background encoder |
| `ps5/title/art/rs_bg.jpg` and `ps5/title/art/rs.png`, supplied by the project owner as their own artwork | owner's artwork, see `ps5/title/art/README.md` | home-screen tile icon, tile and launch background, and the desktop picture behind the Jagex Launcher |
| `ps5/probes/psmkit-wine/title-icon.h`, from ps5-forever `examples/battlenet/icon.c` by Mihawk | MIT, notice kept in the file | the home-screen icon made from the installed Battle.net launcher |
| [stb_image](https://github.com/nothings/stb) v2.30 (Sean Barrett) | Public domain or MIT | decoding that launcher's PNG icon |

## Not included

- **`Lossless.dll`** from [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)
  is proprietary. It is never shipped or downloaded. Users supply their own copy;
  RuneScape Launcher (PS5) reads shader resources from it and never runs its code.
- **Battle.net and World of Warcraft** are downloaded by the user from Blizzard
  with Blizzard's own installer and are subject to Blizzard's terms.
- **Jagex Launcher, RuneScape and Old School RuneScape clients/game content**
  are proprietary and are not bundled. Users must obtain them directly from
  Jagex and comply with Jagex's terms.

World of Warcraft, Battle.net and Blizzard are trademarks of Blizzard Entertainment, Inc.
PlayStation and PS5 are trademarks of Sony Interactive Entertainment Inc.
RuneScape, Old School RuneScape and Jagex are trademarks of Jagex Limited.
This unofficial project is not affiliated with, approved, endorsed or supported
by Jagex, Sony Interactive Entertainment, Blizzard Entertainment or Valve.
