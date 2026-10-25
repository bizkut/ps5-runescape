#!/usr/bin/env python3
"""The PS5 title sandbox rejects lstat() with EPERM (console-observed: the
R1 pad-mapping install failed with "stat failed: Operation not permitted").
Title and runtime sources must use stat() plus O_NOFOLLOW open/fstat instead."""
import re
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
roots = [here.parent.parent / 'title', here]
call = re.compile(r'\blstat\s*\(')
found = []
for root in roots:
    for path in sorted(root.glob('*.[ch]')):
        if path.name.startswith('test-'):
            continue
        for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            code = line.split('//', 1)[0]
            if call.search(code) and not code.lstrip().startswith(('*', '/*')):
                found.append(f'{path.relative_to(here.parent.parent.parent)}:{number}: {line.strip()}')
if found:
    print('lstat() is rejected in the PS5 title sandbox; use stat()/O_NOFOLLOW:', *found, sep='\n  ')
    sys.exit(1)
print('PASS title sources avoid lstat()')
