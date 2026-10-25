#!/usr/bin/env python3
"""Exercise real Wine TCP enumeration with a missing NSI service and real sockets.

Only the device/Unixcall and wineserver object/transport boundaries are adapted.
The server's owner/address enumeration, NSI conversion/dispatch and public table
allocation are extracted verbatim. A forked client transfers its socket using
SCM_RIGHTS, like the title's cross-process descriptor transport.
"""
import argparse
import importlib.util
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('extract', here / 'build-desktop-surface-test.py')
    extract = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extract)
    definition = extract.definition
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sock = args.wine / 'server/sock.c'
    server = definition(sock, 'enum_tcp_connection_info', structure=True) + '\n'
    server += definition(sock, 'enum_tcp_connections') + '\n'
    (args.output.parent / 'nsi-tcp-server.inc').write_text(server)
    nsi = args.wine / 'dlls/nsi/nsi.c'
    unix = args.wine / 'dlls/nsi/unixlib.c'
    shared = args.wine / 'include/wine/nsi_tcp.h'
    if shared.exists():
        tcp = shared.read_text()
        tcp += '\n' + definition(unix, 'unix_enumerate_tcp')
    else:
        tcp = definition(args.wine / 'dlls/nsiproxy.sys/tcp.c', 'tcp_conns_enumerate_all')
        tcp += '\nstatic NTSTATUS unix_enumerate_tcp(void *args) { (void)args; return STATUS_NOT_SUPPORTED; }\n'
    (args.output.parent / 'nsi-tcp-unix.inc').write_text(tcp)
    (args.output.parent / 'nsi-tcp-pe.inc').write_text('\n\n'.join(
        definition(nsi, name) for name in ('NsiEnumerateObjectsAllParametersEx',
                                          'NsiEnumerateObjectsAllParameters',
                                          'NsiAllocateAndGetTable')))
    iphlp = args.wine / 'dlls/iphlpapi/iphlpapi_main.c'
    (args.output.parent / 'nsi-tcp-api.inc').write_text('#define TCP_TABLE2 ~0u' + chr(10) + (chr(10) * 2).join(
        definition(iphlp, name) for name in ('ip_module_id', 'DWORD_cmp', 'tcp_table_id', 'tcp_table_size', 'tcp_row_fill',
            'tcp_row_cmp', 'tcp6_row_basic_cmp', 'tcp6_row_owner_cmp', 'tcp_table_needs_pids',
            'get_extended_tcp_table', 'GetTcpTable2', 'GetExtendedTcpTable')))
    subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unused-function', '-D__PROSPERO__', '-D__WINESRC__',
        '-I' + str(args.output.parent), '-I' + str(args.wine / 'include'),
        str(here / 'test-nsi-tcp.c'), '-o', str(args.output)], check=True)
    print('NSI_TCP_ISOLATION server_owner_and_endpoints=PRODUCTION nsi_dispatch_and_allocation=PRODUCTION '
          'sockets=REAL fork_and_SCM_RIGHTS=REAL missing_service=DEVICE_BOUNDARY')


if __name__ == '__main__':
    main()
