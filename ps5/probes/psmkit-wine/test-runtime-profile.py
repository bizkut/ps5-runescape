#!/usr/bin/env python3
"""Host-only path, manifest and guarded-write tests for the B1 title profile."""
import hashlib
import importlib.util
import shutil
import json
from pathlib import Path
import tempfile

from runtime_profiles import (
    B1_CHILDREN, ADDON_WORKER_MAX_BYTES, ADDON_WORKER_RELATIVE, WRITEBROKER_MAX_BYTES,
    WRITEBROKER_RELATIVE, profile, installed_paths, runtime_tree_manifest,
    b1_guard_paths, qualified_b1_folder, file_identity, builtin_prefix_files,
    REDIST_DLLS,
)
from runtime_imports import (
    candidate_ownership, module_manifest, render_include, load_policy, make_records,
    native_role_imports,
)

with tempfile.TemporaryDirectory() as directory:
    tree = Path(directory)
    builtin = b'MZ' + bytes(62) + b'Wine builtin DLL\0' + bytes(16)
    for arch in ('x86_64', 'i386'):
        folder = tree / f'lib/wine/{arch}-windows'
        folder.mkdir(parents=True)
        for name in ('advapi32.dll', 'kernel32.dll', 'wineps5audio.drv', 'start.exe', 'dxgi.dll',
                     'msvcp120.dll', 'msvcp140_unknown.dll', *REDIST_DLLS):
            (folder / name).write_bytes(builtin)
        (folder / 'not-pe.txt').write_bytes(b'not a builtin PE')
        (folder / 'amd_ags_x64.dll').write_bytes(builtin)
    owned = dict(builtin_prefix_files(tree))
    for arch, folder in (('x86_64', 'system32'), ('i386', 'syswow64')):
        for name in ('advapi32.dll', 'kernel32.dll', 'wineps5audio.drv', 'start.exe'):
            assert owned[f'lib/wine/{arch}-windows/{name}'] == f'drive_c/windows/{folder}/{name}'
    assert not any('not-pe.txt' in name or 'amd_ags_x64.dll' in name for name in owned)
    for arch in ('x86_64', 'i386'):
        assert all(f'lib/wine/{arch}-windows/{name}' not in owned for name in REDIST_DLLS)
        for name in ('msvcp120.dll', 'msvcp140_unknown.dll'):
            assert f'lib/wine/{arch}-windows/{name}' in owned
    assert list(owned) == sorted(owned)

b1 = profile('b1')
p1w = profile('p1w')
product = json.loads(Path(__file__).with_name('b1-product.json').read_text())
assert product['debug'] == 'err+all,trace+loaddll,trace+seh'
assert product['argv'] == [r'C:\RuneScape\runescape-bootstrap.exe']
assert b1['argv'] == [b1['loader'], *product['argv']]
game_config = json.loads(Path(__file__).with_name('b1-game-osrs.json').read_text())
maintenance = profile('b1', product)
assert maintenance['refused_programs'] == ['tabtip.exe', 'winemenubuilder.exe']
# Suppress only already-disabled helpers; explorer owns Wine's desktop.
assert 'explorer.exe' not in maintenance['refused_programs']
assert not any('vcr' in name.lower() or 'msiexec' in name.lower()
               for name in maintenance['refused_programs'])
# The maintenance desktop publishes CPU DIBs, not an ANGLE/D3D swapchain.
# Select Chromium's display-compositor mode without hardware or software GL.
launcher_switches = maintenance['browser_switches']['JagexLauncher.exe']
for switch in ('--disable-gpu', '--disable-gpu-compositing', '--disable-software-rasterizer'):
    assert switch in launcher_switches, switch
assert '--in-process-gpu' in launcher_switches
assert '--disable-features=AudioServiceOutOfProcess' in launcher_switches
assert '--enable-features=NetworkServiceInProcess2' in launcher_switches
assert not any(switch.startswith('--use-angle=') for switch in launcher_switches)
assert not any(host.endswith('.skin') for host in maintenance['prefetch_hosts'])
runtime_allowlist = (Path(__file__).resolve().parents[2] / 'release/runtime-files.txt').read_text().splitlines()
assert 'wine-title/runescape/runescape-bootstrap.exe' in runtime_allowlist
assert 'wine-title/runescape/DigiCertTrustedRootG4.der' in runtime_allowlist
assert not any(path.startswith('wine-title/addonforge/') for path in runtime_allowlist)
assert 'wine-title/share/forever/Config.wtf' not in runtime_allowlist
assert 'B1_GAME_PROFILE_CONFIG ?= $(CURDIR)/b1-game-osrs.json' in (
    Path(__file__).with_name('Makefile').read_text())
game = profile('b1', game_config)
assert 'argv' not in game_config and 'cwd' not in game_config
assert game['argv'] == [game['loader']]
assert game['cwd'] is None and game['runtime_game'] is True
assert game['prefix'] == '/data/runescape/prefix'
assert game['home'] == '/data/runescape/home'
assert game['gpu'] is True and game['headless'] is True
assert game['refused_programs'] == [] and game['prefetch_hosts'] == []
assert game['environment'] == {'GALLIUM_DRIVER': 'zink'} and game['browser_switches'] == {}
assert 'd3d9,d3d10core,d3d11,dxgi=n' in game['dll_overrides']
assert 'PROTON_ENABLE_HDR' not in game['environment']
assert 'opengl32=n;vulkan-1=b' in game['dll_overrides']
for dll in ('opengl32.dll', 'libgallium_wgl.dll'):
    assert f'wine-title/share/forever/system32/{dll}' in runtime_allowlist
