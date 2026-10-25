#!/usr/bin/env python3
"""Real app-owned prefix fonts: staging, managed sync, offline hive registration
and the JDK Win32 physical-font discovery checks they must satisfy.

The JDK model follows OpenJDK 17 exactly (src/java.desktop/...):
windows/native/libfontmanager/fontpath.c getFontPath/populateFontFileNameMap0,
windows/classes/sun/awt/Win32FontManager.java populateHardcodedFileNameMap and
share/classes/sun/font/SunFontManager.java getDefaultPhysicalFont/findFont2D/
loadFonts/registerOtherFontFiles/loadFontFiles. The console baseline (no files
in C:\\windows\\Fonts, Wine's faces registered by \\??\\unix paths, wine.inf's
bare Microsoft names) must reproduce RuneLite's "No physical fonts found".
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('runtime_profiles', here / 'runtime_profiles.py')
profiles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiles)
args.out.mkdir(parents=True, exist_ok=True)
release = here.parents[1] / 'release'

HEADER = 'WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n'
FONTS = profiles.FONTS_KEY.replace('\\', '\\\\')
SUBSTITUTES = profiles.FONT_SUBSTITUTES_KEY.replace('\\', '\\\\')


def hive_values(text, key):
    """Values of one key from Wine's saved hive syntax (REG_SZ only)."""
    values, inside = {}, False
    for line in text.splitlines():
        if line.startswith('['):
            inside = line[1:].split(']', 1)[0].lower() == key.replace('\\', '\\\\').lower()
        elif inside:
            match = re.fullmatch(r'"((?:[^"\\]|\\.)*)"="((?:[^"\\]|\\.)*)"', line)
            if match:
                unescape = lambda value: re.sub(r'\\(.)', r'\1', value)
                values[unescape(match[1])] = unescape(match[2])
    return values


def wine_inf_fonts():
    """The fork's [Fonts] FontsNT and FontSubstitutes lines, as wineboot writes them."""
    fonts, substitutes = [], []
    for line in (args.wine / 'loader/wine.inf.in').read_text().splitlines():
        match = re.fullmatch(r'HKLM,%(FontsNT|FontSubStr)%,"([^"]+)",,"([^"]+)"', line)
        if match:
            (fonts if match[1] == 'FontsNT' else substitutes).append((match[2], match[3]))
    assert ('Arial (TrueType)', 'arial.ttf') in fonts and ('Times', 'Times New Roman') in substitutes
    return fonts, substitutes


def section(key, values, stamp='1792927377'):
    lines = ''.join(f'"{name}"="{value.replace(chr(92), chr(92) * 2)}"\n' for name, value in values)
    return f'\n[{key}] {stamp}\n#time=1dd64732fbe173a\n{lines}'


def jdk_physical_fonts(prefix, hive):
    """Return (default face file, registered physical files) or raise like the JDK."""
    fonts_dir = prefix / 'drive_c/windows/Fonts'
    # fontpath.c getFontPath: system32\..\Fonts and windows\Fonts are one directory.
    listing = {path.name.lower(): path for path in fonts_dir.iterdir()} if fonts_dir.is_dir() else {}

    def exists(name):  # Wine resolves DOS names case-insensitively.
        return listing.get(name.lower())

    def physical(path):
        try:
            profiles.sfnt_names(path.read_bytes())
            return True
        except (OSError, ValueError):
            return False

    registry = {}
    for name, data in hive_values(hive, profiles.FONTS_KEY).items():
        base = name[:-len(' (TrueType)')] if name.endswith((' (TrueType)', ' (OpenType)')) else None
        if base is None and not data.lower().endswith(('.ttf', '.otf')):
            continue
        registry[(base or name).lower()] = data
    registered = set()
    # getDefaultPhysicalFont -> findFont2D("Arial") -> findFontFromPlatformMap:
    # every hardcoded Arial file must exist in the single font directory.
    arial = [exists(name) for name in ('ARIAL.TTF', 'ARIALBD.TTF', 'ARIALI.TTF', 'ARIALBI.TTF')]
    if all(arial) and all(physical(path) for path in arial):
        registered.update(arial)
        default = arial[0]
    else:
        default = None
        # loadFonts -> registerOtherFontFiles: only Windows-absolute registry paths.
        for data in registry.values():
            if re.match(r'^[A-Za-z]:[\\/]|^\\\\', data):
                raise AssertionError(f'unexpected absolute font registration: {data}')
        # loadFontFiles: every TrueType/OpenType file in the font path.
        registered.update(path for name, path in listing.items()
                          if name.endswith(('.ttf', '.ttc', '.otf')) and physical(path))
        if not registered:
            raise RuntimeError('Probable fatal error: No physical fonts found.')
        default = sorted(registered)[0]
    return default, registered


