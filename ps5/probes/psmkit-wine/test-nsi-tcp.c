/* Real Wine NSI TCP path: forked socket owners, no service/device manager. */
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static void *host_cmsg_data(struct cmsghdr *cmsg) { return CMSG_DATA(cmsg); }
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#define USE_WS_PREFIX
#define __WINE_WINDOWS_H
#include "winsock2.h"
#include "ws2ipdef.h"
#include "ifdef.h"
#define __WINE_INIT_NPI_MODULEID
#include "netiodef.h"
#include "tcpmib.h"
#include "iphlpapi.h"
#include "wine/nsi.h"
#include "ntuser.h"
#undef SEND_HWMSG_INJECTED
#include "wine/server_protocol.h"

#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
static int ops;
#define sock_ops ops
struct object { const int *ops; };
struct fd { int fd; };
struct sock
{
    struct object obj;
    struct fd *fd;
    unsigned int type, family;
    union { struct WS_sockaddr_in in; struct WS_sockaddr_in6 in6; } addr, peer_addr;
    unsigned int peer_addr_len;
};
struct process { unsigned int id; };
static struct process processes[] = {{0x40}, {0x88}, {0xcc}};
static struct fd descriptors[3];
static struct sock sockets[3];
static unsigned int socket_count;
static int get_unix_fd(struct fd *fd) { return fd->fd; }
static MIB_TCP_STATE get_tcp_socket_state(int fd)
{
    struct tcp_info info;
    socklen_t size = sizeof(info);
    assert(!getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &size));
    return info.tcpi_state == TCP_LISTEN ? MIB_TCP_STATE_LISTEN : MIB_TCP_STATE_ESTAB;
}
#include "nsi-tcp-server.inc"

static void enum_handles_of_type(const int *type,
    int (*callback)(struct process *, struct object *, void *), void *user)
{
    unsigned int i;
    assert(type == &sock_ops);
    for (i = 0; i < socket_count; i++) callback(&processes[i], &sockets[i].obj, user);
}
static struct { unsigned int state_filter; } request;
static struct { unsigned int count; } response;
static void *reply_data;
static size_t reply_size;
#define SERVER_START_REQ(name) do { __typeof__(request) *req = &request; __typeof__(response) *reply = &response;
#define SERVER_END_REQ } while (0)
static void wine_server_set_reply(void *req, void *data, size_t size)
{
    (void)req;
    reply_data = data;
    reply_size = size;
}
static NTSTATUS server_status;
static NTSTATUS wine_server_call(void *req)
{
    struct enum_tcp_connection_info info = {request.state_filter, 0, NULL};
    if (server_status) return server_status;
    enum_handles_of_type(&sock_ops, enum_tcp_connections, &info);
    response.count = info.count;
    if (info.count > reply_size / sizeof(union tcp_connection)) return STATUS_BUFFER_TOO_SMALL;
    info.conn = reply_data;
    if (info.count) enum_handles_of_type(&sock_ops, enum_tcp_connections, &info);
    return STATUS_SUCCESS;
}
struct ipv6_addr_scope { IN6_ADDR addr; UINT scope; };
static struct ipv6_addr_scope *get_ipv6_addr_scope_table(unsigned int *size) { *size = 0; return NULL; }
static UINT find_ipv6_addr_scope(const IN6_ADDR *addr, const struct ipv6_addr_scope *table, unsigned int count)
{ return 0; }
static int title_role = 1;
static int __wine_ps5_is_title(void) { return title_role; }
static TEB test_teb;
#undef NtCurrentTeb
#define NtCurrentTeb() (&test_teb)
static unsigned int klog_queries, klog_rows;
static int sceKernelDebugOutText(int channel, const char *text)
{
    const char *p;
    assert(channel == 0 && strlen(text) < 256 && !strchr(text, '/') && !strchr(text, '\\'));
    if (!strncmp(text, "wine: nsi tcp query ", 19)) klog_queries++;
    else
    {
        assert(!strncmp(text, "wine: nsi tcp loopback ", 22));
        klog_rows++;
    }
    for (p = text; *p; p++) assert(*p == '\n' || (*p >= 32 && *p < 127));
    return 0;
}
#ifdef NSI_INTERFACE_TEST
#include "nsi-if-unix.inc"
#else
#include "nsi-tcp-unix.inc"
#endif

