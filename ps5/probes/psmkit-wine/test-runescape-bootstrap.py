#!/usr/bin/env python3
"""Compile the real managed PE gate and exercise native maintenance failure hold.

This is a host build/behavior contract, not Authenticode qualification.
Positive/hostile chain and revocation receipts require the real Wine runtime.
"""
import json
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from runtime_profiles import bootstrap_header, profile

here = Path(__file__).resolve().parent
selected = profile('b1', json.loads((here / 'b1-product.json').read_text()))
header = bootstrap_header(selected)
assert '#define RS_PUBLISHER L"Jagex Limited"' in header
assert '#define RS_INSTALLER_ARGUMENTS L"--silent --install"' in header
assert '--single-process' not in header
# The managed launcher entry must carry the same CPU compositor policy as
# Wine's named-program launch path, including after a launcher self-update.
browser_arguments = ' '.join(selected['browser_switches']['JagexLauncher.exe'])
assert f'#define RS_BROWSER_ARGUMENTS L"{browser_arguments}"' in header
for switch in ('--disable-gpu', '--disable-gpu-compositing', '--disable-software-rasterizer'):
    assert switch in browser_arguments, switch
source = (here / 'runescape-bootstrap.c').read_text()
assert '#define RS_APP L"C:\\\\RuneScape\\\\"' in source
assert 'Z:\\\\app0' not in source
for required in ('WinVerifyTrust(', 'WTHelperGetProvSignerFromChain(',
                 'CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED',
                 'CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT',
                 'config.hExclusiveRoot=roots', 'CertVerifyTimeValidity(&now,',
                 'CertVerifyRevocation(', '|| result.dwError', 'CERT_CHAIN_POLICY_BASE',
                 'CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG', 'szOID_PKIX_KP_CODE_SIGNING',
                 'SPC_PE_IMAGE_DATA_OBJID', 'JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE'):
    assert required in source, required
assert 'WTD_REVOCATION_CHECK_NONE' not in source
assert 'CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY' not in source
assert 'WTD_CACHE_ONLY_URL_RETRIEVAL' not in source
assert source.index('if(!rs_prepare_redists())') < source.index('rs_launch_launcher(RS_LAUNCHER,')
# Bootstrap diagnostics must use Wine's direct debug writer, not the CRT's
# stdout handle, which is not a reliable klog route in the title parent.
assert 'GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"__wine_dbg_write")' in source
assert 'prerequisite msi-enum arch=' in source
assert 'prerequisite msi-product arch=' in source
assert 'prerequisite launch stage=' in source
assert 'ProcessWineGrantAdminToken' not in source
assert 'rs_launch_admin(RS_LAUNCHER,' not in source
assert 'launcher window timeout; restarting attempt=%u' in source
logger_function = 'static void rs_logf(' + source.split('static void rs_logf(', 1)[1].split(
    'static void rs_log(', 1)[0]