for dll in ('d3d9.dll', 'd3d10core.dll', 'd3d11.dll', 'dxgi.dll'):
    assert f'wine-title/share/forever/system32/{dll}' in runtime_allowlist
assert set(b1['module_paths']) == {
    'ntdll.so', 'win32u.so', 'wineserver.so', 'ws2_32.so', 'crypt32.so', 'opengl32.so',
    'secur32.so', 'bcrypt.so', 'libgnutls.so', 'wineps5.so', 'dwrite.so', 'nsi.so', 'libfreetype.so',
}
assert b1['headless'] is False
assert b1['gpu'] is False
assert 'gpu' not in p1w
assert profile('b1', {'gpu': False})['gpu'] is False
assert profile('b1', {'gpu': True, 'headless': True})['gpu'] is True
gpu = profile('b1', {'headless': True, 'gpu': True})
assert gpu['gpu'] is True
assert gpu['module_paths'] == {**b1['module_paths'],
                               'wineps5audio.so': '/app0/wine-title/lib/wine/x86_64-unix/wineps5audio.so',
                               'winevulkan.so': '/app0/wine-title/lib/wine/x86_64-unix/winevulkan.so',
                               **{f'xinput1_{v}.so': f'/app0/wine-title/lib/wine/x86_64-unix/xinput1_{v}.so'
                                  for v in (1, 2, 3, 4)}}
assert 'winevulkan.so' not in profile('b1', {'headless': True})['module_paths']
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'PROTON_ENABLE_HDR': '1'}})['environment'] == {'PROTON_ENABLE_HDR': '1'}
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'WINEPS5_HDR_TRACE': '1'}})['environment'] == {'WINEPS5_HDR_TRACE': '1'}
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'WINEPS5_QUERY_TRACE': '1'}})['environment'] == {
                          'WINEPS5_QUERY_TRACE': '1'}
for direct in ('0', '1'):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {'WINEPS5_DIRECT_REQUESTS': direct}})['environment'] == {
                              'WINEPS5_DIRECT_REQUESTS': direct}
for log_async in ('0', '1'):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {'WINEPS5_LOG_ASYNC': log_async}})['environment'] == {
                              'WINEPS5_LOG_ASYNC': log_async}
for trace_name in ('WINEPS5_MXCSR_TRACE', 'WINEPS5_WRITE_TRACE', 'WINEPS5_WRITE_STICKY',
                   'WINEPS5_PROFILE'):
    for trace_value in ('0', '1'):
        for gpu_enabled in (False, True):
            assert profile('b1', {'gpu': gpu_enabled, 'headless': gpu_enabled,
                                  'environment': {trace_name: trace_value}})['environment'] == {
                                      trace_name: trace_value}
# Profiling is not a graphics feature: its startup assignment must run before
# the GPU-only environment branch as well as being admitted by profile().
native_startup = Path(__file__).with_name('native-entry.c').read_text()
common_environment = native_startup.split('if (native_runtime.payload_spawn', 1)[1].split(
    '#if PW_RUNTIME_GPU', 1)[0]
assert 'setenv("WINEPS5_PROFILE", PW_RUNTIME_PROFILE, 0)' in common_environment
# R2 publishes both maintenance interception callbacks. Only explicit launcher
# Play commands enter the game role; no persisted credentials/direct replay.
startup_main = native_startup.split('int main(int argc, char **argv)', 1)[1]
setup_gate = startup_main.index('if (pw_forever_start())')
game_wine_entry = startup_main.index('wine_public_title_main(parent_argc,args)')
assert 'native_runtime.game_launch = pw_child_game_launch' in startup_main
assert setup_gate < game_wine_entry
assert 'pw_maintenance_direct_game()' not in startup_main
assert 'pw_title_icon_start();' not in startup_main
assert 'forever_addon_trash_start();' not in startup_main
assert 'native_runtime.game_launch = pw_game_launch' in startup_main
assert 'title_desktop_installer_mode(0)' in startup_main
assert 'setenv("WINEPS5_BATTLENET_WATCH","1"' not in startup_main
sync_stage = native_startup.split('static int pw_forever_start(void)', 1)[1].split(
    '#if !PW_RUNTIME_GPU\nstatic', 1)[0]
assert 'forever_install_wow_config(' not in sync_stage
assert 'result = pw_forever_sync_managed();' in sync_stage
assert sync_stage.index('result = pw_forever_sync_managed();') < sync_stage.index(
    'if (!forever_prefix_ready(FOREVER_PREFIX))')