static unsigned int device_attempts;
static HANDLE get_nsi_device(BOOL async) { device_attempts++; return INVALID_HANDLE_VALUE; }
#define GetLastError() ERROR_FILE_NOT_FOUND
#define GetProcessHeap() NULL
#ifdef NSI_INTERFACE_TEST
static void *test_interface_heap(DWORD flags, SIZE_T size)
{ return flags & HEAP_ZERO_MEMORY ? calloc(1, size) : malloc(size); }
#define HeapAlloc(heap, flags, size) test_interface_heap(flags, size)
#else
#define HeapAlloc(heap, flags, size) malloc(size)
#endif
#define HeapFree(heap, flags, data) free(data)
static void WINAPI test_free_table(void *key, void *rw, void *dyn, void *stat)
{ free(key); free(rw); free(dyn); free(stat); }
#define NsiFreeTable test_free_table
static BOOL test_device_io(HANDLE device, DWORD code, void *in, DWORD in_size,
                           void *out, DWORD out_size, DWORD *received, void *overlapped)
{ assert(0 && "missing device must not receive an ioctl"); return FALSE; }
#define DeviceIoControl test_device_io
static DWORD test_status_error(NTSTATUS status)
{
    if (status == STATUS_SUCCESS) return ERROR_SUCCESS;
    if (status == STATUS_BUFFER_OVERFLOW) return ERROR_MORE_DATA;
    if (status == STATUS_NOT_SUPPORTED) return ERROR_NOT_SUPPORTED;
    if (status == STATUS_INVALID_PARAMETER) return ERROR_INVALID_PARAMETER;
    if (status == STATUS_ACCESS_DENIED) return ERROR_ACCESS_DENIED;
#ifdef NSI_INTERFACE_TEST
    if (status == STATUS_NO_MEMORY) return ERROR_NOT_ENOUGH_MEMORY;
    if (status == STATUS_NO_MORE_ENTRIES) return ERROR_NO_MORE_ITEMS;
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) return ERROR_FILE_NOT_FOUND;
    if (status == STATUS_NOT_FOUND) return ERROR_NOT_FOUND;
    if (status == STATUS_UNSUCCESSFUL) return ERROR_GEN_FAILURE;
#endif
    assert(0 && "unexpected NTSTATUS"); return ERROR_GEN_FAILURE;
}
#define RtlNtStatusToDosError test_status_error
#ifdef NSI_INTERFACE_TEST
#define WINE_UNIX_CALL(code, args) ((code) < ARRAY_SIZE(__wine_unix_call_funcs) ? \
                                  __wine_unix_call_funcs[code](args) : STATUS_NOT_SUPPORTED)
#else
#define WINE_UNIX_CALL(code, args) unix_enumerate_tcp(args)
#endif
#define __wine_unixlib_handle 1
#include "nsi-tcp-pe.inc"
#ifdef NSI_INTERFACE_TEST
/* These extracted PE functions consume Windows family numbers; provider
 * objects use native families in separate translation units. */
#undef AF_INET6
#define AF_INET6 WS_AF_INET6
#endif
#define s_addr WS_s_addr
#include "nsi-tcp-api.inc"
#undef s_addr

