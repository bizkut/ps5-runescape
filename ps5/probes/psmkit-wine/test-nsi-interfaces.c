/* Service-free NSI/IPHelper contracts. Interface/address/MAC inputs come from
 * the real host; only DLL/registry/DNS/Unixcall boundaries are isolated. */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/mman.h>
#define NSI_INTERFACE_TEST 1
#define main nsi_tcp_reference_main
#include "test-nsi-tcp.c"
#undef main
#include "windns.h"
#include "winreg.h"
#include "ip2string.h"
#include "winnls.h"
#include "netioapi.h"
#define FIXME(...) ((void)0)

static int interface_failures, fault_ifaddrs;
static unsigned interface_queries;
static void interface_check(int condition, const char *what)
{
    if (!condition) { fprintf(stderr, "NSI_INTERFACE_FAIL %s\n", what);interface_failures++; }
}
int getifaddrs(struct ifaddrs **out)
{
    static int (*real_getifaddrs)(struct ifaddrs **);
    interface_queries++;
    if (fault_ifaddrs) { errno = fault_ifaddrs;return -1; }
    if (!real_getifaddrs) real_getifaddrs = dlsym(RTLD_NEXT, "getifaddrs");
    assert(real_getifaddrs);return real_getifaddrs(out);
}
DWORD ntdll_umbstowcs(const char *src, DWORD count, WCHAR *dst, DWORD capacity)
{
    DWORD copied = count < capacity ? count : capacity;
    for (DWORD i = 0; i < copied; i++) dst[i] = (unsigned char)src[i];
    return copied;
}
NTSTATUS WINAPI NtQuerySystemInformation(SYSTEM_INFORMATION_CLASS kind, void *out, ULONG size, ULONG *returned)
{
    assert(kind == SystemTimeOfDayInformation && size == sizeof(SYSTEM_TIMEOFDAY_INFORMATION));
    SYSTEM_TIMEOFDAY_INFORMATION *info = out;
    memset(info, 0, size);info->BootTime.QuadPart = 10000000;
    if (returned) *returned = size;return STATUS_SUCCESS;
}
int sceNetGetMacAddress(BYTE *address, int flags)
{ (void)address;assert(!flags);errno = ENOSYS;return -1; }

#include "nsi-if-pe.inc"
static size_t host_wcslen(const WCHAR *text) { const WCHAR *p = text;while (*p) p++;return p - text; }
static WCHAR *host_wcscpy(WCHAR *dst, const WCHAR *src)
{ WCHAR *out = dst;while ((*out++ = *src++)) {}return dst; }
static INT host_wide_to_ascii(UINT page, DWORD flags, const WCHAR *src, INT count,
                              char *dst, INT capacity, const char *fallback, BOOL *used)
{
    (void)page;(void)flags;(void)fallback;
    INT needed = count < 0 ? host_wcslen(src) + 1 : count;
    if (used) *used = FALSE;
    if (!dst) return needed;
    if (capacity < needed) return 0;
    for (INT i = 0; i < needed; i++) { assert(src[i] < 128);dst[i] = src[i]; }
    return needed;
}
static INT host_ascii_to_wide(UINT page, DWORD flags, const char *src, INT count, WCHAR *dst, INT capacity)
{
    (void)page;(void)flags;
    INT needed = count < 0 ? strlen(src) + 1 : count;
    if (!dst) return needed;
    if (capacity < needed) return 0;
    for (INT i = 0; i < needed; i++) dst[i] = (unsigned char)src[i];
    return needed;
}
/* Wine PE long is 32 bits; host printf long is 64. Only GUID formatting crosses
 * this ABI shim, not table construction or identifier lookup. */