assert setup_gate < startup_main.index('result=wine_public_title_main(3, wineboot_args);')
assert setup_gate < startup_main.index('result=wine_public_title_main(PW_RUNTIME_PARENT_ARGC, args);')
build_source = Path(__file__).with_name('build-runtime.py').read_text()
assert B1_CHILDREN == 7
b1_defines = build_source.split("if args.profile == 'b1':\n    profile_defines +=", 1)[1].split(
    "wrapper = work / 'native-entry-generated.c'", 1)[0]
assert r'#define PW_RUNTIME_RUNESCAPE 1\n#define PW_CHILDREN {B1_CHILDREN}\n' in b1_defines
# The protocol's slot loop must have the same bound as the supervisor array.
protocol_wrapper = build_source.split("parent_protocol.write_text(", 1)[1].split(
    "parent_supervisor =", 1)[0]
assert "profile_defines + '#define PW_P1C_FAULT_INJECTION 1" in protocol_wrapper
assert "for name in ('runescape-bootstrap.exe', 'DigiCertTrustedRootG4.der'):" in build_source
assert "owned[f'drive_c/RuneScape/{name}'] = f'runescape/{name}'" in build_source
assert build_source.index("owned[f'drive_c/RuneScape/{name}']") < build_source.index(
    "for destination, source in sorted(owned.items()):")
assert 'if (result < 0)' in sync_stage.split('result = pw_forever_sync_managed();', 1)[1]
assert 'return -1;' in sync_stage.split('io_log("forever: app-owned prefix files', 1)[1]
assert 'forever: prefix sync checked=%u hashed=%u repaired=%u' in native_startup
assert 'pw_game_record_launch(' not in native_startup
assert 'pw_game_direct_launch(' not in native_startup
assert 'launch->environment, launch->environment_bytes' in native_startup
assert 'native_runtime.parent_environment = game_environment' in native_startup
assert 'game exit returning to maintenance path=/app0/eboot.bin' in native_startup
supervisor_source = Path(__file__).with_name('runtime-parent.c').read_text()
assert 'pw_game_wire_decode((int)request.argc, argv, PW_RUNTIME_PREFIX,' in supervisor_source
assert 'pw_parent_stage_game_launch(argv + PW_GAME_PROGRAM, argv[5], argv[3],' in supervisor_source
assert 'environment, environment_bytes' in supervisor_source
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'PROTON_ENABLE_HDR': '1', 'WINEPS5_DIRECT_REQUESTS': '0',
                                      'WINEPS5_HDR_TRACE': '1', 'WINEPS5_QUERY_TRACE': '1'}})['environment'] == {
                          'PROTON_ENABLE_HDR': '1', 'WINEPS5_DIRECT_REQUESTS': '0',
                          'WINEPS5_HDR_TRACE': '1', 'WINEPS5_QUERY_TRACE': '1'}
for flag in ('WINEPS5_CHUNKED_RESERVE', 'WINEPS5_VAPROBE'):
    assert flag not in b1.get('environment', {})
    for setting in ('0', '1'):
        assert profile('b1', {'environment': {flag: setting}})['environment'] == {flag: setting}
    for setting in (0, 1, 'yes', '', None):
        try:
            profile('b1', {'environment': {flag: setting}})
        except ValueError:
            pass
        else:
            raise AssertionError(f'B1 accepted invalid reserve flag {flag}={setting!r}')
assert maintenance['environment'] == {'WINEPS5_VAPROBE': '0', 'WINEPS5_CHUNKED_RESERVE': '1'}
for name, setting in (
        ('WINEPS5_PROFILE', '1'),
        ('PS5_VIDEOOUT_STATS', '1'),
        ('VKD3D_CONFIG', 'single_queue'),
        ('VKD3D_CONFIG', 'single_queue,no_staggered_submit'),
        ('VKD3D_QUEUE_PROFILE', r'W:\vkd3d-queue-profile.json'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'FIFO'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'IMMEDIATE'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'MAILBOX'),
        ('WINEPS5_SDR_WHITE_NITS', '80'),
        ('WINEPS5_SDR_WHITE_NITS', '203'),
        ('WINEPS5_SDR_WHITE_NITS', '480'),
        ('WINEPS5_HDR_MODE', 'mode=off'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=80'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=637'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=10000'),
        ('WINEPS5_HDR_MODE', 'mode=sdr'),
        ('WINEPS5_HDR_MODE', 'mode=sdr paper=203 peak=637 expand=1 sat=1.0 gamma=2.2 key=y'),
        ('WINEPS5_HDR_MODE', 'mode=sdr gamma=srgb key=max'),
        ('WINEPS5_HDR_MODE', 'mode=auto sdr_shaders=52596c4112cfc065 '
                             'hdr_shaders=c2d9013c5a1bd61e paper=203 peak=637 '
                             'expand=0.4 sat=1.0 gamma=2.6 key=y'),
        ('WINEPS5_HDR_MODE', 'mode=auto sdr_shaders=52596c4112cfc065,0123456789abcdef '
                             'hdr_shaders=c2d9013c5a1bd61e hdr_cap=637')):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {name: setting}})['environment'] == {name: setting}
game_mode = profile('b1', {'gpu': True, 'headless': True,
                           'child_dll_overrides': 'vulkan-1=n,b;dxgi=b'})
