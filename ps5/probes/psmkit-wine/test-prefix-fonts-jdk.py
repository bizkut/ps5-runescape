#!/usr/bin/env python3
"""Run a real Windows JDK under real Wine against the app-owned prefix fonts.

Explicit integration test, not part of the host-only make target: the caller
supplies a host Wine and an extracted Windows x64 JRE (RuneLite bundles
Temurin 17). The disposable prefix has no Z: drive and no fontconfig faces,
like the console title, and receives the fork's wine.inf font values. RED
must be the JDK's own "No physical fonts found"; GREEN runs the production
managed sync and offline registration, then requires Wine to read the values
and the JDK to resolve Dialog/Arial to the staged Liberation Sans face.
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--jre', type=Path, required=True, help='extracted Windows x64 JRE directory')
parser.add_argument('--wine-src', type=Path, required=True)
parser.add_argument('--wine', default='wine')
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('runtime_profiles', here / 'runtime_profiles.py')
profiles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiles)

PROBE = r'''
import java.awt.Font;
import java.awt.image.BufferedImage;
public class FontProbe {
    public static void main(String[] args) throws Exception {
        java.awt.Graphics2D g = new BufferedImage(8, 8, BufferedImage.TYPE_INT_ARGB).createGraphics();
        for (String name : new String[] {"Dialog", "Arial"}) {
            Font font = new Font(name, Font.PLAIN, 12);
            Object f2d = Class.forName("sun.font.FontUtilities").getMethod("getFont2D", Font.class).invoke(null, font);
            if (f2d.getClass().getName().equals("sun.font.CompositeFont"))
                f2d = f2d.getClass().getMethod("getSlotFont", int.class).invoke(f2d, 0);
            System.out.println("PROBE " + name + "=" + f2d.getClass().getMethod("getFamilyName", java.util.Locale.class)
                .invoke(f2d, java.util.Locale.ENGLISH) + " width=" + g.getFontMetrics(font).stringWidth("RuneLite"));
        }
        Object fm = Class.forName("sun.font.FontManagerFactory").getMethod("getInstance").invoke(null);
        Object def = fm.getClass().getMethod("getDefaultPhysicalFont").invoke(fm);
        System.out.println("PROBE default=" + def.getClass().getMethod("getFamilyName", java.util.Locale.class)
            .invoke(def, java.util.Locale.ENGLISH));
    }
}
'''

with tempfile.TemporaryDirectory(prefix='prefix-fonts-jdk-') as temporary:
    root = Path(temporary)
    prefix, tree = root / 'prefix', root / 'runtime'
    fontconfig = root / 'fonts.conf'
    fontconfig.write_text('<?xml version="1.0"?>\n<!DOCTYPE fontconfig SYSTEM "fonts.dtd">\n'
                          f'<fontconfig><cachedir>{root}/fc</cachedir></fontconfig>\n')
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all', DISPLAY='',
               FONTCONFIG_FILE=str(fontconfig), WINEDLLOVERRIDES='mscoree,mshtml=')

    def wine(*command, check=True):
        result = subprocess.run([args.wine, *command], env=env, cwd=prefix / 'drive_c',
                                capture_output=True, text=True, timeout=300)
        if check and result.returncode:
            raise SystemExit(f'wine {command} failed: {result.stdout}{result.stderr}')
        return result.stdout + result.stderr

    def stop():
        subprocess.run(['wineserver', '-w'], env=env, check=True, timeout=300)

    subprocess.run([args.wine, 'wineboot', '-u'], env=env, capture_output=True, check=True, timeout=300)
    stop()
    (prefix / 'dosdevices/z:').unlink()  # the console title has no Z: drive
    shutil.copytree(args.jre, prefix / 'drive_c/jre')
    (root / 'FontProbe.java').write_text(PROBE)
    subprocess.run(['javac', '--release', '17', '-d', str(prefix / 'drive_c'), str(root / 'FontProbe.java')], check=True)

    harness = root / 'harness.c'
    harness.write_text('''#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
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
    binary = root / 'harness'
    subprocess.run(shlex.split(args.cc) + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(here),
                                          '-I' + str(args.wine_src / 'include'), str(harness), '-o', str(binary)],
                   check=True)

    def run(*command):
        return int(subprocess.check_output([str(binary), *command], text=True))

    # The fork's wineboot writes these [Fonts] values (upstream 9.x lacks them).
    seeded = []
    for line in (args.wine_src / 'loader/wine.inf.in').read_text().splitlines():
        match = re.fullmatch(r'HKLM,%(FontsNT|FontSubStr)%,"([^"]+)",,"([^"]+)"', line)
        if match:
            key = profiles.FONTS_KEY if match[1] == 'FontsNT' else profiles.FONT_SUBSTITUTES_KEY
            seeded.append(f'{key}\t{match[2]}\t{match[3]}\n')
    (root / 'wine-inf-fonts.txt').write_text(''.join(seeded))
    assert run('register', str(prefix), str(root / 'wine-inf-fonts.txt')) == 1

    java = ['C:\\jre\\bin\\java.exe', '-Djava.awt.headless=true',
            '--add-exports', 'java.desktop/sun.font=ALL-UNNAMED', '-cp', 'C:\\', 'FontProbe']
    red = wine(*java, check=False)
    stop()
    assert 'java.lang.Error: Probable fatal error: No physical fonts found.' in red, red
    print('PREFIX_FONTS_JDK_RED error="Probable fatal error: No physical fonts found."')

    profiles.stage_prefix_fonts(here.parents[1] / 'build', tree / 'share/forever/fonts')
    (tree / 'share/forever/font-registry.txt').write_text(profiles.prefix_font_registry(tree))
    manifest = tree / 'prefix-files.txt'
    manifest.write_text(''.join(f'{source}\t{destination}\t{hashlib.sha256((tree / source).read_bytes()).hexdigest()}\n'
                                for source, destination in profiles.prefix_font_files()))
    assert run('sync', str(tree), str(manifest), str(prefix)) == 12
    assert run('register', str(prefix), str(tree / 'share/forever/font-registry.txt')) == 1
    for key, name, value in (line.split('\t') for line in profiles.prefix_font_registry(tree).splitlines()):
        output = wine('reg', 'query', 'HKLM\\' + key, '/v', name)
        assert re.search(re.escape(name) + r'\s+REG_SZ\s+' + re.escape(value), output), output
    stop()
    green = wine(*java)
    stop()
    lines = dict(line.split(' ', 1)[1].split('=', 1) for line in green.splitlines() if line.startswith('PROBE '))
    assert lines['default'] == 'Liberation Sans', green
    assert lines['Dialog'].startswith('Liberation Sans width=') and lines['Arial'].startswith('Liberation Sans width='), green
    assert run('register', str(prefix), str(tree / 'share/forever/font-registry.txt')) == 0, \
        'Wine-saved values must be recognized as current'
    print(f'PREFIX_FONTS_JDK_GREEN dialog=Liberation_Sans arial=Liberation_Sans default=Liberation_Sans '
          f'wine_reg_query=all_values jre={args.jre.name} not_console_qualification')
