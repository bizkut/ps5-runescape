.DEFAULT_GOAL := all
.NOTPARALLEL:
.DELETE_ON_ERROR:
OUT := ../../build/title-psmkit-wine-descriptor
WORK := ../../build/title-psmkit-wine-descriptor-work
CHILD_OUT := ../../build/title-psmkit-wine-descriptor-child
CHILD_WORK := ../../build/title-psmkit-wine-descriptor-child-work
HOST_OUT := ../../build/psmkit-wine-host
PE_OUT := ../../build/psmkit-wine-pe
TITLE_BUILD := ../../title/build.sh
TITLE_PARAM := ../../title/sce_sys/param.json
PS5_VULKAN ?= $(abspath ../../../../../PS5_Vulkan)
NATIVE_TOOL ?= $(PS5_VULKAN)/build/host/ps5-native-tool
PATCH := ../localproc/patch-local-helper.py
IO := ../localproc/psmkit-io.c
WINE_SRC ?= $(abspath ../../../../ps5-wine)
PREFIX_INIT_HEADER := $(WINE_SRC)/include/wine/ps5_prefix_init.h
SHARED_MODE_HEADER := $(WINE_SRC)/include/wine/ps5_shared_mode.h
SHARED_GAME_HEADER := $(WINE_SRC)/include/wine/ps5_game.h
PREFIX_INIT_INCLUDES := -idirafter "$(WINE_SRC)/include"
HOST_CC ?= cc
HOST_CXX ?= c++
override HOST_CC += -include $(abspath ../../include/product-wine.h)

../../include/product-name.h: ../../title/sce_sys/param.json ../../title/tools/product-name.py
	python3 ../../title/tools/product-name.py

$(HOST_OUT)/test-forever-menu $(HOST_OUT)/test-forever-setup: ../../include/product-name.h

.PHONY: test-product-identity
test: test-product-identity
test-product-identity: ../../include/product-name.h
	python3 ../../release/test-product-identity.py
.PHONY: test-title-sandbox-calls
test: test-title-sandbox-calls
test-title-sandbox-calls: test-title-sandbox-calls.py
	python3 test-title-sandbox-calls.py