assert game_mode['child_dll_overrides'] == 'vulkan-1=n,b;dxgi=b'
assert b1['child_dll_overrides'] == b1['dll_overrides']
for setting in ({'DXVK_HDR': '1'}, {'PATH': '/app0'}, {'PROTON_ENABLE_HDR': 1},
                {'PROTON_ENABLE_HDR': 'yes'}, {'WINEPS5_DIRECT_REQUESTS': 0},
                {'WINEPS5_DIRECT_REQUESTS': 'yes'}, {'WINEPS5_HDR_TRACE': 1},
                {'WINEPS5_HDR_TRACE': 'yes'}, {'WINEPS5_QUERY_TRACE': 1},
                {'WINEPS5_QUERY_TRACE': 'yes'}, {'PS5_VIDEOOUT_STATS': '0'},
                {'WINEPS5_PROFILE': 1}, {'WINEPS5_PROFILE': 'yes'},
                {'WINEPS5_MXCSR_TRACE': 1}, {'WINEPS5_MXCSR_TRACE': 'yes'},
                {'WINEPS5_WRITE_TRACE': 1}, {'WINEPS5_WRITE_TRACE': 'yes'},
                {'WINEPS5_WRITE_STICKY': 1}, {'WINEPS5_WRITE_STICKY': 'yes'},
                {'VKD3D_CONFIG': 'no_staggered_submit'},
                {'VKD3D_QUEUE_PROFILE': '/tmp/profile.json'},
                {'VKD3D_SWAPCHAIN_PRESENT_MODE': 'FIFO_RELAXED'},
                {'WINEPS5_HDR_MAX_NITS': '637'}, {'WINEPS5_HDR_MODE': 637},
                {'WINEPS5_SDR_WHITE_NITS': 203},
                {'WINEPS5_SDR_WHITE_NITS': '79'},
                {'WINEPS5_SDR_WHITE_NITS': '481'},
                {'WINEPS5_SDR_WHITE_NITS': '203.0'},
                {'WINEPS5_SDR_WHITE_NITS': '0203'},
                {'WINEPS5_HDR_MODE': 'sdr'}, {'WINEPS5_HDR_MODE': 'mode=cap'},
                {'WINEPS5_HDR_MODE': 'mode=cap max=79'},
                {'WINEPS5_HDR_MODE': 'mode=cap max=10001'},
                {'WINEPS5_HDR_MODE': 'mode=off peak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr paper=638 peak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr expand=1.1'},
                {'WINEPS5_HDR_MODE': 'mode=sdr sat=2.1'},
                {'WINEPS5_HDR_MODE': 'mode=sdr gamma=0.9'},
                {'WINEPS5_HDR_MODE': 'mode=sdr key=red'},
                {'WINEPS5_HDR_MODE': 'mode=sdr\npeak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr sdr_shaders=52596c4112cfc065'},
                {'WINEPS5_HDR_MODE': 'mode=auto hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 hdr_shaders=short'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065,52596c4112cfc065 '
                                     'hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 '
                                     'hdr_shaders=52596c4112cfc065'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=0000000000000001,0000000000000002,'
                                     '0000000000000003,0000000000000004,0000000000000005 '
                                     'hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 '
                                     'hdr_shaders=c2d9013c5a1bd61e hdr_cap=79'},
                [], None):
    try:
        profile('b1', {'gpu': True, 'headless': True, 'environment': setting})
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 accepted unsafe environment: {setting}')
try:
    profile('b1', {'environment': {'PROTON_ENABLE_HDR': '1'}})
except ValueError:
    pass
else:
    raise AssertionError('product profile accepted a GPU environment')
assert 'headless' not in p1w
assert 'payload_programs' not in p1w and 'payload_dll_overrides' not in p1w
assert b1['payload_programs'] == []
assert b1['refused_programs'] == []
assert 'refused_programs' not in p1w
assert profile('b1', {'refused_programs': ['BlizzardError.exe']})['refused_programs'] == [
    'BlizzardError.exe']
assert b1['installer_dos'] == r'C:\Jagex Launcher Installer.exe'
assert b1['installer_url'] == 'https://files.publishing.production.jxp.jagex.com/installer/Jagex%20Launcher%20Installer.exe'
assert 'installer_fallback_url' not in b1
assert b1['installer_args'] == ['--silent', '--install']
assert b1['publisher'] == 'Jagex Limited'
assert maintenance['startup_mode'] == 'ssn'
for mode in ('ssn', 'installer-trial', 'launcher-trial'):
    assert profile('b1', {'startup_mode': mode})['startup_mode'] == mode
for mode in ('silent', '', None, 1):
    try:
        profile('b1', {'startup_mode': mode})
    except ValueError:
        pass
    else:
        raise AssertionError(f'accepted unknown startup mode {mode}')
