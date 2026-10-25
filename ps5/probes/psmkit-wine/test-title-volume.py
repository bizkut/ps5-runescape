#!/usr/bin/env python3
"""Run real Wine volume functions with descriptor/driver boundaries isolated.

Files, directory identity, capacity and error translation are real host inputs;
no mount-manager service is provided, matching the title topology. This is not
PS5/JVM execution or arbitrary reparse-point qualification.
"""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
parser.add_argument('--title-only', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = spec_from_file_location('volume_extraction', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
unix = args.wine / 'dlls/ntdll/unix/file.c'
win32 = args.wine / 'dlls/kernelbase/volume.c'
functions = '\n\n'.join(extract.definition(unix, name) for name in (
    'get_full_size_info', 'get_full_size_info_ex', 'get_drives_info', 'find_dos_device',
    'get_mountmgr_fs_info', 'is_device_placeholder', 'get_device_info', 'NtQueryVolumeInformationFile'))
functions += '\n\n' + '\n\n'.join(extract.definition(win32, name) for name in (
    'open_device_root', 'get_mountmgr_drive_type', 'GetVolumeInformationByHandleW',
    'GetVolumeInformationW', 'GetDriveTypeW', 'is_dos_path', 'GetVolumePathNameW'))
error_function = extract.definition(args.wine / 'dlls/ntdll/error.c', 'RtlNtStatusToDosErrorNoTeb')
harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/major.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#define WINE_MOUNTMGR_EXTENSIONS
#include "ddk/mountmgr.h"
#include "wine/ps5_writebroker.h"
#undef WIN32_NO_STATUS
#define HAVE_FSTATFS 1
#ifndef VOLUME_CONTROL
#define __PROSPERO__ 1
#endif
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define FIXME(...) ((void)0)
#define ERR(...) ((void)0)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array) (sizeof(array)/sizeof((array)[0]))
#endif
#include "error.h"
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define MAX_DOS_DRIVES 26
#define ps5_title_flush_all_coalesced_writes_reason(...) STATUS_SUCCESS
#define ps5_title_flush_coalesced_write_reason(...) STATUS_SUCCESS
static char croot[PATH_MAX], wroot[PATH_MAX], dosroot[PATH_MAX];
static const char *dosdevices_dir = dosroot;
static DWORD last_error;
static unsigned mountmgr_opens, broker_queries;
static struct wb_statfs_reply last_space;
struct file_identity { dev_t dev; ino_t ino; };
struct host_handle { int fd; char path[PATH_MAX]; };
static struct host_handle handles[64];
static int title_enabled = 1;
static int ps5_direct_enabled(void) { return title_enabled; }
static const char *ps5_title_drive(char drive) { return drive == 'c' ? croot : drive == 'w' ? wroot : NULL; }
static size_t wide_length(const WCHAR *value) { size_t n=0;while(value[n]) ++n;return n; }
static int wide_compare(const WCHAR *a,const WCHAR *b,size_t count) {
    while(count--) { if(*a!=*b) return *a>*b ? 1 : -1;if(!*a) return 0;++a;++b; }return 0;
}
static WCHAR *wide_find(const WCHAR *value,WCHAR ch) {
    do { if(*value==ch) return (WCHAR *)value; }while(*value++);return NULL;
}
#define wcslen wide_length
#define wcsncmp wide_compare
#define wcschr wide_find
#define towupper(ch) ((ch)>='a' && (ch)<='z' ? (ch)-'a'+'A' : (ch))
DWORD WINAPI GetLastError(void) { return last_error; }
void WINAPI SetLastError(DWORD value) { last_error=value; }
'''
harness += error_function + r'''
ULONG WINAPI RtlNtStatusToDosError(NTSTATUS status) { return RtlNtStatusToDosErrorNoTeb(status); }
static BOOL set_ntstatus(NTSTATUS status) { if(status) SetLastError(RtlNtStatusToDosError(status));return !status; }
static NTSTATUS errno_to_status(int error) {
    if(error==ENOENT) return STATUS_OBJECT_NAME_NOT_FOUND;
    if(error==EBADF) return STATUS_INVALID_HANDLE;
    if(error==EACCES) return STATUS_ACCESS_DENIED;
    if(error==ENOMEM) return STATUS_NO_MEMORY;
    return STATUS_UNSUCCESSFUL;
}
static void init_unicode_string(UNICODE_STRING *value,const WCHAR *text) {
    value->Buffer=(WCHAR *)text;value->Length=wide_length(text)*sizeof(WCHAR);value->MaximumLength=value->Length+sizeof(WCHAR);
}
static void widen(WCHAR *out,const char *text) { do { *out++=(unsigned char)*text; }while(*text++); }
static int wide_equal(const WCHAR *value,const char *text) {
    while(*value && *text && *value==(unsigned char)*text) { ++value;++text; }return !*value && !*text;
}
BOOLEAN WINAPI RtlDosPathNameToNtPathName_U(const WCHAR *path,UNICODE_STRING *result,WCHAR **part,CURDIR *relative) {
    size_t length;
    (void)part;(void)relative;
    if(!path || !*path) return FALSE;
    if(path[0]=='\\' && !path[1]) path=L"C:\\";
    length=wide_length(path);
    result->Buffer=malloc((length+5)*sizeof(WCHAR));assert(result->Buffer);
    memcpy(result->Buffer,L"\\??\\",4*sizeof(WCHAR));
    for(size_t i=0;i<=length;i++) result->Buffer[i+4]=path[i]=='/' ? '\\' : path[i];
    result->Length=(length+4)*sizeof(WCHAR);result->MaximumLength=(length+5)*sizeof(WCHAR);return TRUE;
}
void WINAPI RtlFreeUnicodeString(UNICODE_STRING *string) { free(string->Buffer);string->Buffer=NULL; }
UINT WINAPI GetCurrentDirectoryW(UINT capacity,WCHAR *buffer) {
    const WCHAR cwd[]=L"C:\\Game";if(capacity<ARRAY_SIZE(cwd)) return ARRAY_SIZE(cwd);memcpy(buffer,cwd,sizeof(cwd));return ARRAY_SIZE(cwd)-1;
}
WCHAR *WINAPI lstrcpynW(WCHAR *dest,const WCHAR *source,int count) {
    int i=0;if(count<=0) return dest;while(i+1<count && source[i]) { dest[i]=source[i];++i; }dest[i]=0;return dest;
}
static NTSTATUS unix_path(const UNICODE_STRING *name,char path[PATH_MAX]) {
    const WCHAR *p=name->Buffer;size_t units=name->Length/sizeof(WCHAR),n;
    const char *root;
    if(units<6 || wide_compare(p,L"\\??\\",4) || p[5]!=':') return STATUS_OBJECT_PATH_NOT_FOUND;
    root=ps5_title_drive((char)towupper(p[4])-'A'+'a');if(!root) return STATUS_OBJECT_PATH_NOT_FOUND;
    n=strlen(root);assert(n<PATH_MAX);memcpy(path,root,n);
    for(size_t i=6;i<units;i++) { if(n+1>=PATH_MAX || p[i]>127) return STATUS_NAME_TOO_LONG;path[n++]=p[i]=='\\' ? '/' : p[i]; }
    path[n]=0;return STATUS_SUCCESS;
}
NTSTATUS WINAPI NtOpenFile(HANDLE *result,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes,IO_STATUS_BLOCK *io,ULONG sharing,ULONG options) {
    char path[PATH_MAX];struct stat status;int fd;NTSTATUS error=unix_path(attributes->ObjectName,path);
    (void)access;(void)sharing;
    if(error) return error;
    if(stat(path,&status)) return errno_to_status(errno);
    if((options&FILE_NON_DIRECTORY_FILE) && S_ISDIR(status.st_mode)) return STATUS_FILE_IS_A_DIRECTORY;
    if((options&FILE_DIRECTORY_FILE) && !S_ISDIR(status.st_mode)) return STATUS_NOT_A_DIRECTORY;
    fd=open(path,O_RDONLY);if(fd<0) return errno_to_status(errno);assert(fd<(int)ARRAY_SIZE(handles));
    handles[fd].fd=fd;strcpy(handles[fd].path,path);*result=&handles[fd];io->Status=STATUS_SUCCESS;return STATUS_SUCCESS;
}
NTSTATUS WINAPI NtClose(HANDLE handle) { struct host_handle *h=handle;assert(h>=handles && h<handles+ARRAY_SIZE(handles));assert(!close(h->fd));return STATUS_SUCCESS; }
NTSTATUS WINAPI NtQueryAttributesFile(const OBJECT_ATTRIBUTES *attributes,FILE_BASIC_INFORMATION *info) {
    char path[PATH_MAX];struct stat status;NTSTATUS error=unix_path(attributes->ObjectName,path);
    if(error) return error;if(stat(path,&status)) return errno_to_status(errno);
    memset(info,0,sizeof(*info));if(S_ISDIR(status.st_mode)) info->FileAttributes|=FILE_ATTRIBUTE_DIRECTORY;return STATUS_SUCCESS;
}
NTSTATUS WINAPI NtQueryInformationFile(HANDLE h,IO_STATUS_BLOCK *io,void *info,ULONG bytes,FILE_INFORMATION_CLASS type) {
    (void)h;(void)io;(void)info;(void)bytes;(void)type;assert(!"unexercised reparse boundary");return STATUS_NOT_IMPLEMENTED;
}
static BOOL resolve_symlink(UNICODE_STRING *path) { (void)path;assert(!"unexercised reparse boundary");return FALSE; }
HANDLE WINAPI CreateFileW(const WCHAR *path,DWORD access,DWORD sharing,SECURITY_ATTRIBUTES *security,DWORD disposition,DWORD flags,HANDLE template) {
    (void)path;(void)access;(void)sharing;(void)security;(void)disposition;(void)flags;(void)template;
    mountmgr_opens++;SetLastError(ERROR_FILE_NOT_FOUND);return INVALID_HANDLE_VALUE;
}
BOOL WINAPI CloseHandle(HANDLE handle) { return !NtClose(handle); }
BOOL WINAPI DeviceIoControl(HANDLE handle,DWORD code,void *input,DWORD input_size,void *output,DWORD output_size,DWORD *returned,OVERLAPPED *overlapped) {
    (void)handle;(void)code;(void)input;(void)input_size;(void)output;(void)output_size;(void)returned;(void)overlapped;
    assert(!"unavailable mount manager cannot accept ioctl");return FALSE;
}
static NTSTATUS server_get_unix_name(HANDLE handle,char **name) {
    struct host_handle *h=handle;*name=strdup(h->path);return *name ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
static NTSTATUS server_open_file_object(HANDLE *handle,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes,ULONG sharing,ULONG options) {
    (void)handle;(void)access;(void)attributes;(void)sharing;(void)options;mountmgr_opens++;return STATUS_OBJECT_NAME_NOT_FOUND;
}
static NTSTATUS sync_ioctl(HANDLE h,ULONG code,void *in,ULONG in_size,void *out,ULONG out_size) {
    (void)h;(void)code;(void)in;(void)in_size;(void)out;(void)out_size;assert(!"unavailable mount manager ioctl");return STATUS_NOT_IMPLEMENTED;
}
enum server_fd_type { FD_TYPE_FILE, FD_TYPE_SOCKET };
static NTSTATUS server_get_unix_fd(HANDLE handle,ACCESS_MASK access,int *fd,int *close_fd,enum server_fd_type *type,void *options) {
    struct host_handle *h=handle;(void)access;(void)options;if(!h) return STATUS_INVALID_HANDLE;
    *fd=h->fd;*close_fd=0;*type=0;return STATUS_SUCCESS;
}
static int broker_query_fs(HANDLE handle,struct wb_statfs_reply *reply) {
    struct host_handle *h=handle;struct statvfs info;assert(!fstatvfs(h->fd,&info));
    memset(reply,0,sizeof(*reply));reply->magic=WB_MAGIC;reply->bsize=info.f_frsize;
    reply->blocks=info.f_blocks;reply->bfree=info.f_bfree;reply->bavail=info.f_bavail;last_space=*reply;broker_queries++;return 1;
}
struct async_irp { void *buffer;ULONG size;int io; };
static void irp_completion(void) { assert(!"unexercised async device boundary"); }
static void *alloc_fileio(size_t bytes,void (*callback)(void),HANDLE handle) {
    (void)bytes;(void)callback;(void)handle;assert(!"unexercised async device boundary");return NULL;
}
static NTSTATUS wait_async(HANDLE handle,BOOL alertable) { (void)handle;(void)alertable;assert(0);return STATUS_NOT_IMPLEMENTED; }
struct fake_request { ULONG async,handle,info_class; };
struct fake_reply { ULONG_PTR wait; };
#define SERVER_START_REQ(name) do { struct fake_request request={0},*req=&request;struct fake_reply response={0},*reply=&response;
#define SERVER_END_REQ } while(0)
#define server_async(...) 0
#define wine_server_obj_handle(handle) 0
#define wine_server_set_reply(...) ((void)0)
#define wine_server_call(req) ((void)(req),STATUS_OBJECT_NAME_NOT_FOUND)
#define wine_server_reply_size(reply) ((void)(reply),0)
#define wine_server_ptr_handle(value) ((HANDLE)(UINT_PTR)(value))
'''
cases = r'''
int main(void) {
    char root[]="/tmp/title-volume-XXXXXX",path[PATH_MAX],linkpath[PATH_MAX];
    WCHAR label[32],fsname[16],volume_path[16];DWORD serial,serial_file,maximum,flags;
    struct stat root_identity;HANDLE handle;IO_STATUS_BLOCK io;UNICODE_STRING name;OBJECT_ATTRIBUTES attr;
    assert(mkdtemp(root));assert(snprintf(croot,sizeof(croot),"%s/c",root)<(int)sizeof(croot));assert(!mkdir(croot,0777));
    assert(snprintf(wroot,sizeof(wroot),"%s/w",root)<(int)sizeof(wroot));assert(!mkdir(wroot,0777));
    assert(snprintf(dosroot,sizeof(dosroot),"%s/dosdevices",root)<(int)sizeof(dosroot));assert(!mkdir(dosroot,0777));
    assert(snprintf(linkpath,sizeof(linkpath),"%s/c:",dosroot)<(int)sizeof(linkpath));assert(!symlink(croot,linkpath));
    assert(snprintf(linkpath,sizeof(linkpath),"%s/w:",dosroot)<(int)sizeof(linkpath));assert(!symlink(wroot,linkpath));
    assert(snprintf(path,sizeof(path),"%s/Game",croot)<(int)sizeof(path));assert(!mkdir(path,0777));
    assert(snprintf(path,sizeof(path),"%s/Game/settings.json",croot)<(int)sizeof(path));int fd=open(path,O_CREAT|O_WRONLY,0666);assert(fd>=0 && !close(fd));
    assert(GetVolumePathNameW(L"C:\\Game\\settings.json",volume_path,ARRAY_SIZE(volume_path)) && wide_equal(volume_path,"C:\\"));
    assert(GetDriveTypeW(volume_path)==DRIVE_FIXED);
    SetLastError(0x12345678);
    BOOL result=GetVolumeInformationW(volume_path,label,ARRAY_SIZE(label),&serial,&maximum,&flags,fsname,ARRAY_SIZE(fsname));
#ifdef VOLUME_CONTROL
    assert(!result && GetLastError()==ERROR_INVALID_FUNCTION);
    puts("TITLE_VOLUME_CONTROL_PASS unchanged mount-manager failure is ERROR_INVALID_FUNCTION");
#else
    if(!result) fprintf(stderr,"TITLE_VOLUME_REPRO Win32 error=%lu (mount manager absent)\n",(unsigned long)GetLastError());
    assert(result);
    assert(wide_equal(fsname,"NTFS") && maximum==255 && !(flags&FILE_READ_ONLY_VOLUME));
    assert(!stat(croot,&root_identity));
    ULONGLONG identity=(ULONGLONG)root_identity.st_dev^(ULONGLONG)root_identity.st_ino;
    assert(serial==(DWORD)(identity^(identity>>32)) && !label[0]);
    assert(GetVolumeInformationW(L"C:\\",NULL,0,&serial_file,NULL,NULL,NULL,0) && serial_file==serial);
    assert(GetVolumeInformationW(L"W:\\",NULL,0,&serial_file,NULL,NULL,fsname,ARRAY_SIZE(fsname)) && wide_equal(fsname,"NTFS"));
    assert(!GetVolumeInformationW(L"C:\\Game\\",label,ARRAY_SIZE(label),NULL,NULL,NULL,NULL,0));
    assert(GetLastError()==ERROR_DIR_NOT_ROOT);
    assert(!GetVolumeInformationW(L"C:\\",NULL,0,NULL,NULL,NULL,fsname,3) && GetLastError()==ERROR_BAD_LENGTH);
    assert(!GetVolumeInformationW(L"Z:\\",NULL,0,NULL,NULL,NULL,fsname,ARRAY_SIZE(fsname)));
    assert(GetDriveTypeW(L"Z:\\")==DRIVE_NO_ROOT_DIR);
    WCHAR file_path[]=L"C:\\Game\\settings.json";
    assert(RtlDosPathNameToNtPathName_U(file_path,&name,NULL,NULL));InitializeObjectAttributes(&attr,&name,0,NULL,NULL);
    assert(!NtOpenFile(&handle,FILE_READ_ATTRIBUTES,&attr,&io,FILE_SHARE_READ,0));RtlFreeUnicodeString(&name);
    assert(GetVolumeInformationByHandleW(handle,NULL,0,&serial_file,&maximum,&flags,fsname,ARRAY_SIZE(fsname)) && serial_file==serial);
    union { ULONGLONG alignment;BYTE bytes[sizeof(struct mountmgr_unix_drive)+8]; } descriptor;
    memset(descriptor.bytes,0x5a,sizeof(descriptor.bytes));
    struct mountmgr_unix_drive *logical=(struct mountmgr_unix_drive *)descriptor.bytes;
    assert(get_mountmgr_fs_info(handle,((struct host_handle *)handle)->fd,logical,sizeof(*logical)-1)==STATUS_BUFFER_TOO_SMALL);
    assert(descriptor.bytes[0]==0x5a && descriptor.bytes[sizeof(*logical)]==0x5a);
    assert(!get_mountmgr_fs_info(handle,((struct host_handle *)handle)->fd,logical,sizeof(*logical)));
    assert(logical->unix_dev==(ULONGLONG)root_identity.st_dev && logical->serial==serial && logical->letter=='c');
    assert(logical->fs_type==MOUNTMGR_FS_TYPE_NTFS && logical->type==DRIVE_FIXED);
    assert(descriptor.bytes[sizeof(*logical)]==0x5a);
    FILE_FS_FULL_SIZE_INFORMATION space;FILE_FS_FULL_SIZE_INFORMATION_EX space_ex;
    assert(!NtQueryVolumeInformationFile(handle,&io,&space,sizeof(space),FileFsFullSizeInformation));
    assert(io.Information==sizeof(space) && broker_queries);
    assert(space.TotalAllocationUnits.QuadPart==(LONGLONG)(last_space.bsize*last_space.blocks/4096));
    assert(space.CallerAvailableAllocationUnits.QuadPart==(LONGLONG)(last_space.bsize*last_space.bavail/4096));
    assert(!NtQueryVolumeInformationFile(handle,&io,&space_ex,sizeof(space_ex),FileFsFullSizeInformationEx));
    assert(io.Information==sizeof(space_ex) && space_ex.ActualTotalAllocationUnits==(ULONGLONG)space.TotalAllocationUnits.QuadPart);
    union { ULONGLONG alignment;BYTE data[128]; } buffer;
    memset(buffer.data,0x5a,sizeof(buffer.data));
    assert(NtQueryVolumeInformationFile(handle,&io,buffer.data,sizeof(FILE_FS_VOLUME_INFORMATION)-1,FileFsVolumeInformation)==STATUS_INFO_LENGTH_MISMATCH);
    assert(io.Status==STATUS_INFO_LENGTH_MISMATCH && !io.Information && buffer.data[0]==0x5a);
    assert(NtQueryVolumeInformationFile(handle,&io,buffer.data,sizeof(FILE_FS_ATTRIBUTE_INFORMATION)-1,FileFsAttributeInformation)==STATUS_INFO_LENGTH_MISMATCH);
    assert(io.Status==STATUS_INFO_LENGTH_MISMATCH && !io.Information && buffer.data[0]==0x5a);
    assert(!NtQueryVolumeInformationFile(handle,&io,buffer.data,sizeof(buffer.data)-1,FileFsAttributeInformation));
    assert(io.Information==offsetof(FILE_FS_ATTRIBUTE_INFORMATION,FileSystemName)+8 && buffer.data[127]==0x5a);
    FILE_FS_DEVICE_INFORMATION device;
    assert(!NtQueryVolumeInformationFile(handle,&io,&device,sizeof(device),FileFsDeviceInformation));
    assert(device.DeviceType==FILE_DEVICE_DISK_FILE_SYSTEM && !(device.Characteristics&(FILE_REMOTE_DEVICE|FILE_REMOVABLE_MEDIA)));
    assert(!NtClose(handle));
    puts("TITLE_VOLUME_HOST_PASS source=REGISTERED_REAL_DIRECTORY_IDENTITY fs=NTFS C_W=FIXED capacity=BROKER no_mountmgr=1 APIs=PRODUCTION_EXTRACTED");
#endif
    assert(snprintf(path,sizeof(path),"%s/Game/settings.json",croot)<(int)sizeof(path));assert(!unlink(path));
    assert(snprintf(path,sizeof(path),"%s/Game",croot)<(int)sizeof(path));assert(!rmdir(path));
    assert(snprintf(linkpath,sizeof(linkpath),"%s/c:",dosroot)<(int)sizeof(linkpath));assert(!unlink(linkpath));
    assert(snprintf(linkpath,sizeof(linkpath),"%s/w:",dosroot)<(int)sizeof(linkpath));assert(!unlink(linkpath));
    assert(!rmdir(croot) && !rmdir(wroot) && !rmdir(dosroot) && !rmdir(root));return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
for control in (False,) if args.title_only else (False, True):
    source = args.out / ('test-title-volume' + ('-control' if control else '') + '.c')
    source.write_text(harness + '\n' + functions + '\n' + cases)
    binary = source.with_suffix('')
    subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-fshort-wchar', '-O2', '-g', '-Wall', '-Wextra',
        '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-sign-compare',
        *(['-DVOLUME_CONTROL'] if control else []), '-idirafter', str(args.wine / 'include'),
        '-I', str(args.wine / 'dlls/ntdll'), str(source), '-o', str(binary), '-pthread'], check=True)
    subprocess.run([str(binary)], check=True)