static void test_public_tables(void)
{
    DWORD size = 0, i;
    BOOL found = FALSE;
    MIB_TCPTABLE2 *table;
    assert(GetTcpTable2(NULL, &size, TRUE) == ERROR_INSUFFICIENT_BUFFER);
    table = malloc(size);
    assert(table && !GetTcpTable2(table, &size, TRUE) && table->dwNumEntries == 3);
    for (i = 0; i < table->dwNumEntries; i++)
        if (table->table[i].dwLocalPort == sockets[1].addr.in.sin_port)
        {
            assert(table->table[i].dwOwningPid == processes[1].id);
            assert(table->table[i].dwRemotePort == sockets[2].addr.in.sin_port);
            found = TRUE;
        }
    assert(found);
    assert(!GetExtendedTcpTable(table, &size, FALSE, WS_AF_INET, TCP_TABLE_OWNER_PID_ALL, 0));
    assert(((MIB_TCPTABLE_OWNER_PID *)table)->table[1].dwOwningPid == processes[1].id);
    free(table);
}

static void test_query_boundaries(void)
{
    struct nsi_enumerate_all_ex params = {.module = &NPI_MS_TCP_MODULEID, .table = NSI_TCP_ALL_TABLE};
    struct nsi_tcp_conn_key key, original;
    assert(!NsiEnumerateObjectsAllParametersEx(&params) && params.count == 3);
    memset(&key, 0xa5, sizeof(key));
    original = key;
    params.key_data = &key;
    params.key_size = sizeof(key);
    params.count = 1;
    assert(NsiEnumerateObjectsAllParametersEx(&params) == ERROR_MORE_DATA && params.count == 1);
    assert(!memcmp(&key, &original, sizeof(key)));
    params.key_size--;
    assert(NsiEnumerateObjectsAllParametersEx(&params) == ERROR_INVALID_PARAMETER);
    params.key_size++;
    server_status = STATUS_ACCESS_DENIED;
    assert(NsiEnumerateObjectsAllParametersEx(&params) == ERROR_ACCESS_DENIED);
    server_status = STATUS_SUCCESS;
    params.key_data = NULL;
    assert(NsiEnumerateObjectsAllParametersEx(&params) == ERROR_INVALID_PARAMETER);
    params.key_size = 0;
    for (unsigned int i = 0; i < 40; i++) assert(!NsiEnumerateObjectsAllParametersEx(&params));
    assert(klog_queries == 32 && klog_rows > 0 && klog_rows <= 32 * 16);
}

