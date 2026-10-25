#!/usr/bin/env python3
"""Exercise production handoff boundaries with Win32/NT services isolated."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = spec_from_file_location('source_functions', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
args.out.mkdir(parents=True, exist_ok=True)

def run(name, declarations, functions, cases, includes=()):
    source = args.out / (name + '.c')
    binary = args.out / name
    source.write_text(declarations + '\n' + functions + '\n' + cases)
    subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra',
        '-Werror', '-Wno-unused-function', *includes, str(source), '-o', str(binary), '-pthread'], check=True)
    subprocess.run([str(binary)], check=True)

bootstrap = (here / 'runescape-bootstrap.c').read_text()
launcher_functions = bootstrap[bootstrap.index('struct rs_launcher_processes {'):bootstrap.index('/* Genuine Windows elevation')]
run('test-runescape-resident-launcher', r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef void *HANDLE;
typedef uintptr_t HWND;
typedef intptr_t LPARAM;
typedef unsigned long DWORD;
typedef uintptr_t DWORD_PTR;
typedef unsigned UINT;
typedef uint64_t ULONGLONG;
typedef int BOOL;
typedef wchar_t WCHAR;
typedef struct { DWORD dwSize,th32ProcessID;WCHAR szExeFile[260]; } PROCESSENTRY32W;
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xffffffffu
#define INFINITE 0xffffffffu
#define WM_CLOSE 0x10
#define WM_QUERYENDSESSION 0x11
#define WM_ENDSESSION 0x16
#define ENDSESSION_CLOSEAPP 1
#define SMTO_ABORTIFHUNG 2
#define SMTO_BLOCK 1
#define ERROR_TIMEOUT 1460
#define ERROR_NO_MORE_FILES 18
#define ERROR_INVALID_PARAMETER 87
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define TH32CS_SNAPPROCESS 2
#define SYNCHRONIZE 0x100000
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_TERMINATE 1
static int alive,original_alive,handoff,mode,closes,queries,end_sessions,terminated,unknown_flush,veto;
static int snapshot_index,relay,any_waits,terminate_ok;
static DWORD last_error;
static ULONGLONG clock_ms;
static void rs_log(const char *line) { if(strstr(line,"forced=1 cefcache_flushed=unknown")) unknown_flush++; }
static void rs_logf(const char *format,...) { assert(format); }
static ULONGLONG GetTickCount64(void) { return clock_ms; }
static DWORD GetLastError(void) { return last_error; }
static int lstrcmpiW(const WCHAR *left,const WCHAR *right) { return wcscasecmp(left,right); }
static HANDLE CreateToolhelp32Snapshot(DWORD flags,DWORD pid) { assert(flags==2 && !pid);snapshot_index=0;return (HANDLE)3; }
static BOOL fill_entry(PROCESSENTRY32W *entry) {
    assert(entry->dwSize==sizeof(*entry));
    if(snapshot_index==0) { entry->th32ProcessID=99;wcscpy(entry->szExeFile,L"explorer.exe");return TRUE; }
    if(alive && snapshot_index<=2) {
        entry->th32ProcessID=snapshot_index==1 ? 42 : 43;
        wcscpy(entry->szExeFile,snapshot_index==1 ? L"JagexLauncher.exe" : L"JAGEXLAUNCHER.EXE");
        return TRUE;
    }
    last_error=ERROR_NO_MORE_FILES;return FALSE;
}
static BOOL Process32FirstW(HANDLE snapshot,PROCESSENTRY32W *entry) { assert(snapshot==(HANDLE)3);return fill_entry(entry); }
static BOOL Process32NextW(HANDLE snapshot,PROCESSENTRY32W *entry) { assert(snapshot==(HANDLE)3);snapshot_index++;return fill_entry(entry); }
static HANDLE OpenProcess(DWORD access,BOOL inherit,DWORD pid) {
    assert((access&SYNCHRONIZE) && !inherit && (pid==42 || pid==43));return (HANDLE)(uintptr_t)pid;
}
static BOOL CloseHandle(HANDLE handle) { assert(handle==(HANDLE)3 || handle==(HANDLE)42 || handle==(HANDLE)43);return TRUE; }
static DWORD GetWindowThreadProcessId(HWND window,DWORD *pid) {
    *pid=window==102 && alive ? 42 : window==103 && alive ? 43 : 99;return 1;
}
static BOOL PostMessageW(HWND window,UINT message,uintptr_t wparam,LPARAM lparam) {
    assert((window==102 || window==103) && message==WM_CLOSE && !wparam && !lparam);closes++;
    if(mode==0) alive=0;return TRUE;
}
static uintptr_t SendMessageTimeoutW(HWND window,UINT message,uintptr_t wparam,LPARAM lparam,
                                    UINT flags,UINT timeout,DWORD_PTR *answer) {
    assert((window==102 || window==103) && lparam==ENDSESSION_CLOSEAPP && flags==3 && timeout>0 && timeout<=1000);
    if(message==WM_QUERYENDSESSION) { assert(!wparam);queries++;*answer=!veto;return 1; }
    assert(message==WM_ENDSESSION && wparam==TRUE);end_sessions++;if(mode==1) alive=0;return 1;
}
static BOOL EnumWindows(BOOL (*callback)(HWND,LPARAM),LPARAM parameter) {
    if(!callback(101,parameter)) return FALSE;
    if(!callback(102,parameter)) return FALSE;
    return callback(103,parameter);
}
static DWORD WaitForMultipleObjects(DWORD count,const HANDLE *handles,BOOL all,DWORD timeout) {
    if(all) {
        assert(count==2 && handles[0]==(HANDLE)42 && handles[1]==(HANDLE)43 && timeout>0 && timeout<=5000);
        if(alive) { clock_ms+=timeout;return WAIT_TIMEOUT; }return WAIT_OBJECT_0;
    }
    any_waits++;assert(handles[0]==(HANDLE)2 && timeout==INFINITE);
    if(relay) {
        assert(count==2 && handles[1]==(HANDLE)1 && original_alive && !alive);
        original_alive=0;alive=handoff=1;relay=0;return WAIT_OBJECT_0+1;
    }
    assert(count==3 && handles[1]==(HANDLE)42 && handles[2]==(HANDLE)43 && alive && handoff);
    return WAIT_OBJECT_0; /* resident launchers do not exit on their own */
}
static DWORD WaitForSingleObject(HANDLE process,DWORD timeout) {
    assert(process==(HANDLE)1 && !timeout);return original_alive ? WAIT_TIMEOUT : WAIT_OBJECT_0;
}
static BOOL GetExitCodeProcess(HANDLE process,DWORD *exit) { assert(process==(HANDLE)1 && !original_alive);*exit=42;return TRUE; }
static BOOL TerminateProcess(HANDLE process,DWORD status) {
    assert((process==(HANDLE)42 || process==(HANDLE)43) && status==ERROR_TIMEOUT);terminated++;
    if(terminate_ok) alive=0;return terminate_ok;
}
static void reset(int shutdown_mode) {
    alive=handoff=terminate_ok=1;original_alive=relay=any_waits=0;mode=shutdown_mode;
    closes=queries=end_sessions=terminated=unknown_flush=veto=0;clock_ms=0;
}
''', launcher_functions, r'''
int main(void) {
    DWORD exit=99;
    reset(0);assert(alive && !original_alive && rs_wait_launcher((HANDLE)1,(HANDLE)2,&exit));
    assert(exit==0 && !alive && closes==1 && !queries && !terminated);
    reset(0);alive=handoff=0;original_alive=relay=1;
    assert(rs_wait_launcher((HANDLE)1,(HANDLE)2,&exit) && any_waits==2);
    assert(exit==0 && !alive && !original_alive && closes==1 && !terminated);
    reset(1);assert(rs_wait_launcher((HANDLE)1,(HANDLE)2,&exit));
    assert(exit==0 && !alive && closes==2 && queries==1 && end_sessions==1 && !terminated);
    reset(2);assert(rs_wait_launcher((HANDLE)1,(HANDLE)2,&exit));
    assert(exit==0 && terminated==2 && unknown_flush==1 && clock_ms<=20000);
    reset(2);veto=1;assert(rs_wait_launcher((HANDLE)1,(HANDLE)2,&exit));
    assert(queries==2 && !end_sessions && terminated==2 && unknown_flush==1 && clock_ms<=20000);
    reset(2);terminate_ok=0;assert(rs_wait_launcher((HANDLE)1,(HANDLE)2,&exit));
    assert(exit==0 && alive && terminated==2 && unknown_flush==1 && clock_ms<=20000); /* native drain owns force fallback */
    reset(0);alive=handoff=0;assert(rs_wait_launcher((HANDLE)1,(HANDLE)2,&exit));
    assert(exit==42 && !closes && !queries && !terminated);
    puts("runescape resident launcher: original-root relay, Play event, WM_CLOSE/end-session, bounded force/unknown flush, normal exit PASS");
    return 0;
}
''')

ack_function = extract.definition(args.wine / 'dlls/ntdll/unix/process.c', 'ps5_create_game_acknowledgement')
run('test-runescape-createprocess-ack', r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WINE_PS5_TITLE_HOST_TEST
#include <wine/ps5_title.h>
typedef void *HANDLE;
typedef uint16_t WCHAR;
typedef unsigned ACCESS_MASK;
typedef unsigned ULONG;
typedef int32_t NTSTATUS;
typedef size_t SIZE_T;
typedef struct { WCHAR *Buffer; unsigned Length, MaximumLength; } UNICODE_STRING;
typedef struct { UNICODE_STRING *ObjectName; unsigned Attributes; HANDLE RootDirectory; } OBJECT_ATTRIBUTES;
typedef struct { UNICODE_STRING ImagePathName,CommandLine; struct { UNICODE_STRING DosPath; } CurrentDirectory;
                 void *Environment;HANDLE ConsoleHandle;unsigned ConsoleFlags,Flags; } RTL_USER_PROCESS_PARAMETERS;
typedef struct { int state; } PS_CREATE_INFO;
typedef struct { uintptr_t Attribute;size_t Size;void *ValuePtr; } PS_ATTRIBUTE;
typedef struct { size_t TotalLength;PS_ATTRIBUTE Attributes[]; } PS_ATTRIBUTE_LIST;
#define FALSE 0
#define OBJ_CASE_INSENSITIVE 0x40
#define EVENT_MODIFY_STATE 2
#define PS_ATTRIBUTE_IMAGE_NAME 0x20005
#define STATUS_NO_MEMORY ((NTSTATUS)0xc0000017)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001)
static int open_ok, spawn_ok, stage_ok, staged, spawned, signalled, terminated, closed;
static void *original_environment;
static WCHAR *original_cwd;
static struct { struct { unsigned SessionId; } *Peb; } teb;
static struct { unsigned SessionId; } peb = {3};
#define NtCurrentTeb() (&teb)
static void ascii_to_unicode(WCHAR *dest,const char *src,size_t length) { while(length--) *dest++=(unsigned char)*src++; }
static int equal(const WCHAR *wide,const char *text) { while(*wide && *text && *wide==(unsigned char)*text) { wide++;text++; }return !*wide && !*text; }
static void init_unicode_string(UNICODE_STRING *string,const WCHAR *value) {
    size_t size=0;while(value[size]) size++;string->Buffer=(WCHAR *)value;string->Length=size*2;string->MaximumLength=(size+1)*2;
}
#define InitializeObjectAttributes(a,n,f,r,s) do { (a)->ObjectName=(n);(a)->Attributes=(f);(a)->RootDirectory=(r); } while(0)
static NTSTATUS NtOpenEvent(HANDLE *event,ACCESS_MASK access,const OBJECT_ATTRIBUTES *attributes) {
    assert(access==EVENT_MODIFY_STATE && attributes->Attributes==OBJ_CASE_INSENSITIVE && !attributes->RootDirectory);
    assert(equal(attributes->ObjectName->Buffer,"\\Sessions\\3\\BaseNamedObjects\\RuneScapeOsrsHandoff"));
    if(!open_ok) return STATUS_UNSUCCESSFUL;*event=(HANDLE)3;return 0;
}
static NTSTATUS NtClose(HANDLE handle) { assert(handle);closed++;return 0; }
static NTSTATUS NtTerminateProcess(HANDLE process,NTSTATUS status) { assert(process==(HANDLE)1 && status);terminated++;return 0; }
static NTSTATUS NtSetEvent(HANDLE event,void *previous) { assert(event==(HANDLE)3 && !previous && staged==1);signalled++;return 0; }
static int stage(const struct wine_ps5_title_game_launch *launch) {
    assert(spawned==1 && !signalled && launch->environment==original_environment);staged++;return stage_ok ? 0 : 5;
}
static const struct wine_ps5_title_runtime runtime={.game_launch=stage};
static const struct wine_ps5_title_runtime *ps5_title_get_runtime(void) { return &runtime; }
static NTSTATUS create_user_process(HANDLE *process,HANDLE *thread,ACCESS_MASK pa,ACCESS_MASK ta,
    OBJECT_ATTRIBUTES *po,OBJECT_ATTRIBUTES *to,ULONG pf,ULONG tf,RTL_USER_PROCESS_PARAMETERS *params,
    PS_CREATE_INFO *info,PS_ATTRIBUTE_LIST *attributes,int route) {
    assert(pa==11 && ta==12 && !po && !to && pf==13 && tf==14 && !route && info);
    assert(equal(params->ImagePathName.Buffer,"C:\\windows\\system32\\cmd.exe"));
    assert(equal(params->CommandLine.Buffer,"cmd.exe /c exit 0"));
    assert(params->Environment==original_environment && params->CurrentDirectory.DosPath.Buffer==original_cwd);
    assert(!params->ConsoleHandle && !params->ConsoleFlags); /* no extra conhost child */
    assert(attributes->TotalLength==sizeof(size_t)+2*sizeof(PS_ATTRIBUTE));
    assert(attributes->Attributes[0].Attribute==9 && attributes->Attributes[1].Attribute==PS_ATTRIBUTE_IMAGE_NAME);
    assert(equal(attributes->Attributes[1].ValuePtr,"\\??\\C:\\windows\\system32\\cmd.exe"));
    spawned++;if(!spawn_ok) return STATUS_UNSUCCESSFUL;*process=(HANDLE)1;*thread=(HANDLE)2;return 0;
}
static void reset(void) { open_ok=spawn_ok=stage_ok=1;staged=spawned=signalled=terminated=closed=0;teb.Peb=(void *)&peb; }
''', ack_function, r'''
int main(void) {
    WCHAR image[64],command[64],cwd[64];uint16_t environment[]={'J','X','_','S','=','x',0,0};
    ascii_to_unicode(image,"C:\\Game\\osclient.exe",21);ascii_to_unicode(command,"original arguments",19);ascii_to_unicode(cwd,"C:\\Game",8);
    RTL_USER_PROCESS_PARAMETERS params={0};init_unicode_string(&params.ImagePathName,image);
    init_unicode_string(&params.CommandLine,command);init_unicode_string(&params.CurrentDirectory.DosPath,cwd);
    params.Environment=original_environment=environment;original_cwd=cwd;
    params.ConsoleHandle=(HANDLE)7;params.ConsoleFlags=2;
    struct { size_t TotalLength;PS_ATTRIBUTE Attributes[2]; } attributes={sizeof(size_t)+2*sizeof(PS_ATTRIBUTE),{{9,0,0},{PS_ATTRIBUTE_IMAGE_NAME,0,image}}};
    PS_CREATE_INFO info={0};HANDLE process=0,thread=0;
    struct wine_ps5_title_game_launch launch={.size=sizeof(launch),.environment=environment};
    reset();assert(!ps5_create_game_acknowledgement(&process,&thread,11,12,0,0,13,14,&params,&info,(void *)&attributes,&launch));
    assert(process==(HANDLE)1 && thread==(HANDLE)2 && staged==1 && signalled==1 && !terminated && closed==1);
    assert(params.ImagePathName.Buffer==image && params.CommandLine.Buffer==command && attributes.Attributes[1].ValuePtr==image);
    assert(params.ConsoleHandle==(HANDLE)7 && params.ConsoleFlags==2);
    const char runelite_image[]="C:\\RuneLite\\RuneLite.exe";
    ascii_to_unicode(image,runelite_image,sizeof(runelite_image));init_unicode_string(&params.ImagePathName,image);
    reset();assert(!ps5_create_game_acknowledgement(&process,&thread,11,12,0,0,13,14,&params,&info,(void *)&attributes,&launch));
    assert(process==(HANDLE)1 && thread==(HANDLE)2 && staged==1 && signalled==1 && !terminated && closed==1);
    assert(params.ImagePathName.Buffer==image && params.Environment==original_environment && params.CurrentDirectory.DosPath.Buffer==original_cwd);
    ascii_to_unicode(image,"C:\\Game\\osclient.exe",21);init_unicode_string(&params.ImagePathName,image);
    reset();open_ok=0;assert(ps5_create_game_acknowledgement(&process,&thread,11,12,0,0,13,14,&params,&info,(void *)&attributes,&launch));
    assert(!spawned && !staged && !signalled);
    reset();spawn_ok=0;assert(ps5_create_game_acknowledgement(&process,&thread,11,12,0,0,13,14,&params,&info,(void *)&attributes,&launch));
    assert(spawned==1 && !staged && !signalled && closed==1);
    reset();stage_ok=0;assert(ps5_create_game_acknowledgement(&process,&thread,11,12,0,0,13,14,&params,&info,(void *)&attributes,&launch));
    assert(staged==1 && !signalled && terminated==1 && closed==3 && !process && !thread);
    puts("runescape CreateProcess acknowledgement: real success/handles, immutable original argv/env/cwd, stage-before-event, failures PASS");
    return 0;
}
''', ('-I' + str(args.wine / 'include'),))

waiter = extract.definition(here / 'runtime-parent.c', 'pw_supervisor_wait_children')
request = extract.definition(here / 'native-entry.c', 'pw_parent_request_game_launch')
run('test-runescape-handoff-drain', r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#define PW_RUNTIME_RUNESCAPE 1
#define PW_RUNTIME_GPU 0
#define PW_RUNTIME_HEADLESS 0
#include "runtime-relaunch.h"
#include <stdarg.h>
#include <string.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t children_changed=PTHREAD_COND_INITIALIZER;
static unsigned children;
static int pending,quiesced,menu_waited,quiesce_error,quiesce_result,rejected;
static struct pw_game_wire game_wire={.argc=10,.argv={[PW_GAME_PROGRAM]="C:\\Game\\osclient.exe"}};
static char last_line[256];
void io_log(const char *format,...) {
    va_list arguments;va_start(arguments,format);
    assert(vsnprintf(last_line,sizeof(last_line),format,arguments)>0);va_end(arguments);
}
static unsigned occupied_slots(void) { return children; }
static int pw_parent_game_launch_pending(void) { return pending; }
static int pw_supervisor_quiesce_children(void) {
    assert(pending);quiesced++;if(!quiesce_result) children=0;return quiesce_result;
}
static void pw_parent_game_launch_failed(int error) { rejected=error; }
static void forever_title_menu_wait_lifetime(void) { assert(!children);menu_waited++; }
''', request + '\n' + waiter, r'''
int main(void) {
    alarm(2);pending=1;children=3;
    pw_parent_request_game_launch();assert(children==3 && !quiesced);
    assert(strstr(last_line,"image=osclient.exe"));
    game_wire.argv[PW_GAME_PROGRAM]="C:\\RuneLite\\RuneLite.exe";
    pw_parent_request_game_launch();assert(children==3 && !quiesced && strstr(last_line,"image=RuneLite.exe"));
    pw_supervisor_wait_children();assert(!children && quiesced==1 && menu_waited==1);
    pending=0;pw_supervisor_wait_children();assert(quiesced==1 && menu_waited==2);
    pending=1;children=3;quiesce_result=-110;pw_supervisor_wait_children();
    assert(children==3 && rejected==-110 && quiesced==2 && menu_waited==2);
    alarm(0);puts("runescape handoff drain: capture does not close live callbacks; bootstrap exit drains residual children before loader shutdown PASS");
    return 0;
}
''', ('-I' + str(here), '-I' + str(args.wine / 'include')))
