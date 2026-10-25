#!/usr/bin/env python3
"""Compile the real IME, title text framing, Wine packet input and translation."""
import argparse
import importlib.util
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='cc')
parser.add_argument('--diagnostics', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
args.output.parent.mkdir(parents=True, exist_ok=True)
(args.output.parent / 'ime-title-send.inc').write_text(
    helper.definition(here / '../../title/main.c', 'send_text') + '\n')
(args.output.parent / 'ime-translate.inc').write_text(
    helper.definition(args.wine / 'dlls/win32u/message.c', 'NtUserTranslateMessage') + '\n')
(args.output.parent / 'ime-queue.inc').write_text(
    helper.definition(args.wine / 'server/queue.c', 'rawinput_message', structure=True) + '\n' +
    helper.definition(args.wine / 'server/queue.c', 'queue_keyboard_message') + '\n')
(args.output.parent / 'ime-process.inc').write_text(
    helper.definition(args.wine / 'dlls/win32u/message.c', 'process_keyboard_message') + '\n')
defines = ['-DTEST_TEXT_DIAG=1', '-DWINEPS5_TEXT_DIAG=1'] if args.diagnostics else []
subprocess.run(shlex.split(args.cc) + [
    '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-I' + str(args.output.parent), '-I' + str(args.wine / 'include'),
    *defines, str(here / 'test-title-ime-delivery.c'), '-o', str(args.output)], check=True)