logger_harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define WINAPI
static char captured[1024];
static unsigned writes,lookups;
static int direct;
static int debug_sink(const char *text,unsigned length)
{
    assert(length<sizeof(captured) && text[length]==0);
    memcpy(captured,text,length+1);writes++;return (int)length;
}
static void *GetModuleHandleW(const wchar_t *name)
{ assert(!wcscmp(name,L"ntdll.dll"));return (void *)1; }
static void *GetProcAddress(void *module,const char *name)
{
    assert(module==(void *)1 && !strcmp(name,"__wine_dbg_write"));
    lookups++;return direct ? (void *)debug_sink : NULL;
}
'''
logger_cases = r'''
int main(int argc,char **argv)
{
    (void)argv;direct=argc==1;
    rs_logf("check=%u value=%s",42,"ready");
    if(direct) {
        assert(writes==1 && !strcmp(captured,"runescape: check=42 value=ready\n"));
        char large[2048];memset(large,'x',sizeof(large)-1);large[sizeof(large)-1]=0;
        rs_logf("%s",large);
        assert(writes==2 && strlen(captured)==1023 && captured[1022]=='\n');
    } else assert(!writes);
    rs_logf("final");
    assert(lookups==1);
    return 0;
}
'''
launch_function = 'static int rs_launch(' + source.split('static int rs_launch(', 1)[1].split(
    'struct rs_launcher_processes {', 1)[0]
launch_harness = r'''
#include <assert.h>
#include <stdio.h>
#include <wchar.h>
typedef wchar_t WCHAR;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef HANDLE HWND;
typedef struct { unsigned cb; } STARTUPINFOW;
typedef struct { HANDLE hProcess,hThread; } PROCESS_INFORMATION;
typedef struct { struct { unsigned LimitFlags; } BasicLimitInformation; } JOBOBJECT_EXTENDED_LIMIT_INFORMATION;
typedef struct { DWORD ActiveProcesses; } JOBOBJECT_BASIC_ACCOUNTING_INFORMATION;
#define INFINITE 0xffffffff
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 1
#define JobObjectExtendedLimitInformation 1
#define JobObjectBasicAccountingInformation 2
#define CREATE_SUSPENDED 1
#define FALSE 0
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define ERROR_TIMEOUT 1460
#define ERROR_ACCESS_DENIED 5
static unsigned desktop_calls,jobs,created,closed_jobs,terminated,slept;
static int desktop_ready,query_ok,assigned,exit_ok;
static DWORD active,wait_result;
static void rs_logf(const char *format,...) { assert(format); }
static DWORD GetLastError(void) { return 5; }
static HWND GetDesktopWindow(void) { desktop_calls++;return desktop_ready ? (HWND)1 : NULL; }
static HANDLE CreateJobObjectW(void *a,void *b)
{ assert(!a && !b && desktop_calls==1 && desktop_ready);jobs++;return (HANDLE)2; }
static int SetInformationJobObject(HANDLE h,unsigned kind,void *data,unsigned size)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION *limits=data;
    assert(h==(HANDLE)2 && kind==1 && size==sizeof(*limits));
    assert(limits->BasicLimitInformation.LimitFlags==JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);return 1;
}
static int CreateProcessW(const WCHAR *path,WCHAR *command,void *a,void *b,int inherit,
                         unsigned flags,void *env,const WCHAR *cwd,STARTUPINFOW *si,PROCESS_INFORMATION *pi)
{
    assert(path && command && !a && !b && !inherit && flags==CREATE_SUSPENDED && !env && cwd);
    assert(si->cb==sizeof(*si) && jobs==1);created++;
    pi->hProcess=(HANDLE)3;pi->hThread=(HANDLE)4;return 1;
}
static int AssignProcessToJobObject(HANDLE job,HANDLE process)
{ assert(job==(HANDLE)2 && process==(HANDLE)3);return assigned; }
static DWORD ResumeThread(HANDLE thread) { assert(thread==(HANDLE)4);return 1; }
static int TerminateProcess(HANDLE process,DWORD error)
{ assert(process==(HANDLE)3 && (error==ERROR_TIMEOUT || error==ERROR_ACCESS_DENIED));terminated++;return 1; }
static int CloseHandle(HANDLE h) { if(h==(HANDLE)2) closed_jobs++;return 1; }
static DWORD WaitForSingleObject(HANDLE h,DWORD timeout)
{ assert(h==(HANDLE)3 && (timeout==600000 || timeout==10000));return wait_result; }
static int GetExitCodeProcess(HANDLE h,DWORD *exit)
{ assert(h==(HANDLE)3);*exit=0;return exit_ok; }
static int QueryInformationJobObject(HANDLE h,unsigned kind,void *data,unsigned size,void *out)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *accounting=data;
    assert(h==(HANDLE)2 && kind==2 && size==sizeof(*accounting) && !out);
    accounting->ActiveProcesses=active;return query_ok;
}
static void Sleep(unsigned ms) { assert(ms==10);slept+=ms; }
'''
launch_cases = r'''
static void reset_launch(void)
{
    desktop_calls=jobs=created=closed_jobs=terminated=slept=0;
    desktop_ready=query_ok=assigned=exit_ok=1;active=wait_result=0;
}
int main(void)
{
    DWORD exit=31;
    reset_launch();assert(rs_launch(L"redist",L"/install",L"cwd",600000,&exit));
    assert(exit==0 && created==1 && closed_jobs==1 && !slept && !terminated);
    reset_launch();desktop_ready=0;assert(!rs_launch(L"redist",L"",L"cwd",600000,&exit));
    assert(!jobs && !created);
    reset_launch();active=1;assert(!rs_launch(L"redist",L"",L"cwd",600000,&exit));
    assert(exit==0 && slept==10000 && closed_jobs==1); /* no blanket pass after root exit */
    reset_launch();query_ok=0;assert(!rs_launch(L"redist",L"",L"cwd",600000,&exit));
    reset_launch();exit_ok=0;assert(!rs_launch(L"redist",L"",L"cwd",600000,&exit));
    reset_launch();wait_result=WAIT_TIMEOUT;assert(!rs_launch(L"redist",L"",L"cwd",600000,&exit));
    assert(exit==ERROR_TIMEOUT && terminated==1);
    reset_launch();assigned=0;assert(!rs_launch(L"redist",L"",L"cwd",600000,&exit));
    assert(terminated==1 && closed_jobs==1);
    puts("runescape prerequisite jobs: desktop outside job, strict descendant drain and failures PASS");
    return 0;
}
'''
# Compile the production MSI readiness/elevation boundaries, replacing Win32
# services only. Fake installer state is confined to this host test.
prerequisite_functions = source.split('static int rs_launch_admin(', 1)[1].split(
    'static int rs_reg_string(', 1)[0]
prerequisite_functions = 'static int rs_launch_admin(' + prerequisite_functions
prerequisite_harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "runescape-redist.h"
typedef wchar_t WCHAR;
typedef int INSTALLSTATE;
typedef unsigned UINT;
static void rs_logf(const char *format,...) { assert(format); }
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef HANDLE HKEY;
typedef long LONG;
#define TRUE 1
#define FALSE 0
#define HKEY_LOCAL_MACHINE ((HKEY)8)
#define KEY_READ 1
#define KEY_WOW64_32KEY 2
#define ERROR_FILE_NOT_FOUND 2
typedef struct { unsigned cbSize,fMask;const WCHAR *lpVerb,*lpFile,*lpParameters,*lpDirectory;int nShow;HANDLE hProcess; } SHELLEXECUTEINFOW;
typedef struct { long long QuadPart; } LARGE_INTEGER;
#define RS_APP L"C:\\RuneScape\\"
#define ERROR_SUCCESS 0
#define ERROR_GEN_FAILURE 31
#define INSTALLSTATE_DEFAULT 5
#define INSTALLPROPERTY_VERSIONSTRING "VersionString"
#define INFINITE 0xffffffff
#define WAIT_OBJECT_0 0
#define SEE_MASK_NOCLOSEPROCESS 1
#define SEE_MASK_NOASYNC 2
#define SEE_MASK_FLAG_NO_UI 4
#define SW_SHOWNORMAL 1
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 8
#define GENERIC_READ 1
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
static int states[4],current,component,launches,hash_bad,missing,launch_ok,install_updates,admin_ok,wait_ok,exit_ok,closes;
static int legacy[2],removing,removals,registry_error,keep_registered;
static unsigned enum_error,version_error;
static int fail_arch;
static const struct rs_redist *current_redist;
static DWORD installer_exit;
static DWORD GetLastError(void) { return 5; }
static const char *version="14.44.35211";
static DWORD MsiEnumRelatedProductsA(const char *upgrade,DWORD reserved,DWORD index,char *product)
{
    assert(!reserved);
    if(enum_error) return enum_error;
    if(index) return 259;
    for(unsigned i=0;i<2;i++) for(unsigned j=0;j<2;j++)
        if(!strcmp(upgrade,rs_redists[i].upgrade[j])) {
            component=(int)(i*2+j);strcpy(product,"product");return 0;
        }
    abort();
}
static int MsiQueryProductStateA(const char *product) { assert(!strcmp(product,"product"));return states[component]; }
static DWORD MsiGetProductInfoA(const char *p,const char *key,char *value,DWORD *length)
{
    assert(p && !strcmp(key,INSTALLPROPERTY_VERSIONSTRING) && *length>=strlen(version)+1);
    if(version_error) return version_error;
    strcpy(value,version);return 0;
}
static LONG RegOpenKeyExA(HKEY hive,const char *path,unsigned reserved,unsigned access,HKEY *key)
{
    assert(hive==HKEY_LOCAL_MACHINE && !reserved && access==3);
    if(registry_error) return 5;
    for(unsigned i=0;i<2;i++) if(strstr(path,rs_legacy_redists[i].bundle)) {
        *key=(HKEY)(intptr_t)(i+1);return legacy[i] ? 0 : ERROR_FILE_NOT_FOUND;
    }
    abort();
}
static LONG RegCloseKey(HKEY key) { assert(key);return 0; }
static int MultiByteToWideChar(unsigned cp,unsigned flags,const char *s,int n,WCHAR *out,int cap)
{ assert(cp==CP_UTF8 && flags==MB_ERR_INVALID_CHARS && n==-1);return mbstowcs(out,s,(size_t)cap)!= (size_t)-1; }
static HANDLE CreateFileW(const WCHAR *path,unsigned a,unsigned b,void *c,unsigned d,unsigned e,void *f)
{
    assert(a==GENERIC_READ && b==FILE_SHARE_READ && !c && d==OPEN_EXISTING && !e && !f);
    current=wcsstr(path,L"x64") ? 1 : 0;
    current_redist=wcsstr(path,L"legacy") ? &rs_legacy_redists[current] : &rs_redists[current];
    return missing ? INVALID_HANDLE_VALUE : (HANDLE)(intptr_t)(current+1);
}
static int GetFileSizeEx(HANDLE h,LARGE_INTEGER *size) { assert(h);size->QuadPart=current_redist->size;return 1; }
static int rs_hash(HANDLE h,char *digest) { assert(h);strcpy(digest,hash_bad ? "bad" : current_redist->sha256);return 1; }
static int CloseHandle(HANDLE h) { assert(h);closes++;return 1; }
static int rs_launch(const WCHAR *path,const WCHAR *args,const WCHAR *cwd,DWORD timeout,DWORD *exit)
{
    assert(wcsstr(path,current ? L"x64.exe" : L"x86.exe"));
    removing=wcsstr(args,L"/uninstall")!=NULL;
    assert((removing || wcsstr(args,L"/install")) && wcsstr(args,L"/quiet /norestart /log") &&
           !wcscmp(cwd,RS_APP) && timeout==600000);
    launches++;*exit=installer_exit;
    if(current==fail_arch) { *exit=1603;return 1; }
    if(removing) { removals++;if(!keep_registered) legacy[current]=0; }
    else if(install_updates) states[current*2]=states[current*2+1]=INSTALLSTATE_DEFAULT;
    return launch_ok;
}
static int ShellExecuteExW(SHELLEXECUTEINFOW *info)
{
    assert(info->cbSize==sizeof(*info) && info->fMask==7);
    assert(!wcscmp(info->lpVerb,L"runas") && info->lpFile && info->lpParameters && info->lpDirectory);
    assert(info->nShow==SW_SHOWNORMAL);info->hProcess=admin_ok ? (HANDLE)3 : NULL;return admin_ok;
}
static DWORD WaitForSingleObject(HANDLE h,DWORD timeout) { assert(h==(HANDLE)3 && timeout==INFINITE);return wait_ok ? 0 : 0xffffffff; }
static int GetExitCodeProcess(HANDLE h,DWORD *exit) { assert(h==(HANDLE)3);*exit=42;return exit_ok; }
static HANDLE CreateEventW(void *security,int manual,int initial,const WCHAR *name)
{ assert(!security && manual && !initial && !wcscmp(name,L"RuneScapeOsrsHandoff"));return (HANDLE)4; }
static int ResetEvent(HANDLE event) { assert(event==(HANDLE)4);return 1; }
static int rs_wait_launcher(HANDLE launcher,HANDLE event,DWORD *exit)
{ assert(event==(HANDLE)4);return WaitForSingleObject(launcher,INFINITE)==WAIT_OBJECT_0 && GetExitCodeProcess(launcher,exit); }
'''
prerequisite_cases = r'''
static void reset(void)
{
    memset(states,0,sizeof(states));launches=hash_bad=missing=closes=0;
    memset(legacy,0,sizeof(legacy));removals=registry_error=keep_registered=0;
    launch_ok=install_updates=admin_ok=wait_ok=exit_ok=1;installer_exit=0;version="14.44.35211";
    enum_error=version_error=0;fail_arch=-1;
}
int main(void)
{
    reset();assert(rs_prepare_redists() && launches==2);
    assert(rs_prepare_redists() && launches==2); /* real MSI state, not a private receipt */
    reset();states[0]=states[1]=5;assert(rs_prepare_redists() && launches==1);
    reset();fail_arch=1;assert(!rs_prepare_redists() && launches==2);
    assert(states[0]==5 && states[1]==5 && !states[2] && !states[3]);
    fail_arch=-1;assert(rs_prepare_redists() && launches==3); /* retry skips x86 */
    reset();installer_exit=3010;assert(rs_prepare_redists() && launches==2);
    reset();installer_exit=1638;version="14.45.1";assert(rs_prepare_redists() && launches==2);
    reset();installer_exit=1638;install_updates=0;assert(!rs_prepare_redists() && launches==1);
    reset();installer_exit=6;assert(!rs_prepare_redists() && launches==1);
    reset();hash_bad=1;assert(!rs_prepare_redists() && !launches);
    reset();missing=1;assert(!rs_prepare_redists() && !launches);
    reset();launch_ok=0;assert(!rs_prepare_redists() && launches==1);
    reset();legacy[0]=legacy[1]=1;assert(rs_prepare_redists() && launches==4 && removals==2);
    assert(!legacy[0] && !legacy[1]);
    reset();legacy[0]=1;keep_registered=1;assert(!rs_prepare_redists() && launches==1 && removals==1);
    reset();registry_error=1;assert(!rs_prepare_redists() && !launches);
    reset();for(unsigned i=0;i<4;i++) states[i]=5;
    version="14.40.33810";assert(!rs_redist_installed(&rs_redists[0]));
    version="14.44.35210";assert(!rs_redist_installed(&rs_redists[0]));
    version="14.44.35211";assert(rs_redist_installed(&rs_redists[0]));
    version="14.44.35211junk";assert(!rs_redist_installed(&rs_redists[0]));
    version="15.0.1";assert(rs_redist_installed(&rs_redists[0]));
    enum_error=5;assert(!rs_redist_installed(&rs_redists[0]));
    enum_error=259;assert(!rs_redist_installed(&rs_redists[0]));
    enum_error=0;version_error=1605;assert(!rs_redist_installed(&rs_redists[0]));
    version_error=234;assert(!rs_redist_installed(&rs_redists[0]));
    version_error=0;states[1]=1;assert(!rs_redist_installed(&rs_redists[0]));
    states[1]=5;assert(rs_redist_installed(&rs_redists[0]));
    DWORD exit=1;reset();assert(rs_launch_admin(L"launcher",L"--arg",L"cwd",&exit) && exit==42 && closes==2);
    reset();admin_ok=0;assert(!rs_launch_admin(L"launcher",L"",L"cwd",&exit) && closes==1);
    reset();wait_ok=0;assert(!rs_launch_admin(L"launcher",L"",L"cwd",&exit) && closes==2);
    reset();exit_ok=0;assert(!rs_launch_admin(L"launcher",L"",L"cwd",&exit) && closes==2);
    puts("runescape prerequisites: pinned real installers, MSI readiness, sequential x86/x64, genuine runas PASS");
    return 0;
}
'''
native = (here / 'native-entry.c').read_text()
exit_function = native.split('void __attribute__((noreturn)) pw_native_exit(int status)', 1)[1].split(
    '\n#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE)\n#include "forever-download.h"', 1)[0]