static int host_guid_sprintf(char *dst, const char *format, ...)
{
    va_list args;va_start(args, format);
    unsigned d1 = va_arg(args, unsigned), d2 = va_arg(args, int), d3 = va_arg(args, int), b[8];
    for (unsigned i = 0; i < 8; i++) b[i] = va_arg(args, int);
    va_end(args);
    assert(!strcmp(format, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}"));
    return sprintf(dst, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1,d2,d3,
                   b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
}
static int host_guid_swprintf(WCHAR *dst, size_t capacity, const WCHAR *format, ...)
{
    char text[64];(void)format;
    va_list args;va_start(args, format);
    unsigned d1 = va_arg(args, unsigned), d2 = va_arg(args, int), d3 = va_arg(args, int), b[8];
    for (unsigned i = 0; i < 8; i++) b[i] = va_arg(args, int);
    va_end(args);
    int length = sprintf(text, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1,d2,d3,
                         b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
    assert(capacity > (size_t)length);for (int i = 0; i <= length; i++) dst[i] = text[i];return length;
}
static DWORD no_wins_open(HKEY root, const char *name, HKEY *key)
{ (void)root;(void)name;(void)key;return ERROR_FILE_NOT_FOUND; }
static DWORD no_wins_query(HKEY key, const char *name, DWORD *reserved, DWORD *type, BYTE *data, DWORD *size)
{ (void)key;(void)name;(void)reserved;(void)type;(void)data;(void)size;return ERROR_FILE_NOT_FOUND; }
static DWORD no_wins_close(HKEY key) { (void)key;return ERROR_SUCCESS; }
static char *host_ipv4_string(const IN_ADDR *addr, char *out)
{ assert(inet_ntop(AF_INET, &addr->WS_s_addr, out, 16));return out + strlen(out); }
static NTSTATUS host_ipv4_parse(const char *text, BOOL strict, const char **end, IN_ADDR *addr)
{ (void)strict;(void)end;return inet_pton(AF_INET, text, &addr->WS_s_addr) == 1 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER; }
static DWORD no_search_suffix(DNS_CONFIG_TYPE query, DWORD flags, const WCHAR *name, void *reserved,
                              void *out, DWORD *size)
{
    (void)flags;(void)name;(void)reserved;(void)out;(void)size;
    assert(query == DnsConfigSearchList);return ERROR_FILE_NOT_FOUND;
}
#define wcscpy host_wcscpy
#define wcslen host_wcslen
#define swprintf host_guid_swprintf
#define sprintf host_guid_sprintf
#define WideCharToMultiByte host_wide_to_ascii
#define MultiByteToWideChar host_ascii_to_wide
#define RegOpenKeyA no_wins_open
#define RegQueryValueExA no_wins_query
#define RegCloseKey no_wins_close
#define RtlIpv4AddressToStringA host_ipv4_string
#define RtlIpv4StringToAddressA host_ipv4_parse
#define DnsQueryConfig no_search_suffix
#define s_addr WS_s_addr
#define CHARS_IN_GUID 39
static const WCHAR *device_tcpip = L"\\DEVICE\\TCPIP_";
#include "nsi-if-api.inc"
#undef s_addr
#undef sprintf

static void check_ndis_and_public_tables(void)
{
    MIB_IF_TABLE2 *table = NULL;
    DWORD err = GetIfTable2(&table);
    interface_check(!err && table && table->NumEntries, "GetIfTable2 returns real interfaces without Nsi device");
    if (err || !table) return;
    struct ifaddrs *host;
    assert(!getifaddrs(&host));
    unsigned loopbacks = 0;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        MIB_IF_ROW2 *row = &table->Table[i];
        interface_check(row->InterfaceIndex && row->InterfaceLuid.Info.NetLuidIndex == row->InterfaceIndex,
                        "native index and LUID agree");
        NET_LUID converted;NET_IFINDEX index = 0;GUID guid;
        interface_check(!ConvertInterfaceIndexToLuid(row->InterfaceIndex, &converted) &&
                        converted.Value == row->InterfaceLuid.Value &&
                        !ConvertInterfaceLuidToIndex(&converted, &index) && index == row->InterfaceIndex &&
                        !ConvertInterfaceLuidToGuid(&converted, &guid), "index/LUID/GUID parameter queries bypass missing device");
        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK) {
            loopbacks++;interface_check(!row->PhysicalAddressLength, "loopback has no invented hardware MAC");
        }
        for (struct ifaddrs *entry = host; entry; entry = entry->ifa_next) {
            if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_PACKET) continue;
            struct sockaddr_ll *link = (struct sockaddr_ll *)entry->ifa_addr;
            if ((unsigned)link->sll_ifindex != row->InterfaceIndex) continue;
            if (!(entry->ifa_flags & IFF_LOOPBACK) && link->sll_halen)
                interface_check(row->PhysicalAddressLength == link->sll_halen &&
                                !memcmp(row->PhysicalAddress, link->sll_addr, link->sll_halen), "reported MAC is the actual interface MAC");
            interface_check(row->AdminStatus == (entry->ifa_flags & IFF_UP ? MIB_IF_ADMIN_STATUS_UP : MIB_IF_ADMIN_STATUS_DOWN),
                            "administrative status reflects actual flags");
        }
    }
    interface_check(loopbacks, "actual loopback interface remains in GetIfTable2");
    freeifaddrs(host);free(table);
    ULONG bytes = 0;
    err = GetIfTable(NULL, &bytes, TRUE);
    interface_check(err == ERROR_INSUFFICIENT_BUFFER && bytes >= sizeof(MIB_IFTABLE), "legacy GetIfTable reports required size");
    if (err == ERROR_INSUFFICIENT_BUFFER) {
        MIB_IFTABLE *legacy = malloc(bytes);assert(legacy);
        interface_check(!GetIfTable(legacy, &bytes, TRUE) && legacy->dwNumEntries, "legacy GetIfTable enumerates same real adapters");free(legacy);
    }
    bytes = 0;err = GetIpAddrTable(NULL, &bytes, FALSE);
    interface_check(err == ERROR_INSUFFICIENT_BUFFER && bytes >= sizeof(MIB_IPADDRTABLE), "legacy GetIpAddrTable reports required size");
    if (err == ERROR_INSUFFICIENT_BUFFER) {
        MIB_IPADDRTABLE *ipv4 = malloc(bytes);assert(ipv4);
        interface_check(!GetIpAddrTable(ipv4, &bytes, FALSE) && ipv4->dwNumEntries, "legacy IPv4 table enumerates actual addresses");
        unsigned loop = 0;
        for (ULONG i = 0; i < ipv4->dwNumEntries; i++) if (ipv4->table[i].dwAddr == htonl(INADDR_LOOPBACK)) loop++;
        interface_check(loop, "legacy IPv4 table includes actual127.0.0.1");free(ipv4);
    }
}
static void check_adapters(void)
{
    const ULONG flags = GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_PREFIX;
    ULONG size = 15360;
    IP_ADAPTER_ADDRESSES *adapters = malloc(size);assert(adapters);
    DWORD err = GetAdaptersAddresses(WS_AF_UNSPEC, flags, NULL, adapters, &size);
    if (err == ERROR_BUFFER_OVERFLOW) { free(adapters);adapters = malloc(size);assert(adapters);err = GetAdaptersAddresses(WS_AF_UNSPEC, flags, NULL, adapters, &size); }
    interface_check(!err, "JDK17 exact GetAdaptersAddresses flags work without Nsi service");
    if (!err) {
        unsigned v4_loop = 0, v6_loop = 0, count = 0;
        for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter; adapter = adapter->Next) {
            count++;interface_check(adapter->IfIndex && adapter->Luid.Value, "adapter identity is nonzero and consistent");
            for (IP_ADAPTER_UNICAST_ADDRESS *uni = adapter->FirstUnicastAddress; uni; uni = uni->Next) {
                interface_check(uni->OnLinkPrefixLength <= (uni->Address.lpSockaddr->sa_family == WS_AF_INET ? 32 : 128), "prefix length is valid for address family");
                if (uni->Address.lpSockaddr->sa_family == WS_AF_INET) {
                    SOCKADDR_IN *addr = (SOCKADDR_IN *)uni->Address.lpSockaddr;
                    if (addr->sin_addr.WS_s_addr == htonl(INADDR_LOOPBACK)) v4_loop++;
                } else if (uni->Address.lpSockaddr->sa_family == WS_AF_INET6) {
                    SOCKADDR_IN6 *addr = (SOCKADDR_IN6 *)uni->Address.lpSockaddr;
                    const unsigned char loop[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
                    if (!memcmp(&addr->sin6_addr, loop, 16)) v6_loop++;
                }
            }
        }
        interface_check(count && v4_loop && v6_loop, "real dual-stack loopback addresses survive adapter assembly");
    }
    free(adapters);
    size = 0;err = GetAdaptersInfo(NULL, &size);
    interface_check(err == ERROR_BUFFER_OVERFLOW && size, "legacy GetAdaptersInfo size probe reaches actual adapters");
    if (err == ERROR_BUFFER_OVERFLOW) {
        IP_ADAPTER_INFO *legacy = malloc(size);assert(legacy);
        interface_check(!GetAdaptersInfo(legacy, &size), "legacy adapter details/routes populate without device manager");free(legacy);
    }
    interface_check(GetAdaptersAddresses(WS_AF_UNSPEC, flags, NULL, NULL, NULL) == ERROR_INVALID_PARAMETER &&
                    GetAdaptersInfo(NULL, NULL) == ERROR_INVALID_PARAMETER && GetIfTable2(NULL) == ERROR_INVALID_PARAMETER,
                    "public null-size/output parameter rejection stays Windows-equivalent");
}
static void check_nsi_buffers(void)
{
    struct { UINT64 before;NET_LUID key;UINT64 after; } guarded = {0x123456789abcdef0ULL, {0}, 0xfedcba9876543210ULL};
    struct nsi_enumerate_all_ex enumeration = {0};
    struct nsi_get_parameter_ex parameter = {0};
    struct nsi_get_all_parameters_ex all = {0};
    struct nsi_ndis_ifinfo_static info;
    UINT index = 0;
    enumeration.module = &NPI_MS_NDIS_MODULEID;
    enumeration.table = NSI_NDIS_IFINFO_TABLE;
    DWORD error = NsiEnumerateObjectsAllParametersEx(&enumeration);
    UINT_PTR required = enumeration.count;
    interface_check(!error && required, "NSI count-only interface query returns actual row count");
    enumeration.key_data = &guarded.key;enumeration.key_size = sizeof(guarded.key);enumeration.count = 1;
    error = NsiEnumerateObjectsAllParametersEx(&enumeration);
    interface_check(error == (required > 1 ? ERROR_MORE_DATA : ERROR_SUCCESS) && guarded.key.Value &&
                    guarded.before == 0x123456789abcdef0ULL && guarded.after == 0xfedcba9876543210ULL,
                    "NSI partial-capacity enumeration preserves exact key buffer boundaries");
    enumeration.key_size--;
    interface_check(NsiEnumerateObjectsAllParametersEx(&enumeration) == ERROR_INVALID_PARAMETER,
                    "NSI rejects wrong key element size");
    enumeration.key_size = sizeof(guarded.key);enumeration.key_data = NULL;
    interface_check(NsiEnumerateObjectsAllParametersEx(&enumeration) == ERROR_INVALID_PARAMETER,
                    "NSI rejects null nonempty enumeration buffers");
    parameter.module = &NPI_MS_NDIS_MODULEID;parameter.table = NSI_NDIS_IFINFO_TABLE;
    parameter.key = &guarded.key;parameter.key_size = sizeof(guarded.key);
    parameter.param_type = NSI_PARAM_TYPE_STATIC;parameter.data = &index;parameter.data_size = sizeof(index);
    parameter.data_offset = FIELD_OFFSET(struct nsi_ndis_ifinfo_static, if_index);
    interface_check(!NsiGetParameterEx(&parameter) && index, "NSI parameter slice returns real interface index");
    parameter.data_offset = UINT_MAX;index = 0x76543210;
    interface_check(NsiGetParameterEx(&parameter) == ERROR_INVALID_PARAMETER && index == 0x76543210,
                    "NSI overflowing parameter offset is rejected without writes");
    parameter.data_offset = 0;parameter.data_size = UINT_MAX;
    interface_check(NsiGetParameterEx(&parameter) == ERROR_INVALID_PARAMETER && index == 0x76543210,
                    "NSI oversized parameter slice is rejected without writes");
    parameter.data_size = sizeof(index);parameter.data = NULL;
    interface_check(NsiGetParameterEx(&parameter) == ERROR_INVALID_PARAMETER,
                    "NSI rejects null nonempty parameter output");
    all.module = &NPI_MS_NDIS_MODULEID;all.table = NSI_NDIS_IFINFO_TABLE;
    all.key = &guarded.key;all.key_size = sizeof(guarded.key);all.static_data = &info;all.static_size = sizeof(info);
    interface_check(!NsiGetAllParametersEx(&all) && info.if_index &&
                    !ConvertInterfaceLuidToIndex(&guarded.key, &index) && info.if_index == index,
                    "NSI all-parameter getter returns consistent actual identity");
    all.key = NULL;
    interface_check(NsiGetAllParametersEx(&all) == ERROR_INVALID_PARAMETER,
                    "NSI rejects missing nonempty lookup key");
}