static void record_socket(unsigned int i, int fd)
{
    struct sockaddr_in local, peer;
    socklen_t size = sizeof(local);
    struct sock *sock = &sockets[i];
    descriptors[i].fd = fd;
    sock->obj.ops = &sock_ops;
    sock->fd = &descriptors[i];
    sock->family = WS_AF_INET;
    sock->type = WS_SOCK_STREAM;
    assert(!getsockname(fd, (struct sockaddr *)&local, &size));
    sock->addr.in.sin_family = WS_AF_INET;
    sock->addr.in.sin_addr.WS_s_addr = local.sin_addr.s_addr;
    sock->addr.in.sin_port = local.sin_port;
    size = sizeof(peer);
    if (!getpeername(fd, (struct sockaddr *)&peer, &size))
    {
        sock->peer_addr.in.sin_family = WS_AF_INET;
        sock->peer_addr.in.sin_addr.WS_s_addr = peer.sin_addr.s_addr;
        sock->peer_addr.in.sin_port = peer.sin_port;
        sock->peer_addr_len = sizeof(sock->peer_addr.in);
    }
}
static DWORD get_table(DWORD table, struct nsi_tcp_conn_key **key,
                       struct nsi_tcp_conn_dynamic **dyn, struct nsi_tcp_conn_static **stat, DWORD *count)
{
    return NsiAllocateAndGetTable(1, &NPI_MS_TCP_MODULEID, table, (void **)key, sizeof(**key),
                                  NULL, 0, (void **)dyn, sizeof(**dyn), (void **)stat, sizeof(**stat), count, 0);
}
int main(void)
{
    int listener, pair[2], accepted, child_fd, status;
    pid_t child;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    struct nsi_tcp_conn_key *keys;
    struct nsi_tcp_conn_dynamic *dyn;
    struct nsi_tcp_conn_static *stat;
    DWORD count = 0, err;
    char byte = 'x', control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {&byte, 1};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                              .msg_controllen = sizeof(control)};
    struct cmsghdr *cmsg;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0 && !bind(listener, (struct sockaddr *)&address, sizeof(address)));
    assert(!listen(listener, 2) && !getsockname(listener, (struct sockaddr *)&address, &length));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    child = fork();
    assert(child >= 0);
    if (!child)
    {
        int client = socket(AF_INET, SOCK_STREAM, 0);
        close(pair[0]); close(listener);
        assert(client >= 0 && !connect(client, (struct sockaddr *)&address, sizeof(address)));
        memset(control, 0, sizeof(control));
        cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(client));
        memcpy(host_cmsg_data(cmsg), &client, sizeof(client));
        assert(sendmsg(pair[1], &message, 0) == 1);
        assert(read(pair[1], &byte, 1) == 1);
        close(client); close(pair[1]); _exit(0);
    }
    close(pair[1]);
    accepted = accept(listener, NULL, NULL);
    assert(accepted >= 0 && recvmsg(pair[0], &message, 0) == 1);
    cmsg = CMSG_FIRSTHDR(&message);
    assert(cmsg && cmsg->cmsg_type == SCM_RIGHTS);
    memcpy(&child_fd, host_cmsg_data(cmsg), sizeof(child_fd));
    record_socket(0, listener);
    record_socket(1, child_fd);
    record_socket(2, accepted);
    socket_count = 3;
    test_teb.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)processes[0].id;
    err = get_table(NSI_TCP_ALL_TABLE, &keys, &dyn, &stat, &count);
    if (err)
    {
        fprintf(stderr, "NSI_TCP_RED: missing NSI service makes real TCP query fail: error=%u expected=0\n", err);
        assert(write(pair[0], &byte, 1) == 1);
        waitpid(child, &status, 0);
        return 1;
    }
    assert(count == 3 && !device_attempts);
    assert(stat[0].pid == processes[0].id && stat[1].pid == processes[1].id && stat[2].pid == processes[2].id);
    assert(dyn[0].state == MIB_TCP_STATE_LISTEN && dyn[1].state == MIB_TCP_STATE_ESTAB);
    assert(keys[1].local.Ipv4.sin_port == keys[2].remote.Ipv4.sin_port);
    assert(keys[1].remote.Ipv4.sin_port == keys[2].local.Ipv4.sin_port);
    assert(keys[1].local.Ipv4.sin_addr.WS_s_addr == htonl(INADDR_LOOPBACK));
    test_free_table(keys, NULL, dyn, stat);
    err = get_table(NSI_TCP_LISTEN_TABLE, &keys, &dyn, &stat, &count);
    assert(!err && count == 1 && stat[0].pid == processes[0].id);
    test_free_table(keys, NULL, dyn, stat);
    err = get_table(NSI_TCP_ESTAB_TABLE, &keys, &dyn, &stat, &count);
    assert(!err && count == 2 && stat[0].pid == processes[1].id && stat[1].pid == processes[2].id);
    test_free_table(keys, NULL, dyn, stat);
    test_public_tables();
    test_query_boundaries();
    /* Other modules still use the real device path, never a made-up success. */
    err = NsiAllocateAndGetTable(1, &NPI_MS_UDP_MODULEID, NSI_UDP_ENDPOINT_TABLE,
                                 NULL, 0, NULL, 0, NULL, 0, NULL, 0, &count, 0);
    assert(err == ERROR_FILE_NOT_FOUND && device_attempts == 1);
    assert(write(pair[0], &byte, 1) == 1 && waitpid(child, &status, 0) == child && status == 0);
    close(pair[0]); close(child_fd); close(accepted); close(listener);
    puts("NSI_TCP_PASS missing_service=OK forked_child_owner=EXACT unrelated_owner=PRESERVED loopback_tuple=EXACT filters=OK unrelated_module=REJECTED klog=BOUNDED");
    return 0;
}