for key in ('installer_url', 'installer_dos', 'launcher_unix', 'installer_unix', 'installer_cwd',
            'publisher', 'installer_args', 'launcher_dos', 'launcher_directory_dos'):
    assert key not in p1w
    try:
        profile('b1', {key: b1[key]})
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 profile accepted overridable installer field: {key}')
relocated = profile('b1', {'prefix': '/app0/wine/prefix'})
assert relocated['installer_unix'] == '/app0/wine/prefix/drive_c/Jagex Launcher Installer.exe'
assert relocated['launcher_unix'] == '/app0/wine/prefix/drive_c/Program Files/Jagex Launcher/JagexLauncher.exe'
assert relocated['installer_cwd'] == '/app0/wine/prefix/drive_c'
assert profile('b1', {'payload_programs': [r'C:\ProgramData\Battle.net\Agent\Agent.exe']})['payload_programs'] == [
    r'C:\ProgramData\Battle.net\Agent\Agent.exe']
assert ('"WINEDLLOVERRIDES=' + b1['payload_dll_overrides'] + '"') in (
    Path(__file__).resolve().parents[2] / 'deploy-wine.py').read_text()
assert profile('b1', {'payload_programs': [r'D:\Tools\Switcher.exe']})['payload_programs'] == [r'D:\Tools\Switcher.exe']
for headless in (False, True):
    roles = native_role_imports('b1', headless=headless)
    assert all(roles['parent'][name] == 'libkernel.prx'
               for name in ('connect', 'setsockopt', 'shutdown'))
    assert roles['parent']['sceSystemServiceLaunchWebBrowser'] == 'libSceSystemService.prx'
    assert roles['parent']['sceUserServiceInitialize'] == 'libSceUserService.prx'
    assert roles['child'] == {}
assert all(name not in native_role_imports('p1w')['parent']
           for name in ('connect', 'setsockopt', 'shutdown'))
assert 'sceSystemServiceLaunchWebBrowser' not in native_role_imports('p1w')['parent']
gpu_roles = native_role_imports('b1', headless=True, gpu=True)
assert gpu_roles['child'] == native_role_imports('b1', headless=True)['child']
# Mapping defaults must use the existing native title filesystem surface,
# not acquire WebKit POSIX or new kernel_sys imports for temp/hard-link helpers.
for mapping_roles in (native_role_imports('b1', headless=False), gpu_roles):
    for imports in mapping_roles.values():
        assert not {'mkstemp', 'link'} & imports.keys()
        assert not {'libScePosixForWebKit.prx', 'libkernel_sys.prx'} & set(imports.values())
assert gpu_roles['parent']['sceAgcInit'] == 'libSceAgc.prx'
assert gpu_roles['parent']['sceAgcDriverSubmitDcb'] == 'libSceAgcDriver.prx'
assert gpu_roles['parent']['sceVideoOutOpen'] == 'libSceVideoOut.prx'
for symbol in ('sceAudioOutInit', 'sceAudioOutOpen', 'sceAudioOutOutput', 'sceAudioOutClose'):
    assert gpu_roles['parent'][symbol] == 'libSceAudioOut.prx'
    assert symbol not in gpu_roles['child']
    assert symbol not in native_role_imports('b1', headless=False)['parent']
    assert symbol not in native_role_imports('b1', headless=True)['parent']
for symbol in ('sceVideoOutCursorDisable', 'sceVideoOutCursorEnable',
               'sceVideoOutCursorSetHotSpot', 'sceVideoOutCursorSetPosition'):
    assert gpu_roles['parent'][symbol] == 'libSceVideoOut.prx'
    assert symbol not in gpu_roles['child']
    assert symbol not in native_role_imports('b1', headless=False)['parent']
    assert symbol not in native_role_imports('b1', headless=True)['parent']
for symbol, provider in (
        ('scePadRead', 'libScePad.prx'),
        ('sceKeyboardClose', 'libSceKeyboard.prx'),
        ('sceKeyboardRead', 'libSceKeyboard.prx'),
        ('sceKeyboardReadState', 'libSceKeyboard.prx'),
        ('sceKeyboardSetProcessPrivilege', 'libSceKeyboard.prx'),
        ('sceMouseRead', 'libSceMouse.prx'),
        ('sceUserServiceGetLoginUserIdList', 'libSceUserService.prx'),
        ('sceImeOpen', 'libSceIme.prx')):
    assert gpu_roles['parent'][symbol] == provider
    assert native_role_imports('b1', headless=False)['parent'][symbol] == provider
    assert symbol not in native_role_imports('b1', headless=True)['parent']
    assert symbol not in gpu_roles['child']
assert gpu_roles['parent']['scePadSetVibration'] == 'libScePad.prx'
assert 'scePadSetVibration' not in gpu_roles['child']
for roles in (native_role_imports('b1'), native_role_imports('b1', headless=True),
              native_role_imports('p1w')):
    assert 'scePadSetVibration' not in roles['parent']
assert not any(name.startswith('xinput') for name in b1['module_paths'])
assert not any(name.startswith('xinput') for name in p1w['module_paths'])
assert 'sceHttp2Init' not in gpu_roles['parent']
for roles in (native_role_imports('b1'), native_role_imports('b1', headless=True),
              native_role_imports('p1w')):
    assert 'sceAgcInit' not in roles['parent']
    assert 'sceAgcDriverSubmitDcb' not in roles['parent']