static void check_wow64_marshalling(void)
{
    struct slab
    {
        NPI_MODULEID module;
        NET_LUID key, converted;
        struct nsi_ndis_ifinfo_static info;
        struct { UINT unknown[2], module, table, first_arg, second_arg, keys, key_size;
                 UINT rw, rw_size, dynamic, dynamic_size, stat, stat_size, count; } enumeration;
        struct { UINT unknown[2], module, table, first_arg, unknown2, key, key_size;
                 UINT rw, rw_size, dynamic, dynamic_size, stat, stat_size; } all;
        struct { UINT unknown[2], module, table, first_arg, unknown2, key, key_size;
                 UINT param_type, data, data_size, offset; } parameter;
    } *frame = mmap(NULL, sizeof(*frame), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(frame != MAP_FAILED && (uintptr_t)frame + sizeof(*frame) <= UINT_MAX);
#define NSI_WOW_PTR(value) ((UINT)(uintptr_t)(value))
    frame->module = NPI_MS_NDIS_MODULEID;
    frame->enumeration.module = NSI_WOW_PTR(&frame->module);
    frame->enumeration.table = NSI_NDIS_IFINFO_TABLE;
    NTSTATUS status = __wine_unix_call_wow64_funcs[1](&frame->enumeration);
    interface_check(!status && frame->enumeration.count, "WOW64 count is copied back in 32-bit wire layout");
    frame->enumeration.keys = NSI_WOW_PTR(&frame->key);
    frame->enumeration.key_size = sizeof(frame->key);frame->enumeration.count = 1;
    status = __wine_unix_call_wow64_funcs[1](&frame->enumeration);
    interface_check((!status || status == STATUS_BUFFER_OVERFLOW) && frame->key.Value,
                    "WOW64 enumeration converts 32-bit output and module pointers");
    frame->all.module = NSI_WOW_PTR(&frame->module);frame->all.table = NSI_NDIS_IFINFO_TABLE;
    frame->all.key = NSI_WOW_PTR(&frame->key);frame->all.key_size = sizeof(frame->key);
    frame->all.stat = NSI_WOW_PTR(&frame->info);frame->all.stat_size = sizeof(frame->info);
    interface_check(!__wine_unix_call_wow64_funcs[2](&frame->all) && frame->info.if_index,
                    "WOW64 all-parameter getter converts key and column pointers");
    frame->parameter.module = NSI_WOW_PTR(&frame->module);frame->parameter.table = NSI_NDIS_INDEX_LUID_TABLE;
    frame->parameter.key = NSI_WOW_PTR(&frame->info.if_index);frame->parameter.key_size = sizeof(frame->info.if_index);
    frame->parameter.param_type = NSI_PARAM_TYPE_STATIC;
    frame->parameter.data = NSI_WOW_PTR(&frame->converted);frame->parameter.data_size = sizeof(frame->converted);
    interface_check(!__wine_unix_call_wow64_funcs[3](&frame->parameter) && frame->converted.Value == frame->key.Value,
                    "WOW64 parameter getter preserves index/LUID association");
#undef NSI_WOW_PTR
    assert(!munmap(frame, sizeof(*frame)));
}

int main(void)
{
    check_ndis_and_public_tables();check_adapters();check_nsi_buffers();check_wow64_marshalling();
    interface_check(!device_attempts, "title interface/address paths never attempt absent Nsi device");
    fault_ifaddrs = EACCES;MIB_IF_TABLE2 *table = NULL;
    interface_check(GetIfTable2(&table) == ERROR_ACCESS_DENIED && !table, "actual interface-provider failure propagates, never fake empty success");
    fault_ifaddrs = 0;title_role = 0;unsigned before = interface_queries;
    interface_check(GetIfTable2(&table) == ERROR_FILE_NOT_FOUND && interface_queries == before && device_attempts,
                    "non-title queries retain original NSI device fallback");
    if (interface_failures) return 1;
    puts("NSI_INTERFACE_PUBLIC_APIS_PASS missing_device=1 data=REAL_HOST interfaces,IPv4,IPv6,MAC,legacy,GAA_scope_not_console");
    return 0;
}