LLVM_MINGW ?= $(HOME)/llvm-mingw
MINGW_CC ?= $(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang
MINGW32_CC ?= $(LLVM_MINGW)/bin/i686-w64-mingw32-clang
WINE_BUILD ?= ../../build/wine-ps5-build
WINEBUILD ?= ../../build/wine-ps5-tools/tools/winebuild/winebuild
WRITEBROKER_ELF := ../../build/writebroker/writebroker.elf
WRITEBROKER_DIR := ../../writebroker
ADDON_WORKER_ELF := ../../build/addon-worker/addon-worker.elf
ADDON_WORKER_DIR := ../../addon-worker
ADDON_WORKER_MINIZ := miniz miniz_tinfl miniz_tdef miniz_zip
ADDON_WORKER_HOST_OBJECTS := $(addprefix $(HOST_OUT)/addon-worker-,$(addsuffix .o,$(ADDON_WORKER_MINIZ)))
RUNTIME_OUT := ../../build/title-psmkit-wine-runtime
RUNTIME_QUARANTINE := ../../build/title-psmkit-wine-runtime-quarantine
RUNTIME_PARENT := $(RUNTIME_QUARANTINE)/parent-title
PLATFORM_SRC ?= $(abspath ../../../../../PS5_PayloadSDK/platform)
# Override B1_OUT for isolated publications; every intermediate follows it.
B1_OUT ?= ../../build/title-psmkit-wine-b1
B1_QUARANTINE ?= $(B1_OUT)-quarantine
B1_MAINTENANCE_OUT ?= $(B1_OUT)-maintenance
B1_GAME_OUT ?= $(B1_OUT)-game
B1_PROFILE_CONFIG ?=
B1_GAME_PROFILE_CONFIG ?= $(CURDIR)/b1-game-osrs.json
B1_CONFIG_ARG := $(if $(B1_PROFILE_CONFIG),--profile-config "$(B1_PROFILE_CONFIG)",)
TITLE_DESKTOP := ../../title/main.c ../../title/hub.c ../../title/compositor.c \
	../../title/forever-menu.c ../../title/addonforge-client.c ../../title/display.c \
	../../title/input.c ../../title/pointer.c ../../title/padmap.c ../../title/ime.c
TITLE_VOICE := ../../title/voice.cpp ../../title/voice.h ../../title/voice-state.c \
	../../title/voice-state.h ../../title/stubs/libSceAudioIn_stub.c
TITLE_VOICE += ../../title/voice-model.h forever-download.h
TITLE_VOICE += ../../title/voice-overlay-raster.c ../../title/voice-overlay-raster.h \
	../../title/voice-overlay-vulkan.c ../../title/voice-overlay-vulkan.h ../../title/bitmap-font.h

.PHONY: all descriptor test test-startup-routing test-runtime-profile pe runtime runtime-b1 runtime-b1-dual runtime-child runtime-parent runtime-qualification test-staged
all: descriptor pe
# Intentionally not named runtime: no Wine loader is entered by this gate.
descriptor: $(OUT)/eboot.bin $(OUT)/psmkit-wine-descriptor-child.bin
pe: $(PE_OUT)/console.exe $(PE_OUT)/p1w-parent.exe $(PE_OUT)/p1w-child.exe \
		$(PE_OUT)/audio-tone.exe

runtime: runtime-qualification

# One build creates both roles with the same reviewed import/module manifest.
runtime-child:
	python3 build-runtime.py --wine-build "$(WINE_BUILD)"

runtime-parent: runtime-child

runtime-qualification: runtime-child pe $(HOST_OUT)/test-staged-runtime
	python3 inspect-runtime.py --wine-build "$(WINE_BUILD)" \
		--host-test "$(HOST_OUT)/test-staged-runtime" \
		--parent-title "$(RUNTIME_PARENT)" --publish "$(RUNTIME_OUT)" \
		--pe-dir "$(PE_OUT)"

runtime-b1: $(HOST_OUT)/test-staged-runtime build-runtime.py runtime_profiles.py \
		runtime_imports.py runtime-import-policy-b1.json native-entry.c runtime-parent.c \
		runtime-parent.h runtime-relaunch.h runtime-game-file.h runtime-app-close.h wow-channels.h writebroker-autostart.h writebroker-menu.h \
		forever-menu-settings.h forever-setup.h forever-fonts.h $(PREFIX_INIT_HEADER) sha256.h addon-worker-elf.h runtime-vulkan.c runtime-profile.h runtime-input.c \
		protocol.c protocol.h title-log.c title-log.h $(TITLE_DESKTOP) $(TITLE_VOICE) \
		forever-download.h runescape-vaprobe.h runescape-install.h runescape-ssn.h runescape-piece.h runescape-bootstrap.c \
		runescape-redist.h runescape-launcher-retry.h runescape-permissions.h runescape-shared-io.h $(SHARED_MODE_HEADER) \
		runtime-game-profile.h $(SHARED_GAME_HEADER) \
		../../addon-worker/third_party/miniz/miniz_tinfl.c \
		../../addon-worker/third_party/miniz/miniz_tinfl.h ../../addon-worker/third_party/miniz/miniz_common.h \
		../../title/padmap.h \
		../../title/installer-continue.h \
		../../title/forever-menu.h ../../title/addonforge-client.h \
		../../title/addonforge-security.h ../../title/menu-font.h \
		../../title/menu-diag.h ../../title/tools/make-menu-font.py \
		../../title/cursor-arrow.h ../../addon-worker/protocol.h \
		$(PE_OUT)/audio-tone.exe $(WRITEBROKER_ELF) $(ADDON_WORKER_ELF)
	# B1 installs Wine's PE tree (make install-lib), which runs the i686/x86_64
	# mingw compilers configure chose; they live in llvm-mingw's bin.
	PATH="$(dir $(MINGW_CC)):$$PATH" python3 build-runtime.py --wine-build "$(WINE_BUILD)" \
		--profile b1 $(B1_CONFIG_ARG) --writebroker-elf "$(WRITEBROKER_ELF)" \
		--addon-worker-elf "$(ADDON_WORKER_ELF)" --quarantine "$(B1_QUARANTINE)"
	python3 inspect-runtime.py --wine-build "$(WINE_BUILD)" --profile b1 $(B1_CONFIG_ARG) \
		--host-test "$(HOST_OUT)/test-staged-runtime" \
		--parent-title "$(B1_QUARANTINE)/parent-title" --quarantine "$(B1_QUARANTINE)" --publish "$(B1_OUT)"

$(WRITEBROKER_ELF): $(WRITEBROKER_DIR)/writebroker.c $(WRITEBROKER_DIR)/Makefile \
		writebroker-menu.h $(WINE_SRC)/include/wine/ps5_writebroker.h
	$(MAKE) -C "$(WRITEBROKER_DIR)" WINE_SRC="$(WINE_SRC)" all

$(ADDON_WORKER_ELF): $(ADDON_WORKER_DIR)/main.cpp $(ADDON_WORKER_DIR)/addon-worker.cpp \
		$(ADDON_WORKER_DIR)/addon-worker.hpp $(ADDON_WORKER_DIR)/protocol.h \
		sha256.h $(ADDON_WORKER_DIR)/verify-sources.py \
		$(ADDON_WORKER_DIR)/Makefile $(wildcard $(ADDON_WORKER_DIR)/third_party/miniz/*)
	$(MAKE) -C "$(ADDON_WORKER_DIR)" all

# Product publication: independently build/qualify maintenance first and game
# second, then bind both receipts and both role-specific children into one tree.
runtime-b1-dual:
	rm -rf "$(B1_MAINTENANCE_OUT)" "$(B1_GAME_OUT)"
	$(MAKE) runtime-b1 B1_PROFILE_CONFIG="$(CURDIR)/b1-product.json"
	cp -R "$(B1_OUT)" "$(B1_MAINTENANCE_OUT)"
	$(MAKE) runtime-b1 B1_PROFILE_CONFIG="$(B1_GAME_PROFILE_CONFIG)"
	cp -R "$(B1_OUT)" "$(B1_GAME_OUT)"
	python3 combine-b1-runtime.py --maintenance "$(B1_MAINTENANCE_OUT)" \
		--game "$(B1_GAME_OUT)" --output "$(B1_OUT)"

.PHONY: radv-release-patched
radv-release-patched:
	python3 ../../radv/build-radv.py

# Requires the configured Wine host tools; deliberately separate from test.
.PHONY: test-winebuild-delayload
test-winebuild-delayload: test-winebuild-delayload.py $(WINEBUILD)
	python3 test-winebuild-delayload.py --winebuild "$(WINEBUILD)" --llvm-mingw "$(LLVM_MINGW)"

test-runtime-profile: test-runtime-profile.py runtime_profiles.py runtime_imports.py \
		runtime-import-policy-b1.json ../../deploy-wine.py Makefile b1-product.json \
		b1-game-osrs.json
	python3 test-runtime-profile.py

.PHONY: test-native-wgl test-vulkan-attribution
test: test-native-wgl test-vulkan-attribution
test-native-wgl: test-native-wgl.py $(WINE_SRC)/dlls/gdi32/opengl.c \
		$(WINE_SRC)/dlls/opengl32/wgl.c $(WINE_SRC)/include/wine/wgl_pixel_format.h
	python3 test-native-wgl.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"
test-vulkan-attribution: test-vulkan-attribution.py runtime-vulkan.c $(WINE_SRC)/dlls/win32u/vulkan.c
	python3 test-vulkan-attribution.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"

.PHONY: test-combase-surrogate
test: test-combase-surrogate
test-combase-surrogate: test-combase-surrogate.py $(WINE_SRC)/dlls/combase/rpc.c
	python3 test-combase-surrogate.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)/combase-surrogate" --cc "$(HOST_CC)"

.PHONY: test-title-viewport test-title-viewport-vulkan
test: test-title-viewport
test-title-viewport: test-title-viewport.py $(WINE_SRC)/dlls/win32u/vulkan.c \
		$(WINE_SRC)/dlls/win32u/driver.c $(WINE_SRC)/include/wine/ps5_viewport.h
	python3 test-title-viewport.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"
test-title-viewport-vulkan: test-title-viewport.py test-title-viewport-vulkan.c \
		$(WINE_SRC)/dlls/win32u/vulkan.c $(WINE_SRC)/include/wine/ps5_viewport.h
	python3 test-title-viewport.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)" --vulkan

.PHONY: test-title-compositor test-title-compositor-vulkan
test: test-title-compositor
COMPOSITOR_DEPS = test-title-compositor.py test-title-compositor.c build-desktop-surface-test.py \
		$(WINE_SRC)/dlls/win32u/ps5_compositor.c $(WINE_SRC)/dlls/win32u/ps5_compositor_spv.h \
		$(WINE_SRC)/include/wine/ps5_compositor.h $(WINE_SRC)/include/wine/ps5_viewport.h \
		$(WINE_SRC)/dlls/win32u/vulkan.c $(WINE_SRC)/dlls/win32u/driver.c
test-title-compositor: $(COMPOSITOR_DEPS)
	python3 test-title-compositor.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"
# Host only: needs libvulkan.so.1 and a CPU Vulkan driver (lavapipe).
test-title-compositor-vulkan: $(COMPOSITOR_DEPS) test-title-compositor-vulkan.c
	python3 test-title-compositor.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)" --vulkan

.PHONY: test-title-mouse-trace test-title-click-api
test: test-title-mouse-trace
test-title-mouse-trace: test-title-mouse-trace.py $(HOST_OUT)/title-click-host.exe \
		$(WINE_SRC)/dlls/win32u/driver.c $(WINE_SRC)/dlls/win32u/message.c \
		$(WINE_SRC)/dlls/win32u/ntuser_private.h $(WINE_SRC)/server/queue.c
	python3 test-title-mouse-trace.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"

.PHONY: test-title-mouse-activation
test: test-title-mouse-activation
test-title-mouse-activation: test-title-mouse-activation.py $(WINE_SRC)/dlls/win32u/window.c \
		$(WINE_SRC)/dlls/win32u/message.c
	python3 test-title-mouse-activation.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"

.PHONY: test-runelite-diag
test: test-runelite-diag
test-runelite-diag: test-runelite-diag.py build-desktop-surface-test.py \
		$(WINE_SRC)/include/wine/ps5_runelite_diag.h \
		$(WINE_SRC)/dlls/ntdll/unix/virtual.c $(WINE_SRC)/dlls/ntdll/unix/thread.c \
		$(WINE_SRC)/dlls/win32u/window.c
	python3 test-runelite-diag.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"

.PHONY: test-title-volume test-native-mac test-nsi-interfaces
test: test-title-volume test-native-mac test-nsi-interfaces
test-title-volume: test-title-volume.py build-desktop-surface-test.py \
		$(WINE_SRC)/dlls/ntdll/unix/file.c $(WINE_SRC)/dlls/kernelbase/volume.c \
		$(WINE_SRC)/dlls/ntdll/error.c $(WINE_SRC)/dlls/ntdll/error.h
	python3 test-title-volume.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"
test-native-mac: test-native-mac.py build-desktop-surface-test.py native-entry.c protocol.h runtime_imports.py
	python3 test-native-mac.py --out "$(HOST_OUT)" --cc "$(HOST_CC)"
test-nsi-interfaces: test-nsi-interfaces.py test-nsi-interfaces.c build-nsi-tcp-test.py test-nsi-tcp.c \
		build-desktop-surface-test.py $(WINE_SRC)/dlls/nsi/nsi.c $(WINE_SRC)/dlls/nsi/unixlib.c \
		$(WINE_SRC)/dlls/nsiproxy.sys/ndis.c $(WINE_SRC)/dlls/nsiproxy.sys/ip.c \
		$(WINE_SRC)/dlls/nsiproxy.sys/scope.c $(WINE_SRC)/dlls/nsiproxy.sys/tables.c \
		$(WINE_SRC)/dlls/nsi/Makefile.in $(WINE_SRC)/dlls/nsiproxy.sys/Makefile.in \
		$(WINE_SRC)/dlls/nsiproxy.sys/unix_private.h $(WINE_SRC)/include/wine/nsi.h \
		$(WINE_SRC)/dlls/iphlpapi/iphlpapi_main.c
	python3 test-nsi-interfaces.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"

$(HOST_OUT)/title-click-host.exe: test-title-click.c
	mkdir -p $(HOST_OUT)
	$(MINGW_CC) -O2 -Wall -Wextra -Werror $< -o $@ -luser32

# Real USER32 reference only; never use the developer's default Wine prefix.
test-title-click-api: $(HOST_OUT)/title-click-host.exe
	test -n "$(HOST_WINEPREFIX)"
	env WINEPREFIX="$(HOST_WINEPREFIX)" WINEDEBUG=-all $(or $(HOST_WINE),wine) $<

.PHONY: test-runescape-handoff
test: test-runescape-handoff
test-runescape-handoff: test-runescape-handoff.py runescape-bootstrap.c native-entry.c runtime-parent.c \
		runtime-relaunch.h runtime-game-profile.h $(SHARED_GAME_HEADER) \
		$(WINE_SRC)/dlls/ntdll/unix/process.c
	python3 test-runescape-handoff.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"

.PHONY: test-runescape-loadexec
test: test-runescape-loadexec
test-runescape-loadexec: test-runescape-loadexec.py native-entry.c runtime-game-file.h runtime-game-profile.h runtime-relaunch.h runtime-app-close.h protocol.h $(SHARED_MODE_HEADER) $(SHARED_GAME_HEADER)
	python3 test-runescape-loadexec.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"

.PHONY: test-runescape-permissions
test: test-runescape-permissions
test-runescape-permissions: test-runescape-permissions.py test-runescape-permissions.c \
		runescape-permissions.h runescape-shared-io.h runtime-game-file.h runtime-game-profile.h runtime-relaunch.h runescape-install.h $(SHARED_MODE_HEADER) $(SHARED_GAME_HEADER) \
		$(WINE_SRC)/server/fd.c $(WINE_SRC)/server/file.c $(WINE_SRC)/dlls/ntdll/unix/file.c \
		../../writebroker/writebroker.c
	python3 test-runescape-permissions.py --wine "$(WINE_SRC)" --cc "$(HOST_CC)"

.PHONY: test-prefix-fonts
test: test-prefix-fonts
test-prefix-fonts: test-prefix-fonts.py runtime_profiles.py forever-fonts.h forever-setup.h \
		native-entry.c $(PREFIX_INIT_HEADER) sha256.h $(WINE_SRC)/loader/wine.inf.in \
		../../release/runtime-files.txt ../../release/deps.json ../../release/licenses/fonts/Liberation-LICENSE
	python3 test-prefix-fonts.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)"

.PHONY: test-build-speed
test-build-speed: test-build-speed.py ../../configure-stamp.py ../../release/package.py
	python3 test-build-speed.py

test-staged: runtime-qualification

$(HOST_OUT)/test-staged-runtime: test-staged-runtime.c elf-runtime.c elf-runtime.h sha256.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread test-staged-runtime.c elf-runtime.c -o $@

$(HOST_OUT)/test-placeholder: test-placeholder.c build-placeholder-test.py native-entry.c build-runtime.py \
		$(WINE_SRC)/dlls/ntdll/unix/virtual.c $(WINE_SRC)/dlls/ntdll/unix/ps5_direct.c
	python3 build-placeholder-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-nsi-tcp: build-nsi-tcp-test.py test-nsi-tcp.c build-desktop-surface-test.py \
		$(WINE_SRC)/dlls/nsi/nsi.c $(WINE_SRC)/dlls/nsi/unixlib.c \
		$(WINE_SRC)/include/wine/nsi_tcp.h $(WINE_SRC)/server/sock.c \
		$(WINE_SRC)/dlls/iphlpapi/iphlpapi_main.c
	python3 build-nsi-tcp-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

test: $(HOST_OUT)/test-protocol $(HOST_OUT)/test-protocol-runescape \
		$(HOST_OUT)/test-supervisor-admission $(HOST_OUT)/test-supervisor-admission-runescape $(HOST_OUT)/test-installer-continue \
		$(HOST_OUT)/test-elf-runtime $(HOST_OUT)/test-wine-title \
		$(HOST_OUT)/test-wine-title-exit $(HOST_OUT)/test-write-buffer $(HOST_OUT)/test-request-profile $(HOST_OUT)/test-runtime-profile-callbacks $(HOST_OUT)/test-loader-path $(HOST_OUT)/test-runtime-imports \
		$(HOST_OUT)/test-direct-commit $(HOST_OUT)/test-direct-cache $(HOST_OUT)/test-direct-fail-fast $(HOST_OUT)/test-vm-cpu \
		$(HOST_OUT)/test-direct-reserve $(HOST_OUT)/test-placeholder $(HOST_OUT)/test-runescape-vaprobe \
		$(HOST_OUT)/test-runtime-input $(HOST_OUT)/test-padmap $(HOST_OUT)/test-maintenance-padmap $(HOST_OUT)/test-title-input \
		$(HOST_OUT)/test-runescape-ssn $(HOST_OUT)/test-runescape-install $(HOST_OUT)/test-runescape-launcher-retry $(HOST_OUT)/test-nsi-tcp \
		$(HOST_OUT)/test-title-ime $(HOST_OUT)/test-title-ime-delivery $(HOST_OUT)/test-title-ime-delivery-diag $(HOST_OUT)/test-title-voice \
		$(HOST_OUT)/test-title-voice-overlay $(HOST_OUT)/test-title-voice-overlay-vulkan \
		$(HOST_OUT)/test-runtime-relaunch $(HOST_OUT)/test-wow-channels \
		$(HOST_OUT)/test-battlenet-restart $(HOST_OUT)/test-audio-mixer \
		$(HOST_OUT)/test-audio-selection $(HOST_OUT)/test-fd-parking \
		$(HOST_OUT)/test-writebroker-autostart $(HOST_OUT)/test-writebroker-space $(HOST_OUT)/test-title-log \
		$(HOST_OUT)/test-forever-setup $(HOST_OUT)/test-forever-menu $(HOST_OUT)/test-forever-exit \
		$(HOST_OUT)/test-addonforge-client $(HOST_OUT)/test-addonforge-worker $(HOST_OUT)/test-addonforge-security $(HOST_OUT)/test-addon-trash-native \
		$(HOST_OUT)/test-addon-native-worker $(HOST_OUT)/addon-worker-host $(HOST_OUT)/test-addon-worker-elf \
		$(HOST_OUT)/test-addon-worker-request $(HOST_OUT)/test-registry-save \
		$(HOST_OUT)/test-forever-menu-diag \
		$(HOST_OUT)/test-title-icon $(HOST_OUT)/test-desktop-surface \
		$(HOST_OUT)/test-cursor $(HOST_OUT)/test-cursor-hub \
		$(HOST_OUT)/test-cursor-peer $(HOST_OUT)/test-file-security \
		$(PE_OUT)/audio-tone.exe test-startup-routing test-runtime-profile test-build-speed \
		../../tools/test-ps5-capture.py
	$(HOST_OUT)/test-protocol
	$(HOST_OUT)/test-protocol-runescape
	$(HOST_OUT)/test-supervisor-admission
	$(HOST_OUT)/test-supervisor-admission-runescape
	$(HOST_OUT)/test-installer-continue
	$(HOST_OUT)/test-elf-runtime
	$(HOST_OUT)/test-wine-title
	$(HOST_OUT)/test-wine-title-exit
	$(HOST_OUT)/test-write-buffer
	$(HOST_OUT)/test-request-profile
	$(HOST_OUT)/test-runtime-profile-callbacks
	$(HOST_OUT)/test-direct-commit
	$(HOST_OUT)/test-direct-reserve
	$(HOST_OUT)/test-placeholder
	$(HOST_OUT)/test-runescape-vaprobe
	$(HOST_OUT)/test-direct-cache
	$(HOST_OUT)/test-direct-fail-fast
	$(HOST_OUT)/test-vm-cpu
	$(HOST_OUT)/test-loader-path
	$(HOST_OUT)/test-runtime-imports
	$(HOST_OUT)/test-runtime-input
	$(HOST_OUT)/test-padmap
	$(HOST_OUT)/test-maintenance-padmap
	$(HOST_OUT)/test-runescape-ssn
	$(HOST_OUT)/test-runescape-install
	$(HOST_OUT)/test-runescape-launcher-retry
	$(HOST_OUT)/test-nsi-tcp
	python3 test-runescape-bootstrap.py
	$(HOST_OUT)/test-title-input
	$(HOST_OUT)/test-title-ime
	$(HOST_OUT)/test-title-ime-delivery
	$(HOST_OUT)/test-title-ime-delivery-diag
	$(HOST_OUT)/test-title-voice
	$(HOST_OUT)/test-title-voice-overlay
	$(HOST_OUT)/test-title-voice-overlay-vulkan
	$(HOST_OUT)/test-runtime-relaunch
	$(HOST_OUT)/test-wow-channels
	$(HOST_OUT)/test-battlenet-restart
	$(HOST_OUT)/test-audio-mixer
	$(HOST_OUT)/test-audio-selection
	$(HOST_OUT)/test-fd-parking
	$(HOST_OUT)/test-writebroker-autostart
	$(HOST_OUT)/test-writebroker-space
	$(HOST_OUT)/test-title-log
	$(HOST_OUT)/test-forever-setup
	$(HOST_OUT)/test-forever-menu
	$(HOST_OUT)/test-forever-exit
	$(HOST_OUT)/test-addonforge-client
	$(HOST_OUT)/test-addonforge-worker
	$(HOST_OUT)/test-addonforge-security
	$(HOST_OUT)/test-addon-trash-native
	$(HOST_OUT)/test-addon-native-worker
	python3 test-addon-native-worker-protocol.py "$(HOST_OUT)/addon-worker-host"
	$(HOST_OUT)/test-addon-worker-elf
	$(HOST_OUT)/test-addon-worker-request
	$(HOST_OUT)/test-registry-save
	$(HOST_OUT)/test-forever-menu-diag
	$(HOST_OUT)/test-title-icon
	$(HOST_OUT)/test-desktop-surface
	$(HOST_OUT)/test-cursor
	$(HOST_OUT)/test-cursor --no-proc
	$(HOST_OUT)/test-cursor --no-cursor-proc
	$(HOST_OUT)/test-cursor --cursor-no-modes
	$(HOST_OUT)/test-cursor-hub
	$(HOST_OUT)/test-cursor-peer
	$(HOST_OUT)/test-file-security
	python3 ../../tools/test-ps5-capture.py

$(HOST_OUT)/test-fd-parking: test-fd-parking.c build-fd-parking-test.py $(WINE_SRC)/server/fd.c
	python3 build-fd-parking-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-file-security: test-file-security.c build-file-security-test.py \
		build-desktop-surface-test.py $(WINE_SRC)/server/file.c \
		$(WINE_SRC)/server/token.c $(WINE_SRC)/server/security.h \
		$(WINE_SRC)/include/wine/server_protocol.h
	python3 build-file-security-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-audio-selection: test-audio-selection.c \
		$(WINE_SRC)/dlls/mmdevapi/ps5audio.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror \
		-I "$(WINE_SRC)/dlls/mmdevapi" $< -o $@

$(HOST_OUT)/test-audio-mixer: test-audio-mixer.c \
		$(WINE_SRC)/dlls/wineps5audio.drv/mixer.c \
		$(WINE_SRC)/dlls/wineps5audio.drv/mixer.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror \
		-I "$(WINE_SRC)/dlls/wineps5audio.drv" \
		test-audio-mixer.c $(WINE_SRC)/dlls/wineps5audio.drv/mixer.c -o $@

$(HOST_OUT)/test-runtime-relaunch: test-runtime-relaunch.c runtime-relaunch.h runtime-game-file.h wow-channels.h forever-paths.h \
		forever-setup.h runtime-game-profile.h $(SHARED_GAME_HEADER) $(PREFIX_INIT_HEADER) $(SHARED_MODE_HEADER) sha256.h protocol.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror $(PREFIX_INIT_INCLUDES) $< -o $@

.PHONY: test-supervisor-admission
test-supervisor-admission: $(HOST_OUT)/test-supervisor-admission $(HOST_OUT)/test-supervisor-admission-runescape
	$(HOST_OUT)/test-supervisor-admission
	$(HOST_OUT)/test-supervisor-admission-runescape

$(HOST_OUT)/test-supervisor-admission: test-supervisor-admission.c build-supervisor-admission-test.py \
		build-desktop-surface-test.py runtime-parent.c native-entry.c protocol.c protocol.h \
		runtime-relaunch.h runtime-game-profile.h $(SHARED_GAME_HEADER) \
		../../title/main.c ../../title/forever-menu.c ../../title/forever-menu.h \
		$(IO) ../localproc/psmkit-io.h
	python3 build-supervisor-admission-test.py --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-supervisor-admission-runescape: test-supervisor-admission.c build-supervisor-admission-test.py \
		build-desktop-surface-test.py runtime-parent.c native-entry.c protocol.c protocol.h runtime_profiles.py \
		runtime-relaunch.h runtime-game-profile.h $(SHARED_GAME_HEADER) \
		../../title/main.c ../../title/forever-menu.c ../../title/forever-menu.h \
		$(IO) ../localproc/psmkit-io.h
	python3 build-supervisor-admission-test.py --runescape --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-installer-continue: test-installer-continue.c ../../title/installer-continue.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror $< -o $@

.PHONY: test-desktop-surface
test-desktop-surface: $(HOST_OUT)/test-desktop-surface
	$(HOST_OUT)/test-desktop-surface

$(HOST_OUT)/test-desktop-surface: test-desktop-surface.c build-desktop-surface-test.py \
		../../title/compositor.c ../../title/compositor.h ../../title/display.h \
		../../title/forever-menu.c ../../title/forever-menu.h ../../title/menu-font.h \
		../../title/bitmap-font.h \
		$(WINE_SRC)/dlls/wineps5.drv/window.c $(WINE_SRC)/dlls/ntdll/unix/server.c \
		$(WINE_SRC)/include/wine/ps5_desktop.h $(WINE_SRC)/include/wine/ps5_title.h \
		runtime-parent.c protocol.h
	python3 build-desktop-surface-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

.PHONY: test-cursor
test-cursor: $(HOST_OUT)/test-cursor $(HOST_OUT)/test-cursor-hub $(HOST_OUT)/test-cursor-peer
	$(HOST_OUT)/test-cursor
	$(HOST_OUT)/test-cursor --no-proc
	$(HOST_OUT)/test-cursor --no-cursor-proc
	$(HOST_OUT)/test-cursor --cursor-no-modes
	$(HOST_OUT)/test-cursor-hub
	$(HOST_OUT)/test-cursor-peer

$(HOST_OUT)/test-cursor: test-cursor.c build-cursor-test.py build-desktop-surface-test.py \
		../../title/hub.c ../../title/hub.h $(WINE_SRC)/dlls/wineps5.drv/cursor.c \
		$(WINE_SRC)/dlls/win32u/driver.c $(WINE_SRC)/include/wine/ps5_cursor.h \
		$(WINE_SRC)/include/wine/ps5_desktop.h $(WINE_SRC)/include/wine/ps5_title.h
	python3 build-cursor-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-cursor-hub: test-cursor-hub.c ../../title/hub.c ../../title/hub.h \
		$(WINE_SRC)/include/wine/ps5_desktop.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -I "$(WINE_SRC)/include" $< -o $@

$(HOST_OUT)/test-cursor-peer: test-cursor-peer.c build-cursor-peer-test.py build-desktop-surface-test.py \
		runtime-parent.c $(WINE_SRC)/loader/main.c $(WINE_SRC)/dlls/wineps5.drv/init.c \
		$(WINE_SRC)/include/wine/ps5_desktop.h $(WINE_SRC)/dlls/wineps5.drv/ps5drv.h \
		$(WINE_SRC)/include/wine/ps5_cursor.h $(WINE_SRC)/dlls/wineps5.drv/cursor.c \
		$(WINE_SRC)/dlls/win32u/driver.c
	python3 build-cursor-peer-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-writebroker-autostart: test-writebroker-autostart.c writebroker-autostart.h ../../include/product-wine.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-writebroker-space: test-writebroker-space.c build-writebroker-space-test.py \
		writebroker-menu.h $(WRITEBROKER_DIR)/writebroker.c $(WINE_SRC)/dlls/ntdll/unix/file.c \
		$(WINE_SRC)/include/wine/ps5_writebroker.h $(WINE_SRC)/include/wine/ps5_title_private.h $(SHARED_GAME_HEADER) ../../include/product-wine.h
	python3 build-writebroker-space-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

# Explicitly opt-in: both complete 5 MB -> 50 MB, +520 byte traces (~2.38 TB
# logical per arm). Never add this workload to the ordinary host suite.
.PHONY: bench-writebroker-diff
bench-writebroker-diff: test-writebroker-diff-bench.py test-writebroker-diff-bench.c \
		test-writebroker-space.c build-writebroker-space-test.py $(WRITEBROKER_DIR)/writebroker.c
	python3 test-writebroker-diff-bench.py --wine "$(WINE_SRC)" --out "$(HOST_OUT)" --cc "$(HOST_CC)" --run-full

$(HOST_OUT)/test-wow-channels: test-wow-channels.c wow-channels.h forever-setup.h $(PREFIX_INIT_HEADER) sha256.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $(PREFIX_INIT_INCLUDES) $< -o $@

.PHONY: test-prefix-init
test-prefix-init: $(HOST_OUT)/test-forever-setup
	$(HOST_OUT)/test-forever-setup --prefix-init


.PHONY: test-registry-save
test-registry-save: $(HOST_OUT)/test-registry-save
	$(HOST_OUT)/test-registry-save

$(HOST_OUT)/test-registry-save: build-registry-save-test.py test-registry-save.c \
		build-desktop-surface-test.py $(WINE_SRC)/server/registry.c \
		$(WINE_SRC)/server/file.c $(WINE_SRC)/programs/wineboot/ps5-crypto.h \
		$(WINE_SRC)/include/wine/ps5_prefix_init.h
	python3 build-registry-save-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"
$(HOST_OUT)/test-forever-setup: test-forever-setup.c forever-setup.h $(PREFIX_INIT_HEADER) wow-channels.h sha256.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror $(PREFIX_INIT_INCLUDES) $< -o $@

$(HOST_OUT)/test-forever-exit: test-forever-exit.c build-forever-exit-test.py \
		build-desktop-surface-test.py native-entry.c ../../title/forever-menu.h
	python3 build-forever-exit-test.py --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-forever-menu: test-forever-menu.c forever-menu-settings.h forever-download.h ../../title/voice-model.h writebroker-menu.h \
		forever-setup.h $(PREFIX_INIT_HEADER) wow-channels.h sha256.h ../../title/forever-menu.c ../../title/forever-menu.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $(PREFIX_INIT_INCLUDES) test-forever-menu.c \
		../../title/forever-menu.c -o $@

$(HOST_OUT)/test-addonforge-client: test-addonforge-client.c \
		../../title/addonforge-client.c ../../title/addonforge-client.h \
		../../title/forever-menu.c ../../title/forever-menu.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
		test-addonforge-client.c ../../title/addonforge-client.c \
		../../title/forever-menu.c -o $@

$(HOST_OUT)/test-addonforge-worker: test-addonforge-worker.c build-addonforge-worker-test.py \
		native-entry.c wow-channels.h ../../title/addonforge-client.c ../../title/addonforge-client.h \
		../../title/forever-menu.c ../../title/forever-menu.h
	python3 build-addonforge-worker-test.py --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-addon-trash-native: test-addon-trash-native.c addon-trash-native.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror test-addon-trash-native.c -o $@

.PHONY: test-addon-worker-elf
test-addon-worker-elf: $(HOST_OUT)/test-addon-worker-elf
	$(HOST_OUT)/test-addon-worker-elf

$(HOST_OUT)/test-addon-worker-elf: test-addon-worker-elf.c addon-worker-elf.h \
		../../addon-worker/protocol.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@

.PHONY: test-addon-worker-request
test-addon-worker-request: $(HOST_OUT)/test-addon-worker-request
	$(HOST_OUT)/test-addon-worker-request

$(HOST_OUT)/test-addon-worker-request: test-addon-worker-request.c build-addon-worker-request-test.py \
		build-desktop-surface-test.py native-entry.c sha256.h ../../addon-worker/protocol.h
	python3 build-addon-worker-request-test.py --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-forever-menu-diag: test-forever-menu-diag.c ../../title/menu-diag.h \
		../../title/input.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@


$(HOST_OUT)/test-addonforge-security: test-addonforge-security.c \
		../../addonforge/addonforge-control.h ../../title/addonforge-security.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/addon-worker-%.o: $(ADDON_WORKER_DIR)/third_party/miniz/%.c \
		$(wildcard $(ADDON_WORKER_DIR)/third_party/miniz/*.h)
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -w -c $< -o $@

$(HOST_OUT)/test-addon-native-worker: test-addon-native-worker.cpp \
		$(ADDON_WORKER_DIR)/addon-worker.cpp $(ADDON_WORKER_DIR)/addon-worker.hpp \
		$(ADDON_WORKER_DIR)/protocol.h sha256.h $(ADDON_WORKER_HOST_OBJECTS)
	mkdir -p $(HOST_OUT)
	$(HOST_CXX) -std=c++20 -O2 -g -Wall -Wextra -Werror -pthread \
		test-addon-native-worker.cpp $(ADDON_WORKER_DIR)/addon-worker.cpp \
		$(ADDON_WORKER_HOST_OBJECTS) -o $@

$(HOST_OUT)/addon-worker-host: $(ADDON_WORKER_DIR)/main.cpp \
		$(ADDON_WORKER_DIR)/addon-worker.cpp $(ADDON_WORKER_DIR)/addon-worker.hpp \
		$(ADDON_WORKER_DIR)/protocol.h sha256.h $(ADDON_WORKER_HOST_OBJECTS)
	mkdir -p $(HOST_OUT)
	$(HOST_CXX) -std=c++20 -O2 -g -Wall -Wextra -Werror -pthread \
		$(ADDON_WORKER_DIR)/main.cpp $(ADDON_WORKER_DIR)/addon-worker.cpp \
		$(ADDON_WORKER_HOST_OBJECTS) -o $@

$(HOST_OUT)/test-title-icon: test-title-icon.c title-icon.h forever-setup.h $(PREFIX_INIT_HEADER) sha256.h third_party/stb/stb_image.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $(PREFIX_INIT_INCLUDES) $< -o $@ -lm

# Short tuning poll so the live log_async switch is exercised in milliseconds.
$(HOST_OUT)/test-title-log: test-title-log.c title-log.c title-log.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -DPW_TITLE_LOG_TUNE_POLL_MS=20 \
		test-title-log.c title-log.c -o $@

$(HOST_OUT)/test-battlenet-restart: test-battlenet-restart.c \
		$(WINE_SRC)/programs/explorer/battlenet_restart.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror \
		-I "$(WINE_SRC)/programs/explorer" $< -o $@

$(HOST_OUT)/test-runtime-input: test-runtime-input.c runtime-input.c ../../title/pointer.c \
		../../title/padmap.c ../../title/padmap.h \
		../../title/input.h ../../title/pointer.h ../../title/ime.h ../../title/cursor-arrow.h \
		../../title/voice.h ../../title/voice-state.h ../../title/voice-state.c \
		forever-menu-settings.h forever-setup.h $(PREFIX_INIT_HEADER) ../../title/forever-menu.h \
		$(WINE_SRC)/include/wine/ps5_title_input.h $(WINE_SRC)/include/wine/ps5_desktop.h \
		$(WINE_SRC)/include/wine/ps5_title_gamepad.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -I "$(WINE_SRC)/include" test-runtime-input.c ../../title/pointer.c ../../title/padmap.c ../../title/voice-state.c -o $@

$(HOST_OUT)/test-padmap: test-padmap.c ../../title/padmap.c ../../title/padmap.h ../../title/pointer.c ../../title/pointer.h ../../title/input.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror test-padmap.c ../../title/padmap.c ../../title/pointer.c -o $@

$(HOST_OUT)/test-maintenance-padmap: test-maintenance-padmap.c build-maintenance-padmap-test.py build-desktop-surface-test.py ../../title/main.c ../../title/padmap.c ../../title/padmap.h ../../title/pointer.c
	python3 build-maintenance-padmap-test.py --output "$@" --cc "$(HOST_CC)" --wine "$(WINE_SRC)"

$(HOST_OUT)/test-runescape-ssn: test-runescape-ssn.c runescape-ssn.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-runescape-launcher-retry: test-runescape-launcher-retry.c runescape-launcher-retry.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-runescape-install: test-runescape-install.c runescape-piece.h runescape-ssn.h \
		runescape-install.h runescape-redist.h forever-download.h sha256.h \
		../../addon-worker/third_party/miniz/miniz_tinfl.c $(wildcard ../../addon-worker/third_party/miniz/*.h)
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread $< ../../addon-worker/third_party/miniz/miniz_tinfl.c -o $@

$(HOST_OUT)/test-title-input: test-title-input.c ../../title/input.c ../../title/input.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-title-ime: test-title-ime.c ../../title/ime.c ../../title/ime.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $< -o $@

$(HOST_OUT)/test-title-ime-delivery: test-title-ime-delivery.c build-title-ime-delivery-test.py \
		build-desktop-surface-test.py ../../title/ime.c ../../title/ime.h ../../title/main.c \
		$(WINE_SRC)/include/wine/ps5_input.h $(WINE_SRC)/dlls/win32u/message.c \
		$(WINE_SRC)/server/queue.c $(WINE_SRC)/include/wine/server_protocol.h
	python3 build-title-ime-delivery-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-title-ime-delivery-diag: test-title-ime-delivery.c build-title-ime-delivery-test.py \
		build-desktop-surface-test.py ../../title/ime.c ../../title/ime.h ../../title/main.c \
		$(WINE_SRC)/include/wine/ps5_input.h $(WINE_SRC)/dlls/win32u/message.c \
		$(WINE_SRC)/server/queue.c $(WINE_SRC)/include/wine/server_protocol.h
	python3 build-title-ime-delivery-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)" --diagnostics

$(HOST_OUT)/test-title-voice: test-title-voice.c ../../title/voice-state.c ../../title/voice-state.h ../../title/input.h \
		forever-menu-settings.h forever-setup.h $(PREFIX_INIT_HEADER) ../../title/forever-menu.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror $(PREFIX_INIT_INCLUDES) test-title-voice.c ../../title/voice-state.c -o $@

$(HOST_OUT)/test-title-voice-overlay: test-title-voice-overlay.c ../../title/voice-overlay-raster.c \
		../../title/voice-overlay-raster.h ../../title/bitmap-font.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror test-title-voice-overlay.c ../../title/voice-overlay-raster.c -o $@ -lm

$(HOST_OUT)/test-title-voice-overlay-vulkan: test-title-voice-overlay-vulkan.c ../../title/voice-overlay-vulkan.c \
		../../title/voice-overlay-vulkan.h ../../title/voice-overlay-raster.c ../../title/voice-overlay-raster.h \
		../../title/bitmap-font.h ../../../vkd3d-proton/khronos/Vulkan-Headers/include/vulkan/vulkan_core.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
		-I ../../../vkd3d-proton/khronos/Vulkan-Headers/include \
		test-title-voice-overlay-vulkan.c ../../title/voice-overlay-raster.c -o $@

$(HOST_OUT)/test-runtime-imports: test-runtime-imports.py runtime_imports.py runtime_profiles.py \
		runtime-import-policy.json Makefile
	mkdir -p $(HOST_OUT)
	printf '%s\n' '#!/bin/sh' \
		'here=$$(CDPATH= cd -- "$$(dirname -- "$$0")" && pwd)' \
		'exec python3 "$$here/../../probes/psmkit-wine/test-runtime-imports.py" --cc "$${HOST_CC:-$(HOST_CC)}"' > $@
	chmod +x $@

$(HOST_OUT)/test-protocol: test-protocol.c protocol.c protocol.h $(IO) ../localproc/psmkit-io.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -DPW_P1C_FAULT_INJECTION test-protocol.c protocol.c $(IO) -o $@

$(HOST_OUT)/test-protocol-runescape: test-protocol.c protocol.c protocol.h runtime_profiles.py $(IO) ../localproc/psmkit-io.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -DPW_P1C_FAULT_INJECTION \
		-DPW_CHILDREN=$$(python3 -c 'from runtime_profiles import B1_CHILDREN; print(B1_CHILDREN)') test-protocol.c protocol.c $(IO) -o $@

$(HOST_OUT)/test-elf-runtime: test-elf-runtime.c elf-runtime.c elf-runtime.h sha256.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread test-elf-runtime.c -o $@

$(HOST_OUT)/test-wine-title: $(WINE_SRC)/loader/tests/ps5_title.c $(WINE_SRC)/include/wine/ps5_title.h $(WINE_SRC)/include/wine/ps5_title_private.h $(SHARED_GAME_HEADER) $(WINE_SRC)/include/wine/ps5_suspend.h $(WINE_SRC)/include/wine/ps5_desktop.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -pthread -I "$(WINE_SRC)/include" $< -o $@
$(HOST_OUT)/test-wine-title-exit: build-title-exit-test.py build-desktop-surface-test.py \
		$(WINE_SRC)/loader/tests/ps5_title_exit.c $(WINE_SRC)/dlls/ntdll/unix/server.c \
		$(WINE_SRC)/include/wine/ps5_title_private.h $(SHARED_GAME_HEADER)
	python3 build-title-exit-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"
$(HOST_OUT)/test-write-buffer: test-write-buffer.c build-write-buffer-test.py ../../include/product-wine.h \
		$(WINE_SRC)/dlls/ntdll/unix/file.c $(WINE_SRC)/dlls/ntdll/unix/server.c \
		$(WINE_SRC)/include/wine/ps5_title_private.h $(SHARED_GAME_HEADER)
	python3 build-write-buffer-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-request-profile: test-request-profile.c \
		$(WINE_SRC)/include/wine/ps5_request_profile.h $(WINE_SRC)/include/wine/server_protocol.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror -I "$(WINE_SRC)/include" $< -o $@

$(HOST_OUT)/test-runtime-profile-callbacks: test-runtime-profile-callbacks.c runtime-profile.h \
		$(WINE_SRC)/include/wine/ps5_suspend.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I "$(WINE_SRC)/include" $< -o $@

$(HOST_OUT)/test-direct-commit: test-direct-commit.c build-direct-commit-test.py \
		$(WINE_SRC)/dlls/ntdll/unix/ps5_direct.c
	python3 build-direct-commit-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-direct-reserve: test-direct-reserve.c build-direct-reserve-test.py \
		$(WINE_SRC)/dlls/ntdll/unix/ps5_direct.c $(WINE_SRC)/dlls/ntdll/unix/virtual.c
	python3 build-direct-reserve-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-runescape-vaprobe: test-runescape-vaprobe.c runescape-vaprobe.h
	mkdir -p $(HOST_OUT)
	$(HOST_CC) -std=gnu11 -O2 -g -Wall -Wextra -Werror -I "$(PLATFORM_SRC)/include" $< -o $@

$(HOST_OUT)/test-direct-cache: test-direct-cache.c build-direct-cache-test.py \
		$(WINE_SRC)/dlls/ntdll/unix/virtual.c $(WINE_SRC)/dlls/ntdll/unix/ps5_direct.c
	python3 build-direct-cache-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/test-direct-fail-fast: test-direct-fail-fast.c build-direct-cache-test.py \
		$(WINE_SRC)/dlls/ntdll/unix/virtual.c $(WINE_SRC)/dlls/ntdll/unix/ps5_direct.c
	python3 build-direct-cache-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)" --fail-fast-test

$(HOST_OUT)/test-vm-cpu: test-vm-cpu.c build-direct-cache-test.py \
		$(WINE_SRC)/dlls/ntdll/unix/virtual.c
	python3 build-direct-cache-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)" --cpu-test


$(HOST_OUT)/test-loader-path: test-loader-path.c build-path-test.py $(WINE_SRC)/loader/main.h $(PLATFORM_SRC)/src/libc.c $(PLATFORM_SRC)/include/ps5platform/libc.h
	mkdir -p $(HOST_OUT)
	python3 build-path-test.py --platform "$(PLATFORM_SRC)" --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

# Compile complete production functions on the host, for both platform guards.
test-startup-routing: $(HOST_OUT)/test-startup-routing $(HOST_OUT)/non-ps5/test-startup-routing
	$(HOST_OUT)/test-startup-routing
	$(HOST_OUT)/non-ps5/test-startup-routing

$(HOST_OUT)/test-startup-routing: test-startup-routing.c build-startup-routing-test.py $(WINE_SRC)/dlls/ntdll/unix/loader.c $(WINE_SRC)/dlls/ntdll/unix/env.c
	python3 build-startup-routing-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)"

$(HOST_OUT)/non-ps5/test-startup-routing: test-startup-routing.c build-startup-routing-test.py $(WINE_SRC)/dlls/ntdll/unix/loader.c $(WINE_SRC)/dlls/ntdll/unix/env.c
	python3 build-startup-routing-test.py --wine "$(WINE_SRC)" --output "$@" --cc "$(HOST_CC)" --non-ps5

$(OUT)/eboot.bin: parent.c protocol.c protocol.h $(IO) ../localproc/probe.h $(TITLE_BUILD) $(TITLE_PARAM)
	WINE_SRC="$(WINE_SRC)" PS5_TITLE_RADV= PS5_TITLE_WORK="$(abspath $(WORK))" \
		bash $(TITLE_BUILD) $(OUT) parent.c protocol.c $(IO)

$(CHILD_OUT)/eboot.bin: child.c protocol.c protocol.h $(IO) $(PATCH) $(TITLE_BUILD) $(TITLE_PARAM)
	WINE_SRC="$(WINE_SRC)" PS5_TITLE_RADV= PS5_TITLE_WORK="$(abspath $(CHILD_WORK))" \
		bash $(TITLE_BUILD) $(CHILD_OUT) child.c protocol.c $(IO)
	python3 $(PATCH) $(CHILD_WORK)/eboot.elf $(CHILD_WORK)/localproc.elf
	$(NATIVE_TOOL) self --sign --in $(CHILD_WORK)/localproc.elf --out $@ --magic 0x1D3D154F
	python3 $(PATCH) --container $@ $@
	$(NATIVE_TOOL) self --inspect --file $@

$(OUT)/psmkit-wine-descriptor-child.bin: $(CHILD_OUT)/eboot.bin $(OUT)/eboot.bin
	cp $(CHILD_OUT)/eboot.bin $@

$(PE_OUT)/p1w-parent.exe: p1w-parent.c
	mkdir -p $(PE_OUT)
	$(MINGW32_CC) -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -nostdlib \
		-Wl,--subsystem,console -Wl,--entry,mainCRTStartup -o $@ $< -lkernel32 -luser32

$(PE_OUT)/p1w-child.exe: p1w-child.c
	mkdir -p $(PE_OUT)
	$(MINGW32_CC) -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -nostdlib \
		-Wl,--subsystem,console -Wl,--entry,mainCRTStartup -o $@ $< -lkernel32 -luser32

$(PE_OUT)/audio-tone.exe: audio-tone.c
	mkdir -p $(PE_OUT)
	$(MINGW_CC) -O2 -Wall -Wextra -Werror -o $@ $< -lwinmm

$(PE_OUT)/%.exe: %.c
	mkdir -p $(PE_OUT)
	$(MINGW_CC) -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -nostdlib \
		-Wl,--subsystem,console -Wl,--entry,mainCRTStartup -o $@ $< -lkernel32
