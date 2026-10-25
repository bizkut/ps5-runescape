#!/usr/bin/env python3
"""Compile real maintenance input/output adapters with bounded device/UI stubs."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = spec_from_file_location('desktop_extract', here / 'build-desktop-surface-test.py')
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    source = here / '../../title/main.c'
    (args.output.parent / 'maintenance-padmap.inc').write_text('\n\n'.join(
        module.definition(source, name) for name in ('mouse_button', 'on_pointer', 'on_input')) + '\n')
    subprocess.run(shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-I' + str(args.output.parent), '-I' + str(args.wine / 'include'),
        str(here / 'test-maintenance-padmap.c'), str(here / '../../title/padmap.c'),
        str(here / '../../title/pointer.c'), str(here / '../../title/forever-menu.c'),
        '-o', str(args.output)], check=True)


if __name__ == '__main__':
    main()