voice_symbols = {'sceAudioInInit', 'sceAudioInOpen', 'sceAudioInInput', 'sceAudioInClose'}
assert {name for name in gpu_roles['parent'] if name.startswith('sceAudioIn')} == voice_symbols
assert all(gpu_roles['parent'][name] == 'libSceAudioIn.prx' for name in voice_symbols)
assert gpu_roles['parent']['scePthreadSetprio'] == 'libkernel.prx'
for roles in (native_role_imports('b1'), native_role_imports('b1', headless=True),
              native_role_imports('p1w')):
    assert not voice_symbols.intersection(roles['parent'])
    assert not voice_symbols.intersection(roles['child'])
assert not voice_symbols.intersection(gpu_roles['child'])
for symbol in ('sceHttp2AbortRequest', 'sceHttp2AddRequestHeader', 'sceHttp2GetAllResponseHeaders',
               'sceHttp2SetAutoRedirect', 'sceHttp2SetResolveTimeOut',
               'sceHttp2SetConnectTimeOut', 'sceHttp2SetRecvTimeOut'):
    assert native_role_imports('b1')['parent'][symbol] == 'libSceHttp2.prx'
    assert symbol not in gpu_roles['parent'] and symbol not in gpu_roles['child']
assert len(p1w['module_paths']) == 3
reviewed = load_policy(Path(__file__).with_name('runtime-import-policy-b1.json'), profile_name='b1')
for symbol in ('NtUserGetForegroundWindow', 'NtUserGetGUIThreadInfo', 'NtUserPostMessage', 'NtUserQueryWindow'):
    assert any(row == {'module': 'wineps5.so', 'binding': 'strong', 'owner': 'title_win32u',
                       'symbol': symbol, 'target': symbol} for row in reviewed)
gpu_surfaces = {name: {'strong': set(), 'weak': set()} for name in gpu['module_paths']}
for row in reviewed:
    gpu_surfaces[row['module']][row['binding']].add(row['symbol'])
assert any('winevulkan.so' in row['modules'] for row in make_records(gpu_surfaces, reviewed, profile_name='b1', gpu=True))
assert any('wineps5audio.so' in row['modules']
           for row in make_records(gpu_surfaces, reviewed, profile_name='b1', gpu=True))
assert any(row['name'] == 'ps5_title_profile_set_blocking' and row['owner'] == 'title_ntdll'
           and set(row['modules']) == {'wineps5audio.so', 'winevulkan.so'}
           for row in make_records(gpu_surfaces, reviewed, profile_name='b1', gpu=True))
assert gpu_roles['parent']['pthread_getcpuclockid'] == 'libkernel.prx'
try:
    make_records(gpu_surfaces, reviewed, profile_name='b1')
except ValueError:
    pass
else:
    raise AssertionError('a launcher build accepted the GPU module')
surfaces = {name: gpu_surfaces[name] for name in b1['module_paths']}
authorized = make_records(surfaces, reviewed, profile_name='b1')
assert not any('winevulkan.so' in row['modules'] for row in authorized)
assert not any('wineps5audio.so' in row['modules'] for row in authorized)
assert any(row['owner'] == 'title_win32u' for row in authorized)
assert any(row['name'] == 'getaddrinfo' and row['target'] == 'pw_b1_getaddrinfo' for row in authorized)
# B1 klog writes leave on the native writer thread (title-log.c); p1w is unchanged.
assert candidate_ownership('sceKernelDebugOutText', profile_name='b1') == ('platform', 'pw_title_klog_text')
assert candidate_ownership('sceKernelDebugOutText') == ('UNREVIEWED_NOT_AUTHORIZED', 'sceKernelDebugOutText')
assert {row['module'] for row in reviewed if row['symbol'] == 'sceKernelDebugOutText'} == {
    'nsi.so', 'ntdll.so', 'win32u.so', 'wineps5.so', 'wineserver.so'}
assert all(row['target'] == 'pw_title_klog_text' for row in authorized if row['name'] == 'sceKernelDebugOutText')
surfaces['ws2_32.so']['strong'].add('future_unreviewed_network_function')
try:
    make_records(surfaces, reviewed, profile_name='b1')
except ValueError:
    pass
else:
    raise AssertionError('B1 import policy accepted a new resolver import')
assert b1['root'] == '/app0/wine-title'
assert installed_paths(b1['loader']) == {
    'dll_dir': '/app0/wine-title/lib/wine',
    'bin_dir': '/app0/wine-title/bin',
    'data_dir': '/app0/wine-title/share/wine',
    'wineloader': b1['loader'],
}
assert b1['prefix'] == '/app0/wine/tests/b1-trace/prefix'
assert b1['home'] == '/app0/wine/tests/b1-trace/home'
assert b1['server'] == '/app0/wine-title/server-title'
assert p1w['loader'] == '/app0/wine/lib/wine/x86_64-unix/wine'
assert p1w['module_paths']['wineserver.so'] == '/app0/wine/psmkit-wine-disposable/bin/wineserver.so'
custom = profile('b1', {'argv': ['start.exe', '/exec', 'custom.exe', '--disable-gpu'],
                        'cwd': '/app0/custom', 'prefix': '/app0/wine/custom-prefix',
                        'home': '/app0/wine/custom-home', 'dll_overrides': 'wineps5.drv=d',
                        'headless': True, 'payload_programs': []})
