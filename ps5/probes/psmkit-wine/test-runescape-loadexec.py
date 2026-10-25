#!/usr/bin/env python3
"""Actual private-file producer/consumer and close UX; isolate only SDK calls."""
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
spec = spec_from_file_location('loadexec_functions', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
functions = '\n\n'.join(extract.definition(here / 'native-entry.c', name) for name in (
    'pw_runescape_await_transition', 'pw_runescape_close', 'pw_game_handoff_log',
    'pw_maintenance_loadexec_game', 'pw_game_startup_argv'))
assert '/download0' not in (here / 'runtime-game-file.h').read_text()
harness = r'''
#define _GNU_SOURCE
#define WINE_PS5_TITLE_HOST_TEST
#include <wine/ps5_title.h>
#include "runtime-game-file.h"
#include "runtime-game-profile.h"
#include "runtime-app-close.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#define PW_RUNTIME_PREFIX "/prefix"
#define PW_RUNTIME_LOADER "/app0/wine-title/lib/wine/x86_64-unix/wine"
static char private_directory[4096];
static const char production_directory[] = PW_GAME_HANDOFF_DIRECTORY;
#undef PW_GAME_HANDOFF_DIRECTORY
#define PW_GAME_HANDOFF_DIRECTORY private_directory
static char runelite_config_path[4096];
static const char production_config_path[] = PW_RUNELITE_CONFIG_PATH;
#undef PW_RUNELITE_CONFIG_PATH
#define PW_RUNELITE_CONFIG_PATH runelite_config_path
static struct pw_game_wire game_wire;
static uint16_t game_environment[PW_GAME_ENV_BYTES / 2];
static struct pw_game_request game_file_request;
static char *game_file_argv[PW_GAME_WIRE_ARGV + 1];
static struct pw_game_parent_options game_parent_options;
static struct pw_game_parent_storage game_parent_storage;
static struct wine_ps5_title_runtime native_runtime;
static char message[512], logs[8192], token_seen[33];
static jmp_buf stopped;
static int platform_status, game_requests, exit_requests;
static int query_status, query_calls, kill_status;
static uint32_t own_app_id = 0x8018;
static unsigned delayed_us;
static void forever_say(const char *text) { snprintf(message,sizeof(message),"%s",text); }
void io_log(const char *format,...) {
    size_t used=strlen(logs);va_list arguments;va_start(arguments,format);
    int bytes=vsnprintf(logs+used,sizeof(logs)-used,format,arguments);va_end(arguments);
    assert(bytes>=0 && (size_t)bytes+used+2<sizeof(logs));strcat(logs,"\n");
}
static void test_random(void *buffer,size_t bytes) {
    static unsigned generation;unsigned char *out=buffer;
    for(size_t i=0;i<bytes;i++) out[i]=(unsigned char)(generation+i);generation++;
}
#define arc4random_buf test_random
static int test_delay(unsigned delay) {
    if(delay==100000) longjmp(stopped,exit_requests ? 1 : query_calls ? 3 : 2);
    delayed_us+=delay;return 0;
}
#define usleep test_delay
int sceKernelGetAppInfo(pid_t pid,struct pw_title_app_info *info) {
    assert(pid==getpid() && (uintptr_t)info%8==0 && sizeof(*info)==96);query_calls++;
    info->app_id=own_app_id;return query_status;
}
int sceSystemServiceKillApp(int app,int how,int reason,int core) {
    assert(query_calls==1 && !query_status && app==(int)own_app_id && app>0);
    assert(how==-1 && !reason && !core);exit_requests++;return kill_status;
}
static int sceSystemServiceLoadExec(const char *path,const char *const arguments[]) {
    assert(strcmp(path,"exit")); /* disqualified replacement-spawn route must never be used */
    assert(!strcmp(path,"/app0/game.bin"));game_requests++;
    assert(arguments && arguments[0] && arguments[1] && !arguments[2]);
    assert(!strcmp(arguments[0],PW_GAME_HANDOFF_FLAG) && pw_game_handoff_token_valid(arguments[1]));
    /* This is our compact transport contract, not a fabricated SDK limit. */
    assert(strlen(arguments[0])+strlen(arguments[1])+2==52);
    assert(strlen(arguments[1])+1==33);
    strcpy(token_seen,arguments[1]);
    char file[4096];struct stat status;
    assert(!pw_game_handoff_path(private_directory,token_seen,file));
    assert(!stat(file,&status) && (status.st_mode&07777)==0600);
    struct pw_game_argv_metrics metrics=pw_game_wire_metrics(&game_wire);
    assert(metrics.total_bytes>4096 && metrics.max_bytes>4096);
    assert(status.st_size==(off_t)(sizeof(struct pw_game_handoff_header)+metrics.total_bytes));
    return platform_status;
}
static void reset(void) {
    message[0]=logs[0]=token_seen[0]=0;game_requests=exit_requests=query_calls=0;delayed_us=0;
    query_status=kill_status=0;own_app_id=0x8018;
}
'''
cases = r'''
int main(void) {
    char root[]="/tmp/runescape-loadexec-XXXXXX", file[4096];
    assert(!strcmp(production_directory,"/data/runescape/handoff"));
    assert(mkdtemp(root));assert(snprintf(private_directory,sizeof(private_directory),"%s/private",root)<(int)sizeof(private_directory));
    assert(!strcmp(production_config_path,"/data/runescape/runelite.properties"));
    assert(snprintf(runelite_config_path,sizeof(runelite_config_path),"%s/runelite.properties",root)<(int)sizeof(runelite_config_path));
    unsigned removed;assert(!pw_game_handoff_prepare(private_directory,&removed,NULL));
    uint16_t original[4096];const char session[]="JX_SESSION_ID=session-private";
    for(unsigned i=0;i<4094;i++) original[i]='x';
    for(unsigned i=0;i<sizeof(session)-1;i++) original[i]=(unsigned char)session[i];
    original[4094]=original[4095]=0;
    char *program[]={"C:\\Game\\osclient.exe","","future argument",NULL};
    assert(!pw_game_wire_encode(&game_wire,program,"C:\\Game","/prefix/drive_c/Game",original,sizeof(original)));
    reset();platform_status=(int)0x80a10003u;
    int action=setjmp(stopped);
    if(!action) pw_maintenance_loadexec_game("shutdown complete");
    assert(action==1 && game_requests==1 && exit_requests==1 && delayed_us==3000000);
    assert(strstr(message,"RuneScape could not start Old School RuneScape") && strstr(message,"0x80a10003"));
    assert(strstr(message,"Closing the title automatically") && !strstr(message,"The Jagex Launcher has closed"));
    assert(!pw_game_handoff_path(private_directory,token_seen,file));
    assert(access(file,F_OK)==-1 && errno==ENOENT);
    assert(strstr(logs,"handoff_cleanup=0") && !strstr(logs,"session-private") && !strstr(logs,token_seen));

    reset();platform_status=0;
    action=setjmp(stopped);if(!action) pw_maintenance_loadexec_game("shutdown complete");
    assert(action==2 && game_requests==1 && !exit_requests && !delayed_us);
    assert(!pw_game_handoff_path(private_directory,token_seen,file) && !access(file,F_OK));
    char *arguments[]={(char *)PW_GAME_HANDOFF_FLAG,token_seen,NULL},**wine_arguments=NULL;
    const char *cwd=NULL;
    assert(pw_game_startup_argv(2,arguments,&wine_arguments,&cwd)==4);
    assert(!strcmp(wine_arguments[0],PW_RUNTIME_LOADER));
    for(unsigned i=0;program[i];i++) assert(!strcmp(wine_arguments[i+1],program[i]));
    assert(!strcmp(cwd,"/prefix/drive_c/Game") && !strcmp(native_runtime.parent_windows_cwd,"C:\\Game"));
    assert(native_runtime.parent_environment_bytes==sizeof(original) && !memcmp(native_runtime.parent_environment,original,sizeof(original)));
    assert(game_parent_options.profile->kind==WINE_PS5_GAME_OSRS && !game_parent_options.launch_mode);
    assert(access(file,F_OK)==-1 && errno==ENOENT);
    assert(pw_game_startup_argv(2,arguments,&wine_arguments,&cwd)==-ENOENT);
    assert(pw_game_startup_argv(1,arguments,&wine_arguments,&cwd)==-EINVAL);
    assert(!strstr(logs,"session-private") && !strstr(logs,token_seen));

    char *runelite[]={"C:\\RuneLite\\RuneLite.exe",NULL};
    assert(!pw_game_wire_encode(&game_wire,runelite,"C:\\RuneLite","/prefix/drive_c/RuneLite",original,sizeof(original)));
    reset();platform_status=0;
    action=setjmp(stopped);if(!action) pw_maintenance_loadexec_game("shutdown complete");
    assert(action==2 && game_requests==1 && !exit_requests);
    assert(pw_game_startup_argv(2,arguments,&wine_arguments,&cwd)==4);
    assert(!strcmp(wine_arguments[0],PW_RUNTIME_LOADER) && !strcmp(wine_arguments[1],runelite[0]));
    assert(!strcmp(wine_arguments[2],"--launch-mode=REFLECT"));
    assert(!strcmp(wine_arguments[3],"--hw-accel=AUTO") && !wine_arguments[4]);
    assert(!strcmp(cwd,"/prefix/drive_c/RuneLite") && !strcmp(native_runtime.parent_windows_cwd,"C:\\RuneLite"));
    assert(native_runtime.parent_environment_bytes==sizeof(original) && !memcmp(native_runtime.parent_environment,original,sizeof(original)));
    assert(!strstr(logs,"session-private") && !strstr(logs,token_seen));
    assert(game_parent_options.profile->kind==WINE_PS5_GAME_RUNELITE && !strcmp(game_parent_options.launch_mode,"REFLECT"));
    const char config[]="launch_mode=REFLECT\nhw_accel=OPENGL\n";
    int config_fd=open(runelite_config_path,O_CREAT|O_TRUNC|O_WRONLY,0666);
    assert(config_fd>=0 && write(config_fd,config,sizeof(config)-1)==(ssize_t)(sizeof(config)-1) && !close(config_fd));
    reset();action=setjmp(stopped);if(!action) pw_maintenance_loadexec_game("shutdown complete");
    assert(action==2 && pw_game_startup_argv(2,arguments,&wine_arguments,&cwd)==4);
    assert(!strcmp(wine_arguments[3],"--hw-accel=OPENGL") && !strcmp(game_parent_options.hw_source,"file"));
    assert(native_runtime.parent_environment_bytes==sizeof(original) && !memcmp(native_runtime.parent_environment,original,sizeof(original)));
    assert(!unlink(runelite_config_path));
    reset();platform_status=(int)0x80a10003u;
    action=setjmp(stopped);if(!action) pw_maintenance_loadexec_game("shutdown complete");
    assert(action==1 && game_requests==1 && exit_requests==1);
    assert(strstr(message,"RuneScape could not start RuneLite") && !strstr(message,"Old School RuneScape"));
    assert(!strstr(logs,"session-private") && !strstr(logs,token_seen));
    assert(!pw_game_wire_encode(&game_wire,program,"C:\\Game","/prefix/drive_c/Game",original,sizeof(original)));

    reset();action=setjmp(stopped);if(!action) pw_runescape_close("launcher closed",0,0);
    assert(action==1 && exit_requests==1 && !game_requests && !delayed_us && !*message);
    assert(snprintf(private_directory,sizeof(private_directory),"%s/missing/private",root)<(int)sizeof(private_directory));
    reset();action=setjmp(stopped);if(!action) pw_maintenance_loadexec_game("shutdown complete");
    assert(action==1 && !game_requests && exit_requests==1 && delayed_us==3000000);
    assert(strstr(message,"prepare the private OSRS handoff") && strstr(message,"Closing the title automatically"));
    assert(strstr(logs,"operation=mkdir-directory") && strstr(logs,"directory_mode="));
    reset();query_status=(int)0x80000001u;
    action=setjmp(stopped);if(!action) pw_runescape_close("launcher closed",0,0);
    assert(action==3 && query_calls==1 && !exit_requests && !game_requests);
    assert(strstr(logs,"self-close withheld reason=own-app-identity"));
    reset();own_app_id=0;
    action=setjmp(stopped);if(!action) pw_runescape_close("launcher closed",0,0);
    assert(action==3 && query_calls==1 && !exit_requests && !game_requests);
    assert(snprintf(private_directory,sizeof(private_directory),"%s/private",root)<(int)sizeof(private_directory));
    assert(!rmdir(private_directory) && !rmdir(root));
    puts("runescape LoadExec: compact argv52 bytes, private large-env transfer/read-once, failed-exec cleanup/code/autoclose, normal close PASS");
    return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
source = args.out / 'test-runescape-loadexec.c'
source.write_text(harness + '\n' + functions + '\n' + cases)
output = args.out / 'test-runescape-loadexec'
subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
    '-I' + str(here), '-I' + str(args.wine / 'include'), str(source), '-o', str(output)], check=True)
subprocess.run([str(output)], check=True)