exit_function = 'void __attribute__((noreturn)) pw_native_exit(int status)' + exit_function
extract_spec = importlib.util.spec_from_file_location('close_extraction', here / 'build-desktop-surface-test.py')
extract = importlib.util.module_from_spec(extract_spec)
extract_spec.loader.exec_module(extract)
close_functions = '\n\n'.join(extract.definition(here / 'native-entry.c', name) for name in (
    'pw_runescape_await_transition', 'pw_runescape_close'))
wait_function = native.split('static void b1_wait(void)\n{', 1)[1].split('\n}\n', 1)[0]
assert 'for (;;) pthread_cond_wait(&condition, &mutex);' in wait_function
supervisor = (here / 'runtime-parent.c').read_text().split(
    'void pw_supervisor_shutdown(int keep_desktop)', 1)[1].split('\n}', 1)[0]
supervisor = 'void pw_supervisor_shutdown(int keep_desktop)' + supervisor + '\n}\n'
# Compile the entire production exit boundary, replacing only native services
# and the infinite wait. A wait escapes by longjmp so libc exit is observable.
exit_harness = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef PW_RUNTIME_RUNESCAPE
#include "wine/ps5_game.h"
#define PW_GAME_PROGRAM 9
static struct { const char *argv[PW_GAME_PROGRAM + 1]; } game_wire = {
    .argv = {[PW_GAME_PROGRAM] = "C:\\Game\\osclient.exe"}
};
#endif
#define PW_RUNTIME_B1 1
#define PW_PARENT_ROLE 1
#define PW_RUNTIME_GPU 0
#define PW_RUNTIME_HEADLESS 0
#define PRODUCT_DISPLAY_NAME "RuneScape Launcher (PS5)"
#define FOREVER_PREFIX "/test/prefix"
static jmp_buf stopped;
static char message[512];
static int held, exited, log_stopped, desktop_stopped, supervisor_joins;
static int output_started, output_bad, output_ok, output_read, output_write;
static int duplicate_channel[2], game_relaunch_pending, forever_prefix_init;
static pthread_t output_thread;
static atomic_int output_done, forever_menu_exit_pending, forever_menu_relaunch_pending;
static void forever_addon_trash_shutdown(void) {}
static void forever_title_voice_shutdown(void) {}
static void pw_title_log_shutdown(void) { log_stopped++; }
static int started, stopping;
static int64_t shutdown_grace, shutdown_deadline;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t reaper;
static unsigned occupied_slots(void) { return 0; }
static int64_t io_now_ms(void) { return 0; }
static void pw_desktop_stop(void) { desktop_stopped++; }
static int test_join(pthread_t thread, void **value)
{ (void)thread; (void)value; supervisor_joins++; return 0; }
#define pthread_join test_join
static int join_thread(pthread_t thread, void **value) { (void)thread; (void)value; return 0; }
static void io_log(const char *format, ...) { (void)format; }
static void pw_duplicate_bridge_close(int *channel) { (void)channel; }
#ifdef PW_RUNTIME_RUNESCAPE
static unsigned close_delay, platform_closed, identity_queries;
static int identity_status;
static uint32_t own_app_id = 0x8018;
static int test_close_delay(unsigned delay) {
    if (delay == 100000) longjmp(stopped, 3);
    close_delay += delay;return 0;
}
#define usleep test_close_delay
int sceKernelGetAppInfo(pid_t pid, struct pw_title_app_info *info) {
    assert(pid == getpid() && (uintptr_t)info % 8 == 0 && sizeof(*info) == 96);
    identity_queries++;info->app_id = own_app_id;return identity_status;
}
int sceSystemServiceKillApp(int app, int how, int reason, int core_dump) {
    assert(identity_queries == 1 && !identity_status && app == (int)own_app_id && app > 0);
    assert(how == -1 && !reason && !core_dump);platform_closed++;longjmp(stopped, 2);
}
#endif
static int sceSystemServiceLoadExec(const char *path, const void *args)
{
    (void)path; (void)args; assert(0); return -1; /* never use replacement spawning for self-close */
}
static void forever_say(const char *text) { snprintf(message, sizeof(message), "%s", text); }
static void b1_wait(void) { held++; longjmp(stopped, 1); }
static void pw_maintenance_loadexec_game(const char *reason) { (void)reason; assert(0); }
static int forever_finish_init(const char *prefix, int status)
{ (void)prefix; (void)status; return 0; }
static int forever_permission_error(int status) { (void)status; return 0; }
static void pw_forever_permission_stop(const char *a, const char *b) { (void)a; (void)b; assert(0); }
static int forever_prefix_ready(const char *prefix) { (void)prefix; return 0; }
static __attribute__((noreturn)) void forbidden_exit(int status)
{ (void)status; exited++; longjmp(stopped, 2); }
#define exit forbidden_exit
'''
exit_cases = r'''
int main(void)
{
    /* Rejected bootstrap, missing PE/dependency, and an arbitrary Wine fault. */
    const unsigned statuses[] = {1, 0xc0000135u, 0xc0000005u, 0};
    for (unsigned i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++)
    {
        held = exited = log_stopped = desktop_stopped = supervisor_joins = 0;
        started = 1;
        message[0] = 0;
        if (!setjmp(stopped)) pw_native_exit((int)statuses[i]);
        assert(held == 1 && !exited && supervisor_joins == 1 && log_stopped == 1);
        assert(!started && !desktop_stopped); /* error remains presentable */
        assert(strstr(message, "Close RuneScape Launcher (PS5)"));
        if (statuses[i])
        {
            char hex[16];
            snprintf(hex, sizeof(hex), "0x%08x", statuses[i]);
            assert(strstr(message, hex) && strstr(message, "signature verification"));
        }
        else assert(strstr(message, "has closed"));
    }
    game_relaunch_pending = -110;held = exited = 0;started = 1;
    if (!setjmp(stopped)) pw_native_exit(0);
    assert(held == 1 && !exited && strstr(message, "could not stop all launcher helpers"));
    game_relaunch_pending = 0;
    forever_prefix_init = 1;
    held = exited = 0;
    if (!setjmp(stopped)) pw_native_exit(1);
    assert(held == 1 && !exited && strstr(message, "Setting up the Windows files did not finish"));
    started = 1;
    pw_supervisor_shutdown(0);
    assert(desktop_stopped == 1); /* other roles retain their normal teardown */
    puts("runescape maintenance Wine failure: native screen message + hold, never libc exit PASS");
    return 0;
}
'''
rune_exit_cases = r'''
int main(void)
{
    started = 1;
    if (!setjmp(stopped)) pw_native_exit(0);
    assert(platform_closed == 1 && !exited && !held && !close_delay && !message[0]);
    assert(!started && supervisor_joins == 1 && log_stopped == 1);
    started = 1;held = exited = platform_closed = identity_queries = 0;game_relaunch_pending = -110;
    if (!setjmp(stopped)) pw_native_exit(0);
    assert(platform_closed == 1 && !exited && !held && close_delay == 3000000);
    assert(strstr(message, "RuneScape could not stop the launcher helpers") && strstr(message, "0xffffff92"));
    assert(strstr(message, "Closing the title automatically"));
    started = 1;held = exited = platform_closed = identity_queries = 0;close_delay = 0;
    game_wire.argv[PW_GAME_PROGRAM] = "C:\\RuneLite\\RuneLite.exe";
    if (!setjmp(stopped)) pw_native_exit(0);
    assert(platform_closed == 1 && !exited && !held && close_delay == 3000000);
    assert(strstr(message, "stop the launcher helpers for RuneLite") && !strstr(message, "for OSRS"));
    game_wire.argv[PW_GAME_PROGRAM] = "C:\\Game\\osclient.exe";
    started = 1;held = exited = platform_closed = identity_queries = 0;close_delay = 0;game_relaunch_pending = 0;
    if (!setjmp(stopped)) pw_native_exit((int)0xc0000135u);
    assert(held == 1 && !exited && !platform_closed && !close_delay && strstr(message, "signature verification"));
    held = exited = platform_closed = identity_queries = 0;identity_status = -1;
    if (!setjmp(stopped)) pw_runescape_close("launcher closed", 0, 0);
    assert(identity_queries == 1 && !platform_closed && !exited && !held);
    identity_queries = 0;identity_status = 0;own_app_id = 0;
    if (!setjmp(stopped)) pw_runescape_close("launcher closed", 0, 0);
    assert(identity_queries == 1 && !platform_closed && !exited && !held);
    puts("runescape title closure: normal launcher exit auto-closes, handoff error code/delay/close, trust-error hold retained PASS");
    return 0;
}
'''

retry_function = 'static ULONGLONG rs_launcher_log_size(' + source.split(
    'static ULONGLONG rs_launcher_log_size(', 1)[1].split('int wmain(', 1)[0]
retry_harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "runescape-launcher-retry.h"
typedef wchar_t WCHAR;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef void *HANDLE;
typedef int BOOL;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { unsigned short wYear,wMonth,wDayOfWeek,wDay,wHour,wMinute,wSecond,wMilliseconds; } SYSTEMTIME;
#define GENERIC_READ 1
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define OPEN_EXISTING 3
#define FILE_BEGIN 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define WINAPI
#define TRUE 1
#define FALSE 0
#define WAIT_TIMEOUT 258
#define WAIT_OBJECT_0 0
#define INFINITE 0xffffffff
#define ERROR_TIMEOUT 1460
struct rs_launcher_processes { HANDLE handles[33];DWORD pids[32];unsigned count; };
static unsigned live_launchers,terminated;
static int rs_launcher_snapshot(struct rs_launcher_processes *p)
{
    p->count=live_launchers;
    for(unsigned i=0;i<live_launchers;i++) p->handles[i+1]=(HANDLE)(intptr_t)(100+i);
    return 1;
}
static void rs_launcher_release(struct rs_launcher_processes *p) { p->count=0; }
static int TerminateProcess(HANDLE h,unsigned code) { assert((intptr_t)h>=100 && code==ERROR_TIMEOUT);terminated++;live_launchers--;return 1; }
static void rs_logf(const char *format,...);
static void rs_log(const char *text) { rs_logf("%s",text); }
static char logged[16][160];static unsigned logged_count;
static void rs_logf(const char *format,...)
{
    va_list args;va_start(args,format);
    vsnprintf(logged[logged_count%16],sizeof(logged[0]),format,args);va_end(args);logged_count++;
}
/* Two in-memory files: today's launcher log and the next local day's. */
static unsigned short today=24;
static char *files[2];static size_t sizes[2];static int exists[2];
static int slot_of(const WCHAR *path) { return wcsstr(path,L".10.24.2026.")?0:wcsstr(path,L".10.25.2026.")?1:-1; }
static HANDLE CreateFileW(const WCHAR *path,DWORD a,DWORD b,void *c,DWORD d,DWORD e,void *f)
{
    assert(a==GENERIC_READ && b==7 && !c && d==OPEN_EXISTING && !e && !f);
    int slot=slot_of(path);assert(slot>=0);
    return exists[slot] ? (HANDLE)(intptr_t)(slot+1) : INVALID_HANDLE_VALUE;
}
static int GetFileSizeEx(HANDLE h,LARGE_INTEGER *size) { size->QuadPart=(LONGLONG)sizes[(intptr_t)h-1];return 1; }
static size_t cursor;
static int SetFilePointerEx(HANDLE h,LARGE_INTEGER at,LARGE_INTEGER *out,DWORD how)
{ assert(h && !out && how==FILE_BEGIN && at.QuadPart>=0);cursor=(size_t)at.QuadPart;return 1; }
static int ReadFile(HANDLE h,void *buffer,DWORD n,DWORD *got,void *overlapped)
{
    int slot=(int)(intptr_t)h-1;assert(!overlapped && cursor+n<=sizes[slot]);
    memcpy(buffer,files[slot]+cursor,n);*got=n;return 1;
}
static int CloseHandle(HANDLE h) { assert(h);return 1; }
/* Watcher: the thread body runs inside the stubbed launch, polling a scripted
 * number of ticks while the launcher is "alive". */
static DWORD (*thread_proc)(void *);static void *thread_arg;static int thread_ok,stopped_events;
static unsigned ticks_left;
static HANDLE CreateEventW(void *s,int manual,int initial,const WCHAR *name)
{ assert(!s && manual && !initial && !name);return (HANDLE)5; }
static HANDLE CreateThread(void *s,size_t stack,DWORD (*proc)(void *),void *arg,DWORD flags,DWORD *id)
{ assert(!s && !stack && !flags && !id);thread_proc=proc;thread_arg=arg;return thread_ok ? (HANDLE)6 : NULL; }
static int SetEvent(HANDLE h) { assert(h==(HANDLE)5);stopped_events++;return 1; }
static DWORD WaitForSingleObject(HANDLE h,DWORD ms)
{
    if(h==(HANDLE)6) { assert(ms==INFINITE);return WAIT_OBJECT_0; }
    assert(h==(HANDLE)5 && ms==1000);
    return ticks_left-- ? WAIT_TIMEOUT : WAIT_OBJECT_0;
}
static void GetLocalTime(SYSTEMTIME *t) { memset(t,0,sizeof(*t));t->wYear=2026;t->wMonth=10;t->wDay=today; }
static unsigned sleeps;static DWORD slept;
static void Sleep(DWORD ms) { sleeps++;slept+=ms; }
static void append(int slot,const char *text)
{
    size_t n=strlen(text);files[slot]=realloc(files[slot],sizes[slot]+n);
    memcpy(files[slot]+sizes[slot],text,n);sizes[slot]+=n;exists[slot]=1;
}
/* Scripted launcher: each start appends its log lines, then exits. */
static const char *script[4][2];static unsigned starts;static int admin_ok;
/* Per start: appended text becomes visible after `alive_ticks[n]` watcher
 * ticks, with the launcher process alive the whole time (error dialog up). */
static const char *alive_text[4];static unsigned alive_ticks[4];
static int rs_launch_admin(const WCHAR *path,const WCHAR *arguments,const WCHAR *cwd,DWORD *exit)
{
    assert(!wcscmp(path,L"launcher") && !wcscmp(arguments,L"--arg") && !wcscmp(cwd,L"cwd"));
    unsigned n=starts++;
    live_launchers=2;ticks_left=alive_ticks[n]+1;
    if(thread_proc && alive_text[n]) {
        /* Quiet ticks first, then the launcher writes its error line. */
        unsigned quiet=alive_ticks[n];
        ticks_left=quiet;thread_proc(thread_arg);
        append(0,alive_text[n]);
        ticks_left=3;thread_proc(thread_arg);
        if(!live_launchers) { *exit=1;return admin_ok; } /* killed: the process tree is gone */
    }
    if(script[n][0]) append(0,script[n][0]);
    if(script[n][1]) { today=25;append(1,script[n][1]); }
    *exit=7+n;return admin_ok;
}
"""
retry_cases = r"""
static const char *started="2026-10-24T08:40:44.000+08 INFO - Process Id: 44\r\n";
static const char *timeout="2026-10-24T08:41:29.000+08 ERROR - Timeout waiting for window to load\r\n";
static const char *console="2026-10-24T08:41:29.000+08 CONSOLE - ERROR - Timeout waiting for window to load\r\n";
static void reset(void)
{
    for(unsigned i=0;i<2;i++) { free(files[i]);files[i]=NULL;sizes[i]=0;exists[i]=0; }
    memset(script,0,sizeof(script));memset(alive_text,0,sizeof(alive_text));memset(alive_ticks,0,sizeof(alive_ticks));
    thread_proc=NULL;thread_ok=1;terminated=stopped_events=0;live_launchers=0;starts=sleeps=0;slept=0;logged_count=0;today=24;admin_ok=1;
}
int main(void)
{
    DWORD exit=0;
    /* Healthy start: no timeout, no restart. */
    reset();script[0][0]=started;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==1 && exit==7 && !logged_count);
    /* No log file at all (launcher never wrote one): single start. */
    reset();assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==1 && !logged_count);
    /* Cold-start timeout then a good warm start: exactly one restart, one marker. */
    reset();script[0][0]=timeout;script[1][0]=started;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==2 && exit==8);
    assert(logged_count==1 && !strcmp(logged[0],"launcher window timeout; restarting attempt=1"));
    /* A timeout from an earlier session already in the file is not a new timeout. */
    reset();append(0,timeout);script[0][0]=started;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==1 && !logged_count);
    /* A page console line quoting the text does not restart. */
    reset();script[0][0]=console;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==1 && !logged_count);
    /* Persistent timeout is bounded: first start + RS_LAUNCHER_RETRY_MAX restarts. */
    reset();script[0][0]=timeout;script[1][0]=timeout;script[2][0]=timeout;script[3][0]=timeout;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==3 && exit==9);
    assert(logged_count==3 && !strcmp(logged[0],"launcher window timeout; restarting attempt=1") &&
           !strcmp(logged[1],"launcher window timeout; restarting attempt=2") &&
           !strcmp(logged[2],"launcher window timeout; retries exhausted attempts=3"));
    /* Timeout then user closes normally on the retry. */
    reset();script[0][0]=timeout;script[1][0]=started;script[2][0]=timeout;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==2);
    /* Local midnight mid-attempt: the timeout lands in the next day's log. */
    reset();append(0,started);script[0][1]=timeout;script[1][0]=NULL;
    today=24;assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==2 && logged_count==1);
    /* Error dialog keeps the launcher alive: the line is seen by the watcher,
     * the process tree is terminated, and the same retry bound restarts it. */
    reset();alive_text[0]=timeout;alive_ticks[0]=2;script[1][0]=started;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==2 && terminated==2);
    assert(logged_count==2 && strstr(logged[0],"launcher still running, terminated processes=2") &&
           !strcmp(logged[1],"launcher window timeout; restarting attempt=1") && stopped_events==2);
    /* Timeout appears while alive on every start: killed each time, bounded. */
    reset();for(unsigned i=0;i<4;i++) { alive_text[i]=timeout;alive_ticks[i]=1; }
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==3 && terminated==6);
    assert(!strcmp(logged[logged_count-1],"launcher window timeout; retries exhausted attempts=3"));
    /* A healthy running launcher (no timeout line) is never killed or restarted. */
    reset();alive_text[0]=started;alive_ticks[0]=3;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==1 && !terminated && !logged_count);
    /* A quoted console line while alive is not a timeout. */
    reset();alive_text[0]=console;alive_ticks[0]=1;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==1 && !terminated && !logged_count);
    /* Text from an earlier session in the same file does not kill a new launcher. */
    reset();append(0,timeout);alive_text[0]=started;alive_ticks[0]=2;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==1 && !terminated);
    /* No watcher thread available: exit path still recovers. */
    reset();thread_ok=0;script[0][0]=timeout;script[1][0]=started;
    assert(rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==2 && !terminated);
    /* A failed elevated start is a failure, not a retry. */
    reset();admin_ok=0;script[0][0]=timeout;
    assert(!rs_launch_launcher(L"launcher",L"--arg",L"cwd",&exit) && starts==1 && !logged_count);
    puts("runescape launcher window-timeout recovery: bounded restart from the launcher log PASS");
    return 0;
}
"""
compiler = Path(os.environ.get('LLVM_MINGW', '/opt/llvm-mingw')) / 'bin/x86_64-w64-mingw32-clang'
# Keep all compiler outputs in this development checkout, including on failure.
with tempfile.TemporaryDirectory(prefix='.runescape-bootstrap-test-', dir=here) as folder:
    work = Path(folder)
    (work / 'runescape-product.h').write_text(header)
    logger = work / 'test-bootstrap-log'
    (work / 'test-bootstrap-log.c').write_text(logger_harness + logger_function + logger_cases)
    subprocess.run([*shlex.split(os.environ.get('HOST_CC', 'cc')), '-std=gnu11',
                    '-Wall', '-Wextra', '-Werror', str(work / 'test-bootstrap-log.c'),
                    '-o', str(logger)], check=True)
    subprocess.run([str(logger)], check=True)
    fallback = subprocess.check_output([str(logger), 'windows'], text=True)
    assert fallback == 'runescape: check=42 value=ready\nrunescape: final\n'
    launch = work / 'test-prerequisite-launch'
    (work / 'test-prerequisite-launch.c').write_text(launch_harness + launch_function + launch_cases)
    subprocess.run([*shlex.split(os.environ.get('HOST_CC', 'cc')), '-std=gnu11',
                    '-Wall', '-Wextra', '-Werror', str(work / 'test-prerequisite-launch.c'),
                    '-o', str(launch)], check=True)
    subprocess.run([str(launch)], check=True)
    (work / 'test-maintenance-exit.c').write_text(exit_harness + supervisor + exit_function + exit_cases)
    host_output = work / 'test-maintenance-exit'
    subprocess.run([*shlex.split(os.environ.get('HOST_CC', 'cc')), '-std=gnu11', '-pthread',
                    str(work / 'test-maintenance-exit.c'), '-o', str(host_output)], check=True)
    subprocess.run([str(host_output)], check=True)
    rune_source = work / 'test-runescape-autoclose.c'
    rune_source.write_text('#define PW_RUNTIME_RUNESCAPE 1\n#include "runtime-app-close.h"\n' + exit_harness +
                           close_functions + supervisor + exit_function + rune_exit_cases)
    rune_output = work / 'test-runescape-autoclose'
    subprocess.run([*shlex.split(os.environ.get('HOST_CC', 'cc')), '-std=gnu11', '-pthread', '-I', str(here),
                    '-I', str(here.parents[3] / 'ps5-wine' / 'include'),
                    str(rune_source), '-o', str(rune_output)], check=True)
    subprocess.run([str(rune_output)], check=True)
    prerequisites = work / 'test-prerequisites'
    (work / 'test-prerequisites.c').write_text(
        prerequisite_harness + prerequisite_functions + prerequisite_cases)
    subprocess.run([*shlex.split(os.environ.get('HOST_CC', 'cc')), '-std=gnu11',
                    '-Wall', '-Wextra', '-Werror', '-I', str(here),
                    str(work / 'test-prerequisites.c'), '-o', str(prerequisites)], check=True)
    subprocess.run([str(prerequisites)], check=True)
    retry = work / 'test-launcher-retry'
    (work / 'test-launcher-retry.c').write_text(
        '#include <stdarg.h>\n' + retry_harness + retry_function + retry_cases)
    subprocess.run([*shlex.split(os.environ.get('HOST_CC', 'cc')), '-std=gnu11',
                    '-Wall', '-Wextra', '-Werror', '-I', str(here),
                    str(work / 'test-launcher-retry.c'), '-o', str(retry)], check=True)
    subprocess.run([str(retry)], check=True)
    output = work / 'runescape-bootstrap.exe'
    subprocess.run([str(compiler), '-std=c11', '-Os', '-s', '-static', '-municode',
                    '-Wall', '-Wextra', '-Werror', '-Wl,--no-insert-timestamp',
                    '-I', str(work), str(here / 'runescape-bootstrap.c'), '-o', str(output),
                    '-lwintrust', '-lcrypt32', '-ladvapi32', '-lversion', '-luser32',
                    '-lshell32', '-lmsi'], check=True)
    assert output.read_bytes()[:2] == b'MZ'
print('runescape managed PE build contract passed (not trust qualification)')