assert custom['argv'] == [b1['loader'], 'start.exe', '/exec', 'custom.exe', '--disable-gpu']
assert custom['module_paths'] == b1['module_paths'] and custom['root'] == b1['root']
assert custom['headless'] is True
assert custom['payload_programs'] == []
assert custom['payload_dll_overrides'] == b1['payload_dll_overrides']
for invalid in ({'root': '/app0/wine'}, {'prefix': '/app0/../data'}, {'argv': []},
                {'headless': 'yes'}, {'gpu': 'yes'}, {'gpu': 1}, {'gpu': True},
                {'gpu': True, 'headless': False}, {'payload_programs': 'Agent.exe'},
                {'payload_programs': ['Agent.exe']}, {'payload_programs': [r'C:\Agent.exe;C:\Other.exe']},
                {'payload_programs': ['C:\\Agent.exe\n']}, {'payload_programs': [None]},
                {'refused_programs': 'BlizzardError.exe'},
                {'refused_programs': [r'C:\BlizzardError.exe']},
                {'refused_programs': ['Utils/BlizzardError.exe']},
                {'refused_programs': ['BlizzardError.exe;Other.exe']},
                {'refused_programs': ['BlizzardError.exe', 'blizzarderror.EXE']},
                {'refused_programs': ['BlizzárdError.exe']}, {'refused_programs': [None]},
                {'gpu': True, 'headless': True, 'child_dll_overrides': ''},
                {'gpu': True, 'headless': True, 'child_dll_overrides': 'dxgi=b\nsecret'},
                {'child_dll_overrides': 'dxgi=b'}):
    try:
        profile('b1', invalid)
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 profile accepted unsafe input: {invalid}')

