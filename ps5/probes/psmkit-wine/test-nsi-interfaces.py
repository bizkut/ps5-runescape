#!/usr/bin/env python3
"""Real NSI/IPHelper interface APIs with a missing device and real host interfaces."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
args.out.mkdir(parents=True, exist_ok=True)
spec = spec_from_file_location('nsi_interface_extraction', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
# Reuse the existing real wineserver TCP transport fixture. This builds its
# reference binary but never substitutes a service-free interface implementation.
subprocess.run([sys.executable, str(here / 'build-nsi-tcp-test.py'), '--wine', str(args.wine),
                '--output', str(args.out / 'test-nsi-tcp'), '--cc', args.cc], check=True)
nsi = args.wine / 'dlls/nsi/nsi.c'
unix = args.wine / 'dlls/nsi/unixlib.c'
iphlp = args.wine / 'dlls/iphlpapi/iphlpapi_main.c'
proxy = args.wine / 'dlls/nsiproxy.sys'
body = unix.read_text()
body = body[body.index('static NTSTATUS unix_enumerate_tcp'):]
(args.out / 'nsi-if-unix.inc').write_text(
    'typedef NTSTATUS (*unixlib_entry_t)(void *);\n'
    'extern NTSTATUS nsi_ndis_refresh(void);\n' +
    'extern NTSTATUS nsi_enumerate_all_ex(struct nsi_enumerate_all_ex *);\n'
    'extern NTSTATUS nsi_get_all_parameters_ex(struct nsi_get_all_parameters_ex *);\n'
    'extern NTSTATUS nsi_get_parameter_ex(struct nsi_get_parameter_ex *);\n' +
    (args.wine / 'include/wine/nsi_tcp.h').read_text() + '\n' + body)
(args.out / 'nsi-if-pe.inc').write_text('\n\n'.join(extract.definition(nsi, name) for name in (
    'NsiGetAllParameters', 'NsiGetAllParametersEx', 'NsiGetParameter', 'NsiGetParameterEx')))
api_names = (
    'ConvertGuidToStringA', 'ConvertGuidToStringW', 'if_counted_string_copy',
    'ConvertInterfaceIndexToLuid', 'ConvertInterfaceLuidToGuid', 'ConvertInterfaceLuidToIndex',
    'ConvertLengthToIpv4Mask', 'get_wins_servers', 'ip_addr_string_init', 'GetAdaptersInfo',
    'address_entry_free', 'address_entry_size', 'address_entry_copy', 'address_lists_iterate',
    'adapters_addresses_free', 'adapters_addresses_size', 'adapters_addresses_cmp',
    'adapters_addresses_sort', 'adapters_addresses_copy', 'sockaddr_is_loopback',
    'sockaddr_is_linklocal', 'unicast_is_dns_eligible', 'unicast_addresses_alloc',
    'gateway_and_prefix_addresses_alloc', 'call_families', 'dns_servers_query_code',
    'dns_info_alloc', 'adapters_addresses_alloc', 'GetAdaptersAddresses',
    'if_row_fill', 'ifrow_cmp', 'GetIfTable', 'if_row2_fill', 'GetIfTable2Ex', 'GetIfTable2',
    'ipaddrrow_cmp', 'GetIpAddrTable')
api = extract.definition(iphlp, 'address_entry_copy_params', structure=True) + '\n'
api += '\n\n'.join(extract.definition(iphlp, name) for name in api_names)
(args.out / 'nsi-if-api.inc').write_text(api)
# The fixture loads the actual NDIS/IP modules. Registry membership is transport
# metadata; all table validation and provider implementations remain production.
dispatch = proxy / 'tables.c'
registry = '''#include "nsi-interface-provider.h"\n#include "unix_private.h"\nstatic const struct module *modules[] = {
    &ndis_module, &ipv4_module, &ipv6_module
};\n'''
registry += '\n\n'.join(extract.definition(dispatch, name) for name in (
    'get_module_table', 'nsi_enumerate_all_ex', 'nsi_get_all_parameters_ex', 'nsi_get_parameter_ex'))
(args.out / 'nsi-if-dispatch.c').write_text(registry)
(args.out / 'config.h').write_text('''#define HAVE_NET_IF_H 1
#define HAVE_NET_IF_ARP_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_IF_ETHER_H 1
#define HAVE_NET_ROUTE_H 1
#define HAVE_IFADDRS_H 1
#define HAVE_GETIFADDRS 1
#define HAVE_ARPA_INET_H 1
#define HAVE_STRUCT_IFREQ_IFR_HWADDR 1
#define HAVE___BUILTIN_POPCOUNT 1
''')
prefix = r'''#ifndef NSI_INTERFACE_PROVIDER_H
#define NSI_INTERFACE_PROVIDER_H
#define _GNU_SOURCE 1
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#define USE_WS_PREFIX
#include "winsock2.h"
#include "ws2ipdef.h"
#include "nldef.h"
#include "ifdef.h"
#include "netiodef.h"
#include "wine/nsi.h"
#include "wine/unixlib.h"
#include "wine/debug.h"
#undef WINE_DEFAULT_DEBUG_CHANNEL
#undef TRACE
#undef WARN
#undef ERR
#undef FIXME
#define WINE_DEFAULT_DEBUG_CHANNEL(x)
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define FIXME(...) ((void)0)
#endif
'''
(args.out / 'nsi-interface-provider.h').write_text(prefix)
scope_source = proxy / 'scope.c'
common = shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unused-function',
    '-Wno-missing-field-initializers', '-fshort-wchar',
    '-D_GNU_SOURCE=1', '-D__PROSPERO__=1', '-D__WINESRC__=1', '-D_WIN64=1', '-DWINE_UNIX_LIB=1', '-DNSI_SERVICE_FREE=1',
    '-I' + str(args.out), '-I' + str(args.wine / 'include'), '-I' + str(proxy)]
objects = []
for path in (proxy / 'ndis.c', proxy / 'ip.c', scope_source, args.out / 'nsi-if-dispatch.c'):
    output = args.out / ('nsi-if-' + path.stem + '.o')
    subprocess.run(common + ['-include', str(args.out / 'nsi-interface-provider.h'), '-c', str(path), '-o', str(output)], check=True)
    objects.append(output)
binary = args.out / 'test-nsi-interfaces'
subprocess.run(common + ['-D__WINE_PS5__=1', '-I' + str(here), str(here / 'test-nsi-interfaces.c'),
    *map(str, objects), '-pthread', '-ldl', '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print('NSI_INTERFACE_HOST_PASS scope=REAL_HOST_INTERFACES_AND_PRODUCTION_API_CHAIN_NOT_PS5_QUALIFICATION')
