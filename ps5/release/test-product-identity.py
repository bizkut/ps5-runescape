#!/usr/bin/env python3
"""Check title metadata, generated branding and broker isolation without an SDK."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

product = Path(__file__).resolve().parents[2]
ps5 = product / 'ps5'
wine = Path(os.environ.get('WINE_SRC', product.parent / 'ps5-wine'))
param = json.loads((ps5 / 'title/sce_sys/param.json').read_text())
assert param['titleId'] == 'PPSA99225'
assert param['conceptId'] == '99225'
assert param['contentId'].startswith('UP9000-PPSA99225_00-')
name = param['localizedParameters'][param['localizedParameters']['defaultLanguage']]['titleName']
assert f'#define PRODUCT_DISPLAY_NAME {json.dumps(name)}' in (ps5 / 'include/product-name.h').read_text()
paths = subprocess.check_output(['bash', '-c', 'source "$1"; printf "%s\\n%s\\n" "$TITLE_ID" "$WINE_ROOT"',
                                 'identity', str(ps5 / 'paths.sh')], text=True).splitlines()
assert paths == ['PPSA99225', '/data/homebrew/PPSA99225/wine']
for profile in ('b1-product.json', 'b1-game-osrs.json'):
    config = json.loads((ps5 / 'probes/psmkit-wine' / profile).read_text())
    assert config['prefix'] == '/data/runescape/prefix'
    assert config['home'] == '/data/runescape/home'

# Exercise the actual shared header twice: unchanged Forever defaults and the
# product's build-time defines, including positive and near-root negative paths.
source = r'''
#include <assert.h>
#include <string.h>
#include "wine/ps5_writebroker.h"
int main(void) {
    assert(!strcmp(WB_USER_ROOT, EXPECT_ROOT));
    assert(!strcmp(WB_TITLE_ID, EXPECT_TITLE));
    assert(WB_PORT == EXPECT_PORT);
    assert(wb_path_root(EXPECT_ROOT "prefix/file"));
    assert(!wb_path_root(EXPECT_ROOT));
    assert(!wb_path_root(EXPECT_ROOT "../other/file"));
    assert(!wb_path_root(EXPECT_ROOT "/file"));
    assert(!wb_path_root(OTHER_ROOT "prefix/file"));
    assert(!strcmp(WB_TOKEN_FILE, EXPECT_ROOT ".writebroker-token"));
    assert(!strcmp(WB_ICON_FILE, EXPECT_ROOT "icon0.png"));
    return 0;
}
'''
cc = shlex.split(os.environ.get('CC', 'cc'))
with tempfile.TemporaryDirectory(prefix='runescape-identity-') as directory:
    directory = Path(directory)
    c = directory / 'identity.c'
    c.write_text(source)
    for title, root, other, port, includes in (
        ('PPSA99030', '/data/forever/', '/data/runescape/', 9141, []),
        ('PPSA99225', '/data/runescape/', '/data/forever/', 9142,
         ['-include', str(ps5 / 'include/product-wine.h')]),
    ):
        binary = directory / title
        subprocess.run(cc + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(wine / 'include'),
                              *includes, f'-DEXPECT_ROOT="{root}"', f'-DEXPECT_TITLE="{title}"',
                              f'-DOTHER_ROOT="{other}"', f'-DEXPECT_PORT={port}',
                              str(c), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
native = (ps5 / 'probes/psmkit-wine/native-entry.c').read_text()
autostart = (ps5 / 'probes/psmkit-wine/writebroker-autostart.h').read_text()
assert 'PW_WRITEBROKER_PORT' not in native + autostart
assert 'PW_RUNTIME_GPU ? "game" : "maintenance"' in native
assert 'role=%s port=%u root=%s status=%s connected=%d' in native
assert 'pw_writebroker_connect_port(WB_PORT,' in native
assert 'htons(WB_PORT)' in (ps5 / 'writebroker/writebroker.c').read_text()
assert 'htons( WB_PORT )' in (wine / 'dlls/ntdll/unix/file.c').read_text()
print('PASS product identity, canonical branding, profiles, and both broker root configurations')