with tempfile.TemporaryDirectory(prefix='pw-b1-profile-') as temporary:
    root = Path(temporary) / 'tree'
    for relative in (WRITEBROKER_RELATIVE, ADDON_WORKER_RELATIVE,
                     'runescape/runescape-bootstrap.exe', 'runescape/DigiCertTrustedRootG4.der',
                     'lib/wine/i386-windows/ntdll.dll',
                     'lib/wine/x86_64-windows/ntdll.dll', 'share/wine/nls/locale.nls',
                     'share/wine/wine.inf', 'lib/wine/x86_64-unix/wine',
                     'lib/wine/x86_64-unix/ntdll.so', 'lib/wine/x86_64-unix/win32u.so',
                     'bin/wineserver.so', 'lib/wine/x86_64-unix/ws2_32.so',
                     'lib/wine/x86_64-unix/crypt32.so', 'lib/wine/x86_64-unix/opengl32.so',
                     'lib/wine/x86_64-unix/secur32.so', 'lib/wine/x86_64-unix/bcrypt.so',
                     'lib/wine/x86_64-unix/wineps5.so', 'lib/libgnutls.so',
                     'lib/wine/x86_64-unix/dwrite.so', 'lib/wine/x86_64-unix/nsi.so', 'lib/libfreetype.so'):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(
            b'\x7fELFhost-test'
            if relative in (WRITEBROKER_RELATIVE, ADDON_WORKER_RELATIVE)
            else relative.encode())
    manifest = runtime_tree_manifest(root, b1)
    assert manifest['remote_root'] == '/data/homebrew/PPSA99225/wine-title'
    assert manifest['pe_dll_counts'] == {'i386': 1, 'x86_64': 1}
    assert manifest['files']['share/wine/nls/locale.nls']['sha256'] == hashlib.sha256(
        b'share/wine/nls/locale.nls').hexdigest()
    assert manifest['files'][WRITEBROKER_RELATIVE] == file_identity(root / WRITEBROKER_RELATIVE)
    assert (root / WRITEBROKER_RELATIVE).stat().st_size <= WRITEBROKER_MAX_BYTES
    assert manifest['files'][ADDON_WORKER_RELATIVE] == file_identity(root / ADDON_WORKER_RELATIVE)
    assert (root / ADDON_WORKER_RELATIVE).stat().st_size <= ADDON_WORKER_MAX_BYTES
    modules = module_manifest({name: root / relative.removeprefix('/app0/wine-title/')
                              for name, relative in b1['module_paths'].items()}, profile_name='b1')
    assert all(item['path'].startswith('/app0/wine-title/') for item in modules)
    assert '/app0/wine-title/bin/wineserver.so' in render_include([], modules, profile_name='b1')
    folder = Path(temporary) / 'published'
    for relative in ('eboot.bin', 'sce_sys/param.json', 'sce_module/libc.prx',
                     'assets/icon0.png', 'psmkit-wine-runtime-child.bin',
                     'runtime-qualification.json'):
        path = folder / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'published')
    guards = b1_guard_paths(folder, '/data/homebrew/PPSA99225')
    assert '/data/homebrew/PPSA99225/wine/boot.txt' in guards
    assert '/data/homebrew/PPSA99225/assets/icon0.png' in guards
    assert '/data/homebrew/PPSA99225/.upload-eboot.bin' in guards
    assert '/data/homebrew/PPSA99225/sce_sys/.upload-param.json' in guards
    assert not any('/wine/lib/wine/' in path or '/wine-title/' in path for path in guards)
    (root / 'title-root-manifest.json').write_text(json.dumps(manifest, sort_keys=True))
    shutil.copytree(root, folder / 'wine-title')
    def inventory(directory):
        return [{'path': path.relative_to(directory).as_posix(),
                 'size': path.stat().st_size, 'sha256': file_identity(path)['sha256']}
                for path in sorted(directory.rglob('*')) if path.is_file()]
    broker_identity = {
        'path': WRITEBROKER_RELATIVE,
        'app_path': b1['root'] + '/' + WRITEBROKER_RELATIVE,
        'classification': 'data_only_not_loaded',
        'size': (root / WRITEBROKER_RELATIVE).stat().st_size,
        'sha256': file_identity(root / WRITEBROKER_RELATIVE)['sha256'],
    }
    addon_worker_identity = {
        'path': ADDON_WORKER_RELATIVE,
        'app_path': b1['root'] + '/' + ADDON_WORKER_RELATIVE,
        'classification': 'data_only_not_loaded',
        'size': (root / ADDON_WORKER_RELATIVE).stat().st_size,
        'sha256': file_identity(root / ADDON_WORKER_RELATIVE)['sha256'],
    }
    material = {
        'profile': b1, 'modules': modules, 'writebroker': broker_identity,
        'addon_worker': addon_worker_identity,
        'title_root': inventory(folder / 'wine-title'),
        'parent_title': [item for item in inventory(folder)
                         if not item['path'].startswith('wine-title/') and item['path'] not in
                         ('runtime-qualification.json', 'psmkit-wine-runtime-child.bin')],
        'native_parent': {'fself_size': 9, 'fself_sha256': hashlib.sha256(b'published').hexdigest()},
        'native_child': {'fself_size': 9, 'fself_sha256': hashlib.sha256(b'published').hexdigest()},
    }
    receipt = {
        'status': 'QUALIFIED_STATIC_NOT_EXECUTED', 'material': material,
        'qualification_key': hashlib.sha256(json.dumps(
            material, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
    }
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    assert qualified_b1_folder(folder) == (receipt, manifest)
    # The same channel-neutral role must survive both its own qualification
    # and the combined maintenance/game receipt reconstruction.
    added_modules = []
    for name, installed in game['module_paths'].items():
        destination = folder / installed.removeprefix('/app0/')
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(name.encode())
            added_modules.append(destination)
    game_manifest = runtime_tree_manifest(folder / 'wine-title', game)
    (folder / 'wine-title/title-root-manifest.json').write_text(json.dumps(game_manifest))
    original_material = dict(material)
    for dual in (False, True):
        material['profile'] = maintenance if dual else game
        if dual:
            material['game_profile'] = game
        material['title_root'] = inventory(folder / 'wine-title')
        receipt['qualification_key'] = hashlib.sha256(json.dumps(
            material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
        assert qualified_b1_folder(folder) == (receipt, game_manifest)
    for destination in added_modules:
        destination.unlink()
    material.clear()
    material.update(original_material)
    (folder / 'wine-title/title-root-manifest.json').write_text(json.dumps(manifest, sort_keys=True))
    for selected_profile, accepted in ((profile('b1', {'gpu': True, 'headless': True}), False),
                                       ({key: value for key, value in b1.items() if key != 'gpu'}, True)):
        material['profile'] = selected_profile
        receipt['qualification_key'] = hashlib.sha256(json.dumps(
            material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
        try:
            assert qualified_b1_folder(folder) == (receipt, manifest)
        except ValueError:
            assert not accepted, 'a receipt without gpu was refused'
        else:
            assert accepted, 'a gpu receipt passed without winevulkan.so in the tree'
    material['profile'] = b1
    receipt['qualification_key'] = hashlib.sha256(json.dumps(
        material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    (folder / 'sce_module/libc.prx').write_bytes(b'corrupted')
    try:
        qualified_b1_folder(folder)
    except ValueError:
        pass
    else:
        raise AssertionError('qualification accepted corrupted title bytes')
    (folder / 'sce_module/libc.prx').write_bytes(b'published')
    receipt['qualification_key'] = '00' * 32
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    try:
        qualified_b1_folder(folder)
    except ValueError:
        pass
    else:
        raise AssertionError('qualification accepted a forged material digest')
    try:
        (folder / 'wine').mkdir()
        (folder / 'wine/bad.dll').write_bytes(b'bad')
        b1_guard_paths(folder, '/data/homebrew/PPSA99225')
    except ValueError:
        pass
    else:
        raise AssertionError('B1 publication accepted a payload write')
    extra = root / 'lib/wine/x86_64-unix/kerberos.so'
    extra.write_bytes(b'not admitted')
    try:
        runtime_tree_manifest(root, b1)
    except ValueError:
        pass
    else:
        raise AssertionError('B1 tree accepted an additional Unix module')

print('RUNTIME_PROFILE_HOST_PASS root=SEPARATE_TITLE_ONLY guards=NO_PAYLOAD_WRITES modules=EXACT_PROFILE console=NOT_CONTACTED')