with tempfile.TemporaryDirectory(prefix='prefix-fonts-') as temporary:
    root = Path(temporary)
    tree, prefix = root / 'runtime', root / 'prefix'
    profiles.stage_prefix_fonts(here.parents[1] / 'build', tree / 'share/forever/fonts')
    owned = profiles.prefix_font_files()
    assert len(owned) == 12 and len({destination.lower() for _, destination in owned}) == 12
    expected = {'arial': 'Liberation Sans', 'times': 'Liberation Serif', 'cour': 'Liberation Mono'}
    for source, destination in owned:
        assert destination == f'drive_c/windows/Fonts/{Path(source).name}', destination
        family, full = profiles.sfnt_names((tree / source).read_bytes())
        assert family == expected[re.match(r'arial|times|cour', Path(source).name)[0]], (source, family)
        assert full.startswith(family), (source, full)
    licence = (tree / 'share/forever/fonts' / profiles.PREFIX_FONT_LICENSE).read_bytes()
    assert b'SIL OPEN FONT LICENSE Version 1.1' in licence
    assert licence == (release / 'licenses/fonts' / profiles.PREFIX_FONT_LICENSE).read_bytes()
    registration = profiles.prefix_font_registry(tree)
    rows = [line.split('\t') for line in registration.splitlines()]
    font_rows = [row for row in rows if row[0] == profiles.FONTS_KEY]
    assert len(font_rows) == 12
    for key, name, value in font_rows:
        source = tree / 'share/forever/fonts' / value
        assert name == profiles.sfnt_names(source.read_bytes())[1] + ' (TrueType)', 'registry/sfnt name mismatch'
    assert [row[1:] for row in rows if row[0] == profiles.FONT_SUBSTITUTES_KEY] == [
        ['Arial', 'Liberation Sans'], ['Times New Roman', 'Liberation Serif'], ['Courier New', 'Liberation Mono']]
    assert 'Z:' not in registration and '/app0' not in registration
    (tree / 'share/forever/font-registry.txt').write_text(registration)
    manifest = tree / 'prefix-files.txt'
    manifest.write_text(''.join(f'{source}\t{destination}\t{hashlib.sha256((tree / source).read_bytes()).hexdigest()}\n'
                                for source, destination in owned))

    source = args.out / 'prefix-fonts-host.c'
    # PS5's title fstat on /data reports st_nlink=0 for ordinary files (run 3:
    # "font registry result=-22"); the PS5 shape reports a real count of one as
    # zero, keeping real hardlink counts (>1) visible.
    source.write_text('''#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
static int ps5_shaped_fstat(int fd, struct stat *status)
{
    int result = fstat(fd, status);
    if (!result && getenv("PS5_ZERO_LINKS") && status->st_nlink == 1) status->st_nlink = 0;
    return result;
}
#define fstat ps5_shaped_fstat
#include "forever-setup.h"
#include "forever-fonts.h"
int main(int argc, char **argv)
{
    struct forever_managed_report report;
    if (argc == 5 && argv[1][0] == 's') printf("%d\\n", forever_sync_managed(argv[2], argv[3], argv[4], &report));
    else if (argc == 4 && argv[1][0] == 'r') printf("%d\\n", forever_register_fonts(argv[2], argv[3]));
    else return 2;
    return 0;
}
''')
    binary = args.out / 'prefix-fonts-host'
    subprocess.run(shlex.split(args.cc) + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I' + str(here), '-I' + str(args.wine / 'include'), str(source), '-o', str(binary)], check=True)

    def sync():
        return int(subprocess.check_output([str(binary), 'sync', str(tree), str(manifest), str(prefix)], text=True))

    def register(registry=tree / 'share/forever/font-registry.txt', ps5=True):
        env = dict(os.environ, PS5_ZERO_LINKS='1') if ps5 else {k: v for k, v in os.environ.items()
                                                                if k != 'PS5_ZERO_LINKS'}
        return int(subprocess.check_output([str(binary), 'register', str(prefix), str(registry)],
                                           text=True, env=env))

    # Console baseline: wineboot's [Fonts] values, with Wine's data-dir faces
    # re-recorded through the title's drive-less \??\unix path (win32u
    # update_external_font_keys), and the user's own registration.
    prefix.mkdir()
    hive = prefix / 'system.reg'
    inf_fonts, inf_substitutes = wine_inf_fonts()
    baseline_fonts = dict(inf_fonts)
    baseline_fonts.update({f'{face} (TrueType)': f'\\??\\unix\\app0\\wine-title\\share\\wine\\fonts\\{name}'
                           for face, name in (('Tahoma', 'tahoma.ttf'), ('Marlett', 'marlett.ttf'),
                                              ('Symbol', 'symbol.ttf'))})
    baseline_fonts['Customer Font (TrueType)'] = 'customer.ttf'
    foreign = '\n[Software\\\\Customer] 1792927377\n"Keep"="user-data"\n"Blob"=hex:01,02,\\\n  03,04\n'
    baseline = (HEADER + section(FONTS, list(baseline_fonts.items())) +
                section(SUBSTITUTES, inf_substitutes) + foreign)
    hive.write_text(baseline)
    try:
        jdk_physical_fonts(prefix, baseline)
        raise AssertionError('console baseline unexpectedly has JDK physical fonts')
    except RuntimeError as error:
        assert str(error) == 'Probable fatal error: No physical fonts found.'
    print('PREFIX_FONTS_RED baseline=no_physical_fonts (matches RuneLite run 2)')

    # Managed sync installs exact bytes; unchanged files are not rewritten; damage is repaired.
    assert sync() == 12
    for source_name, destination in owned:
        assert (prefix / destination).read_bytes() == (tree / source_name).read_bytes()
    assert sync() == 0
    damaged = prefix / 'drive_c/windows/Fonts/arial.ttf'
    damaged.write_bytes(b'corrupted managed font')
    assert sync() == 1 and damaged.read_bytes() == (tree / 'share/forever/fonts/arial.ttf').read_bytes()

    # Offline registration keeps every foreign byte and value.
    result = register()
    assert result == 1, f'PS5-shaped registration result={result}'
    text = hive.read_text()
    assert text.startswith(HEADER) and foreign in text
    fonts = hive_values(text, profiles.FONTS_KEY)
    substitutes = hive_values(text, profiles.FONT_SUBSTITUTES_KEY)
    for key, name, value in rows:
        assert (fonts if key == profiles.FONTS_KEY else substitutes)[name] == value, (name, value)
    for name, value in baseline_fonts.items():
        assert fonts[name] == value, name
    for name, value in inf_substitutes:
        assert substitutes[name] == value, name
    assert text.count(f'[{FONTS}]') == 1 and text.count(f'[{SUBSTITUTES}]') == 1
    default, registered = jdk_physical_fonts(prefix, text)
    assert default.name == 'arial.ttf' and profiles.sfnt_names(default.read_bytes())[0] == 'Liberation Sans'
    assert {path.name for path in registered} == {'arial.ttf', 'arialbd.ttf', 'ariali.ttf', 'arialbi.ttf'}
    # With only the files (first start before registration), the JDK still finds them.
    assert jdk_physical_fonts(prefix, HEADER)[0].name == 'arial.ttf'
    before = hive.read_bytes()
    assert register() == 0 and hive.read_bytes() == before, 'current values must not rewrite the hive'
    assert register(ps5=False) == 0 and hive.read_bytes() == before, 'host link count of one is equally valid'

    # Wine re-saves keys in its own order/case; recognize persisted values.
    resaved = (HEADER + section(SUBSTITUTES.upper(), [tuple(row[1:]) for row in reversed(rows)
                                                       if row[0] == profiles.FONT_SUBSTITUTES_KEY]) +
               foreign + section(FONTS, [tuple(row[1:]) for row in rows if row[0] == profiles.FONTS_KEY]))
    hive.write_text(resaved)
    assert register() == 0 and hive.read_text() == resaved
    hive.write_text(resaved.replace('"Liberation Sans (TrueType)"="arial.ttf"',
                                    '"Liberation Sans (TrueType)"=hex(2):61,00,\\\n  62,00'))
    assert register() == 1
    text = hive.read_text()
    assert hive_values(text, profiles.FONTS_KEY)['Liberation Sans (TrueType)'] == 'arial.ttf'
    assert 'hex(2):61,00' not in text and '  62,00' not in text and foreign in text

    # Absent keys are created as Wine syntax; the hive stays loadable by the same reader.
    hive.write_text(HEADER + foreign.rstrip('\n'))
    assert register() == 1
    text = hive.read_text()
    assert foreign.rstrip('\n') + '\n' in text and re.search(r'\n\[' + re.escape(FONTS) + r'\] \d+\n#time=[0-9a-f]+\n', text)
    assert hive_values(text, profiles.FONT_SUBSTITUTES_KEY)['Arial'] == 'Liberation Sans'
    assert register() == 0

    # Refusals leave every byte untouched.
    for invalid in ('not a Wine hive\n', HEADER + '\0\n'):
        hive.write_text(invalid)
        assert register() < 0 and hive.read_text() == invalid
    hive.unlink()
    assert register() < 0 and not hive.exists(), 'never create a hive for an uninitialized prefix'
    outside = root / 'outside.reg'
    outside.write_text(HEADER)
    hive.symlink_to(outside)
    assert register() < 0 and outside.read_text() == HEADER, 'never follow a hive symlink'
    hive.unlink()
    hive.write_text(HEADER)
    os.link(hive, root / 'alias.reg')
    for ps5 in (True, False):
        assert register(ps5=ps5) < 0 and hive.read_text() == HEADER, 'a hardlinked hive is refused'
    (root / 'alias.reg').unlink()
    hive.unlink()
    hive.write_text(HEADER)
    bad = root / 'bad-registry.txt'
    for text in ('key\tname\n', 'key\tna"me\tvalue\n', 'key\tname\tC:\\value\n', ''):
        bad.write_text(text)
        assert register(bad) < 0 and hive.read_text() == HEADER, text

allowlist = (release / 'runtime-files.txt').read_text().splitlines()
for _, target in profiles.PREFIX_FONTS:
    assert f'wine-title/share/forever/fonts/{target}' in allowlist, target
for name in ('fonts/' + profiles.PREFIX_FONT_LICENSE, 'font-registry.txt'):
    assert f'wine-title/share/forever/{name}' in allowlist, name
native = (here / 'native-entry.c').read_text()
start = native.index('static int pw_forever_start(void)')
body = native[start:native.index('\n}\n', start)]
call = body.index('forever_register_fonts(FOREVER_PREFIX, FOREVER_APP_SHARE "/font-registry.txt")')
assert body.index('pw_forever_sync_managed()') < call and body.rfind('#if', 0, call) < body.rfind('#endif', 0, call), \
    'both title roles register after the managed sync, outside role-specific blocks'
assert '"' + profiles.PREFIX_FONT_ARCHIVE[1] + '"' in (release / 'deps.json').read_text()
print('PREFIX_FONTS_HOST_PASS fonts=LIBERATION_2.1.5_REAL_OUTLINE sync=idempotent_repair '
      'registry=sfnt_names+substitutes preserved_user_data ps5_zero_links=accepted hardlink=refused '
      'jdk_default=arial.ttf/Liberation_Sans '
      'both_roles_hook=1 not_console_qualification')
