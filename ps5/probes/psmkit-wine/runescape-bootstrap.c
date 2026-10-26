/* App-owned maintenance bootstrap, synchronized from immutable app storage.
 * Current-time policy deliberately does not honor Wine's unverified legacy
 * timestamp. Unknown/offline revocation is a rejection, not soft success. */
#define WIN32_LEAN_AND_MEAN
/* MinGW follows the SDK's opt-in for the extended CryptoAPI structures.
 * Their full cbSize makes Wine honor the explicit retrieval timeouts below. */
#define CERT_CHAIN_PARA_HAS_EXTRA_FIELDS
#define CERT_REVOCATION_PARA_HAS_EXTRA_FIELDS
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mssip.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include <winver.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <msi.h>
#include "runescape-redist.h"
#include "runescape-ssn.h"
#include "runescape-launcher-retry.h"
#include "sha256.h"
#include "runescape-product.h"
#define RS_POLICY "jagex-current-time-sha2-exclusive-g4-revocation-v1"
#define RS_APP L"C:\\RuneScape\\"
#define RS_STAGE_DOS L"C:\\Jagex Launcher.stage"
#define RS_META L"C:\\ProgramData\\Jagex Launcher\\launcher-win.json"
/* The title's Wine debug writer goes straight to klog even when the PE CRT's
 * standard handles are unavailable. Do not enable global debugstr (vendor
 * messages can contain credentials). On Windows use ordinary stdout. */
static void rs_logf(const char *format,...)
{
    char line[1024];va_list args;
    memcpy(line,"runescape: ",11);
    va_start(args,format);vsnprintf(line+11,sizeof(line)-12,format,args);va_end(args);
    size_t length=strlen(line);line[length++]='\n';line[length]=0;
    typedef int (WINAPI *debug_write_fn)(const char *,unsigned);
    static debug_write_fn debug_write;
    static int resolved;
    if(!resolved) {
        debug_write=(debug_write_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"__wine_dbg_write");
        resolved=1;
    }
    if(debug_write) debug_write(line,(unsigned)length);
    else { fputs(line,stdout);fflush(stdout); }
}
static void rs_log(const char *text)
{ rs_logf("%s",text); }
static int rs_fail(const WCHAR *reason)
{
    rs_log("bootstrap rejected policy=" RS_POLICY);
    MessageBoxW(NULL,reason,L"RuneScape Launcher: installation blocked",MB_OK|MB_ICONERROR);
    return 1;
}
static void rs_hex(const unsigned char *bytes,size_t n,char *out)
{
    static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<n;i++) { out[i*2]=digits[bytes[i]>>4];out[i*2+1]=digits[bytes[i]&15]; }
    out[n*2]=0;
}
static int rs_hash(HANDLE file,char digest[65])
{
    unsigned char buffer[65536],bytes[32]; DWORD got; struct sha256_context hash;
    LARGE_INTEGER zero;zero.QuadPart=0;
    if(!SetFilePointerEx(file,zero,NULL,FILE_BEGIN)) return 0;
    sha256_init(&hash);
    for(;;) { if(!ReadFile(file,buffer,sizeof(buffer),&got,NULL)) return 0;
        if(!got) break;
        sha256_update(&hash,buffer,got); }
    sha256_final(&hash,bytes);rs_hex(bytes,32,digest);
    return SetFilePointerEx(file,zero,NULL,FILE_BEGIN);
}
static int rs_name(PCCERT_CONTEXT cert,const char *oid)
{
    WCHAR name[128];
    DWORD n=CertGetNameStringW(cert,CERT_NAME_ATTR_TYPE,0,(void *)oid,name,128);
    return n>1 && n<=128 && !wcscmp(name,RS_PUBLISHER);
}
static int rs_sha2(const char *oid)
{ return oid && (!strcmp(oid,szOID_NIST_sha256)||!strcmp(oid,szOID_NIST_sha384)||!strcmp(oid,szOID_NIST_sha512)); }
static HCERTSTORE rs_roots(void)
{
    HANDLE file=CreateFileW(RS_APP L"DigiCertTrustedRootG4.der",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) return NULL;
    unsigned char der[4096]; DWORD bytes=0; char digest[65];
    int valid=rs_hash(file,digest) && !strcmp(digest,"552f7bdcf1a7af9e6ce672017f4f12abf77240c78e761ac203d1d9d20ac89988") &&
        ReadFile(file,der,sizeof(der),&bytes,NULL) && bytes==1428;
    CloseHandle(file); if(!valid) return NULL;
    HCERTSTORE roots=CertOpenStore(CERT_STORE_PROV_MEMORY,0,0,CERT_STORE_CREATE_NEW_FLAG,NULL);
    if(roots && !CertAddEncodedCertificateToStore(roots,X509_ASN_ENCODING,der,bytes,CERT_STORE_ADD_NEW,NULL)) {
        CertCloseStore(roots,0);roots=NULL;
    }
    return roots;
}
static void rs_version(const WCHAR *path,char product[64],char file_version[64])
{
    strcpy(product,"unavailable");
    if(file_version) strcpy(file_version,"unavailable");
    DWORD ignored=0,bytes=GetFileVersionInfoSizeW(path,&ignored);
    if(!bytes || bytes>1048576) return;
    void *data=malloc(bytes); VS_FIXEDFILEINFO *info=NULL; UINT size=0;
    if(data && GetFileVersionInfoW(path,0,bytes,data) && VerQueryValueW(data,L"\\",(void **)&info,&size) &&
       size>=sizeof(*info) && info->dwSignature==VS_FFI_SIGNATURE) {
        snprintf(product,64,"%u.%u.%u.%u",HIWORD(info->dwProductVersionMS),LOWORD(info->dwProductVersionMS),
                 HIWORD(info->dwProductVersionLS),LOWORD(info->dwProductVersionLS));
        if(file_version) snprintf(file_version,64,"%u.%u.%u.%u",HIWORD(info->dwFileVersionMS),LOWORD(info->dwFileVersionMS),
                                 HIWORD(info->dwFileVersionLS),LOWORD(info->dwFileVersionLS));
    }
    free(data);
}
/* Returned read-only handle denies concurrent writes/deletion until the caller
 * has published/launched this exact verified file. No receipt bypass exists. */
static HANDLE rs_verify(const WCHAR *path,const char *kind)
{
    HANDLE lock=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(lock==INVALID_HANDLE_VALUE) return lock;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO file={0};WINTRUST_DATA data={0};
    HCERTSTORE roots=NULL,embedded=NULL;HCRYPTMSG message=NULL;HCERTCHAINENGINE engine=NULL;
    PCCERT_CHAIN_CONTEXT chain=NULL; int accepted=0;char digest[65]={0},cert_digest[65]={0},version[64],file_version[64];
    DWORD content=0,encoding=0,format=0;
    const char *gate="authenticode";DWORD chain_error=0;
    file.cbStruct=sizeof(file);file.pcwszFilePath=path;file.hFile=lock;
    data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;
    data.dwProvFlags=WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT|WTD_DISABLE_MD2_MD4;
    LONG trust=WinVerifyTrust(NULL,&action,&data);
    if(trust || !rs_hash(lock,digest) ||
       !CryptQueryObject(CERT_QUERY_OBJECT_FILE,path,CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
          CERT_QUERY_FORMAT_FLAG_BINARY,0,&encoding,&content,&format,&embedded,&message,NULL)) goto done;
    DWORD signers=0,bytes=sizeof(signers);
    if(!CryptMsgGetParam(message,CMSG_SIGNER_COUNT_PARAM,0,&signers,&bytes) || signers!=1) goto done;
    CRYPT_PROVIDER_DATA *provider=WTHelperProvDataFromStateData(data.hWVTStateData);
    CRYPT_PROVIDER_SGNR *signer=provider ? WTHelperGetProvSignerFromChain(provider,0,FALSE,0) : NULL;
    if(!signer || provider->csSigners!=1 || signer->dwError || !signer->csCertChain || !signer->pasCertChain || !signer->psSigner ||
       !rs_sha2(signer->psSigner->HashAlgorithm.pszObjId) ||
       !provider->pPDSip || !provider->pPDSip->psIndirectData ||
       !provider->pPDSip->psIndirectData->Data.pszObjId ||
       strcmp(provider->pPDSip->psIndirectData->Data.pszObjId,SPC_PE_IMAGE_DATA_OBJID) ||
       !rs_sha2(provider->pPDSip->psIndirectData->DigestAlgorithm.pszObjId)) goto done;
    gate="publisher_usage_current_time";
    PCCERT_CONTEXT cert=signer->pasCertChain[0].pCert;
    if(!cert || !rs_name(cert,szOID_ORGANIZATION_NAME) || !rs_name(cert,szOID_COMMON_NAME)) goto done;
    FILETIME now;GetSystemTimeAsFileTime(&now);
    if(CertVerifyTimeValidity(&now,cert->pCertInfo)!=0) goto done;
    /* Require an explicit code-signing EKU, not unrestricted/absent usage. */
    DWORD eku_bytes=0;
    if(!CertGetEnhancedKeyUsage(cert,CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG,NULL,&eku_bytes) ||
       eku_bytes>65536 || eku_bytes<sizeof(CERT_ENHKEY_USAGE)) goto done;
    CERT_ENHKEY_USAGE *eku=malloc(eku_bytes);int code_signing=0;
    if(eku && CertGetEnhancedKeyUsage(cert,CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG,eku,&eku_bytes))
        for(DWORD i=0;i<eku->cUsageIdentifier;i++) if(!strcmp(eku->rgpszUsageIdentifier[i],szOID_PKIX_KP_CODE_SIGNING)) code_signing=1;
    free(eku);if(!code_signing) goto done;
    BYTE usage=0;
    if(!CertFindExtension(szOID_KEY_USAGE,cert->pCertInfo->cExtension,cert->pCertInfo->rgExtension) ||
       !CertGetIntendedKeyUsage(X509_ASN_ENCODING,cert->pCertInfo,&usage,sizeof(usage)) || !(usage&CERT_DIGITAL_SIGNATURE_KEY_USAGE)) goto done;
    gate="immutable_roots";
    roots=rs_roots();if(!roots) goto done;
    CERT_CHAIN_ENGINE_CONFIG config={0};config.cbSize=sizeof(config);config.hExclusiveRoot=roots;
    if(!CertCreateCertificateChainEngine(&config,&engine)) goto done;
    CERT_CHAIN_PARA parameters={0};parameters.cbSize=sizeof(parameters);
    char *usage_oid=szOID_PKIX_KP_CODE_SIGNING;
    parameters.RequestedUsage.dwType=USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage.cUsageIdentifier=1;parameters.RequestedUsage.Usage.rgpszUsageIdentifier=&usage_oid;
    parameters.dwUrlRetrievalTimeout=10000;
    gate="exclusive_chain";
    if(!CertGetCertificateChain(engine,cert,&now,embedded,&parameters,
        CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT,NULL,&chain)) { chain_error=GetLastError();goto done; }
    if(chain->TrustStatus.dwErrorStatus) { chain_error=chain->TrustStatus.dwErrorStatus;goto done; }
    CERT_CHAIN_POLICY_PARA policy={0};CERT_CHAIN_POLICY_STATUS status={0};
    policy.cbSize=sizeof(policy);status.cbSize=sizeof(status);
    if(!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASE,chain,&policy,&status) || status.dwError) goto done;
    /* Independently check every non-root revocation result. Wine's chain.c
     * maps some unmapped revocation failures to zero trust-error bits. A false
     * result or any nonzero status here is always a hard rejection. */
    gate="current_time_revocation";
    for(DWORD c=0;c<chain->cChain;c++) for(DWORD e=0;e<chain->rgpChain[c]->cElement;e++) {
        PCCERT_CONTEXT member=chain->rgpChain[c]->rgpElement[e]->pCertContext;
        if(CertVerifyTimeValidity(&now,member->pCertInfo)!=0) goto done;
        if(e+1<chain->rgpChain[c]->cElement) {
            CERT_REVOCATION_PARA revocation={0};CERT_REVOCATION_STATUS result={0};
            revocation.cbSize=sizeof(revocation);result.cbSize=sizeof(result);
            revocation.pIssuerCert=chain->rgpChain[c]->rgpElement[e+1]->pCertContext;
            revocation.pftTimeToUse=&now;revocation.dwUrlRetrievalTimeout=10000;
            if(!CertVerifyRevocation(X509_ASN_ENCODING,CERT_CONTEXT_REVOCATION_TYPE,1,(void **)&member,
                    CERT_VERIFY_REV_CHAIN_FLAG,&revocation,&result) || result.dwError) { chain_error=result.dwError;goto done; }
        }
    }
    unsigned char hash[32];DWORD hash_bytes=sizeof(hash);
    if(!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM,0,NULL,cert->pbCertEncoded,cert->cbCertEncoded,hash,&hash_bytes) || hash_bytes!=32) goto done;
    rs_hex(hash,32,cert_digest);rs_version(path,version,file_version);
    rs_logf("verify kind=%s digest=%s product_version=%s file_version=%s publisher_match=1 signer=Jagex_Limited cert_sha256=%s chain=valid policy=%s",kind,digest,version,file_version,cert_digest,RS_POLICY);
    accepted=1;
done:
    if(!accepted) rs_logf("verify kind=%s accepted=0 chain=unaccepted stage=%s trust=%08lx chain_error=%08lx policy=%s",kind,gate,(DWORD)trust,chain_error,RS_POLICY);
    if(chain) CertFreeCertificateChain(chain);
    if(engine) CertFreeCertificateChainEngine(engine);
    if(roots) CertCloseStore(roots,0);
    if(message) CryptMsgClose(message);
    if(embedded) CertCloseStore(embedded,0);
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(NULL,&action,&data);
    if(!accepted) { CloseHandle(lock);return INVALID_HANDLE_VALUE; }
    return lock;
}
static int rs_launch(const WCHAR *path,const WCHAR *arguments,const WCHAR *cwd,DWORD timeout,DWORD *exit)
{
    WCHAR command[2048];STARTUPINFOW startup={0};PROCESS_INFORMATION process={0};
    startup.cb=sizeof(startup);
    if(swprintf(command,2048,L"\"%ls\" %ls",path,arguments)<0) return 0;
    HANDLE job=NULL;
    if(timeout!=INFINITE) {
        /* Wine lazily starts its persistent explorer desktop in the first GUI
         * caller's job. Burn's clean-room child must not own that shared host:
         * it survives successful MSI completion and makes the job never drain.
         * Establish the real desktop here, before the installer job exists.
         * No descendant is exempted from the drain or killed on success. */
        HWND desktop=GetDesktopWindow();
        rs_logf("prerequisite desktop ready=%d error=%lu owner=bootstrap job=outside",
                desktop!=NULL,desktop ? 0 : GetLastError());
        if(!desktop) return 0;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        job=CreateJobObjectW(NULL,NULL);
        rs_logf("prerequisite launch stage=create-job success=%d error=%lu",job!=NULL,job ? 0 : GetLastError());
        int configured=job && SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
        rs_logf("prerequisite launch stage=configure-job success=%d error=%lu",configured,configured ? 0 : GetLastError());
        if(!configured) {
            if(job) CloseHandle(job);
            return 0;
        }
    }
    int created=CreateProcessW(path,command,NULL,NULL,FALSE,job ? CREATE_SUSPENDED : 0,NULL,cwd,&startup,&process);
    rs_logf("prerequisite launch stage=create-process success=%d error=%lu",created,created ? 0 : GetLastError());
    if(!created) {
        if(job) CloseHandle(job);
        return 0;
    }
    int assigned=!job || AssignProcessToJobObject(job,process.hProcess);
    rs_logf("prerequisite launch stage=assign-job success=%d error=%lu",assigned,assigned ? 0 : GetLastError());
    DWORD resumed=assigned && job ? ResumeThread(process.hThread) : 0;
    rs_logf("prerequisite launch stage=resume-thread result=%lu error=%lu",resumed,resumed==(DWORD)-1 ? GetLastError() : 0);
    if(!assigned || resumed==(DWORD)-1) {
        TerminateProcess(process.hProcess,ERROR_ACCESS_DENIED);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(job);return 0;
    }
    CloseHandle(process.hThread);
    DWORD wait=WaitForSingleObject(process.hProcess,timeout);
    int result=wait==WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess,exit);
    rs_logf("prerequisite launch stage=wait-root wait=%lu exit_status=%d exit=%lu error=%lu",
            wait,result,*exit,result ? 0 : GetLastError());
    if(wait==WAIT_TIMEOUT) { TerminateProcess(process.hProcess,ERROR_TIMEOUT);WaitForSingleObject(process.hProcess,10000);*exit=ERROR_TIMEOUT; }
    /* The next architecture must not overlap a still-running Burn companion
     * or custom-action host. Process exit alone is not descendant completion. */
    if(job && result) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting={0};
        unsigned waited=0;DWORD query_error=0;
        for(;;) {
            if(!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,
                    &accounting,sizeof(accounting),NULL)) { query_error=GetLastError();result=0;break; }
            if(!accounting.ActiveProcesses) break;
            if(waited>=10000) { result=0;break; }
            Sleep(10);waited+=10;
        }
        rs_logf("prerequisite launch stage=drain-job success=%d error=%lu active=%lu waited_ms=%u",
                result,query_error,accounting.ActiveProcesses,waited);
    }
    if(job) CloseHandle(job);
    CloseHandle(process.hProcess);return result;
}
struct rs_launcher_processes {
    HANDLE handles[33]; /* slot zero belongs to the handoff event */
    DWORD pids[32];
    unsigned count;
};
static void rs_launcher_release(struct rs_launcher_processes *processes)
{
    for(unsigned i=0;i<processes->count;i++) CloseHandle(processes->handles[i+1]);
    processes->count=0;
}
static int rs_launcher_snapshot(struct rs_launcher_processes *processes)
{
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    PROCESSENTRY32W entry={.dwSize=sizeof(entry)};
    processes->count=0;
    if(snapshot==INVALID_HANDLE_VALUE) return 0;
    BOOL more=Process32FirstW(snapshot,&entry);
    while(more) {
        if(!lstrcmpiW(entry.szExeFile,L"JagexLauncher.exe")) {
            HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE,FALSE,entry.th32ProcessID);
            if(!process) process=OpenProcess(SYNCHRONIZE,FALSE,entry.th32ProcessID);
            if(process) {
                if(processes->count==32) {
                    CloseHandle(process);CloseHandle(snapshot);rs_launcher_release(processes);return 0;
                }
                processes->pids[processes->count]=entry.th32ProcessID;
                processes->handles[++processes->count]=process;
            }
            else if(GetLastError()!=ERROR_INVALID_PARAMETER) {
                CloseHandle(snapshot);rs_launcher_release(processes);return 0;
            }
        }
        more=Process32NextW(snapshot,&entry);
    }
    DWORD error=GetLastError();CloseHandle(snapshot);
    if(error!=ERROR_NO_MORE_FILES) { rs_launcher_release(processes);return 0; }
    return 1;
}
struct rs_launcher_close {
    const struct rs_launcher_processes *processes;
    int end_session;
    ULONGLONG deadline;
    unsigned windows, vetoed;
};
static BOOL CALLBACK rs_launcher_close_window(HWND window,LPARAM parameter)
{
    struct rs_launcher_close *close=(void *)parameter;
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    unsigned i=0;while(i<close->processes->count && close->processes->pids[i]!=pid) i++;
    if(i==close->processes->count) return TRUE;
    ULONGLONG now=GetTickCount64();
    if(now>=close->deadline) return FALSE;
    close->windows++;
    if(!close->end_session) PostMessageW(window,WM_CLOSE,0,0);
    else {
        DWORD_PTR answer=0;
        UINT timeout=(UINT)(close->deadline-now);if(timeout>1000) timeout=1000;
        if(SendMessageTimeoutW(window,WM_QUERYENDSESSION,0,ENDSESSION_CLOSEAPP,
                               SMTO_ABORTIFHUNG|SMTO_BLOCK,timeout,&answer) && answer) {
            now=GetTickCount64();
            if(now<close->deadline) {
                timeout=(UINT)(close->deadline-now);if(timeout>1000) timeout=1000;
                SendMessageTimeoutW(window,WM_ENDSESSION,TRUE,ENDSESSION_CLOSEAPP,
                                    SMTO_ABORTIFHUNG|SMTO_BLOCK,timeout,&answer);
            }
        } else close->vetoed++;
    }
    return TRUE;
}
static DWORD rs_launcher_wait_all(DWORD timeout)
{
    ULONGLONG deadline=GetTickCount64()+timeout;
    for(;;) {
        struct rs_launcher_processes processes={0};
        if(!rs_launcher_snapshot(&processes)) return WAIT_FAILED;
        if(!processes.count) return WAIT_OBJECT_0;
        ULONGLONG now=GetTickCount64();
        DWORD wait=now>=deadline ? WAIT_TIMEOUT :
            WaitForMultipleObjects(processes.count,processes.handles+1,TRUE,(DWORD)(deadline-now));
        rs_launcher_release(&processes);
        if(wait!=WAIT_OBJECT_0) return wait;
    }
}
static int rs_wait_launcher(HANDLE launcher,HANDLE handoff,DWORD *exit)
{
    /* The elevated bootstrap instance can exit after relaying to the resident
     * launcher. Keep the event and Windows owner alive across that relay. */
    for(;;) {
        struct rs_launcher_processes processes={0};
        if(!rs_launcher_snapshot(&processes)) return 0;
        processes.handles[0]=handoff;
        unsigned count=processes.count+1;
        if(!processes.count) {
            if(WaitForSingleObject(launcher,0)==WAIT_OBJECT_0) return GetExitCodeProcess(launcher,exit);
            processes.handles[count++]=launcher;
        }
        DWORD wait=WaitForMultipleObjects(count,processes.handles,FALSE,INFINITE);
        rs_launcher_release(&processes);
        if(wait==WAIT_OBJECT_0) break;
        if(wait<WAIT_OBJECT_0+1 || wait>=WAIT_OBJECT_0+count) return 0;
    }
    struct rs_launcher_processes processes={0};
    int known=rs_launcher_snapshot(&processes);
    struct rs_launcher_close close={.processes=&processes,.deadline=GetTickCount64()+5000};
    rs_log("handoff launcher close requested method=WM_CLOSE cefcache_flush=normal-shutdown-requested");
    if(known) EnumWindows(rs_launcher_close_window,(LPARAM)&close);
    rs_launcher_release(&processes);
    DWORD wait=rs_launcher_wait_all(5000);
    if(wait==WAIT_TIMEOUT) {
        close.end_session=1;close.deadline=GetTickCount64()+5000;
        if(rs_launcher_snapshot(&processes)) EnumWindows(rs_launcher_close_window,(LPARAM)&close);
        rs_launcher_release(&processes);
        rs_logf("handoff launcher end-session windows=%u vetoed=%u",close.windows,close.vetoed);
        wait=rs_launcher_wait_all(5000);
    }
    if(wait!=WAIT_OBJECT_0) {
        rs_log("handoff launcher close timeout forced=1 cefcache_flushed=unknown");
        if(rs_launcher_snapshot(&processes)) {
            for(unsigned i=0;i<processes.count;i++) TerminateProcess(processes.handles[i+1],ERROR_TIMEOUT);
            rs_launcher_release(&processes);
        }
        /* Native supervision can still force/reap inaccessible or relayed
         * children. Its successful drain, not these Windows handles, gates
         * LoadExec. Cache persistence after forced shutdown is unknown. */
        rs_launcher_wait_all(5000);
    } else rs_log("handoff launcher closed forced=0 cefcache_flush=normal-shutdown-requested");
    *exit=0;return 1;
}

/* Genuine Windows elevation, not a fabricated TokenElevation answer. Wine's
 * runas implementation supplies the linked full admin token to this child. */
static int rs_launch_admin(const WCHAR *path,const WCHAR *arguments,const WCHAR *cwd,DWORD *exit)
{
    HANDLE handoff=CreateEventW(NULL,TRUE,FALSE,L"RuneScapeOsrsHandoff");
    if(!handoff) return 0;
    if(!ResetEvent(handoff)) { CloseHandle(handoff);return 0; }
    SHELLEXECUTEINFOW info={0};info.cbSize=sizeof(info);
    info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
    info.lpVerb=L"runas";info.lpFile=path;info.lpParameters=arguments;
    info.lpDirectory=cwd;info.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&info) || !info.hProcess) { CloseHandle(handoff);return 0; }
    int result=rs_wait_launcher(info.hProcess,handoff,exit);
    CloseHandle(info.hProcess);CloseHandle(handoff);return result;
}
static int rs_redist_installed(const struct rs_redist *redist)
{
    int ready=1;
    for(unsigned component=0;component<2;component++) {
        int found=0;char product[39],version[64];
        for(DWORD index=0;;index++) {
            UINT status=MsiEnumRelatedProductsA(redist->upgrade[component],0,index,product);
            rs_logf("prerequisite msi-enum arch=%s component=%u upgrade=%s index=%lu status=%u product=%s",
                    redist->arch,component,redist->upgrade[component],index,status,status ? "-" : product);
            if(status!=ERROR_SUCCESS) break;
            INSTALLSTATE state=MsiQueryProductStateA(product);
            DWORD length=sizeof(version);version[0]=0;
            status=MsiGetProductInfoA(product,"VersionString",version,&length);
            unsigned major,minor,build;char extra;
            int sufficient=status==ERROR_SUCCESS && sscanf(version,"%u.%u.%u%c",&major,&minor,&build,&extra)==3 &&
                (major>14 || (major==14 && (minor>44 || (minor==44 && build>=35211))));
            rs_logf("prerequisite msi-product arch=%s component=%u product=%s state=%d version_status=%u version=%s length=%lu sufficient=%d",
                    redist->arch,component,product,state,status,status ? "-" : version,length,sufficient);
            if(state==INSTALLSTATE_DEFAULT && sufficient) { found=1;break; }
        }
        rs_logf("prerequisite msi-component arch=%s component=%u ready=%d",redist->arch,component,found);
        if(!found) ready=0;
    }
    return ready;
}
static int rs_bundle_registered(const struct rs_redist *redist)
{
    char path[192];HKEY key;
    snprintf(path,sizeof(path),"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s",redist->bundle);
    LONG error=RegOpenKeyExA(HKEY_LOCAL_MACHINE,path,0,KEY_READ|KEY_WOW64_32KEY,&key);
    rs_logf("prerequisite bundle arch=%s bundle=%s view=32 status=%ld",redist->arch,redist->bundle,error);
    if(error==ERROR_FILE_NOT_FOUND) return 0;
    if(error) return -1;
    RegCloseKey(key);return 1;
}
static int rs_run_redist(const struct rs_redist *redist,int remove)
{
    WCHAR name[64],path[128],arguments[256];char digest[65];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,redist->name,-1,name,64) ||
       swprintf(path,128,RS_APP L"%ls",name)<0 ||
       swprintf(arguments,256,L"/%ls /quiet /norestart /log \"" RS_APP L"VC_redist.%hs.%ls.log\"",
                remove ? L"uninstall" : L"install",redist->arch,remove ? L"remove" : L"install")<0) return 0;
    HANDLE lock=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(lock==INVALID_HANDLE_VALUE) {
        rs_logf("prerequisite pin arch=%s open=0 error=%lu",redist->arch,GetLastError());return 0;
    }
    LARGE_INTEGER size={0};
    int sized=GetFileSizeEx(lock,&size);DWORD size_error=sized ? 0 : GetLastError();
    int hashed=sized && size.QuadPart==redist->size && rs_hash(lock,digest);
    int verified=hashed && !strcmp(digest,redist->sha256);
    rs_logf("prerequisite pin arch=%s open=1 size_status=%d size_error=%lu bytes=%llu hash_status=%d digest_match=%d",
            redist->arch,sized,size_error,(unsigned long long)size.QuadPart,hashed,verified);
    DWORD exit=ERROR_GEN_FAILURE;
    rs_logf("prerequisite begin arch=%s pinned=%d launcher_children=0 operation=%s",
            redist->arch,verified,remove ? "uninstall" : "install");
    int completed=verified && rs_launch(path,arguments,RS_APP,600000,&exit);
    CloseHandle(lock);
    /* Report actual MSI state even if waiting/exit failed. Never confuse the
     * postcondition with the combined launch + exit + readiness decision. */
    int installed=remove ? rs_bundle_registered(redist)==0 : rs_redist_installed(redist);
    int ready=completed && (exit==0 || exit==1638 || exit==3010) && installed;
    rs_logf("prerequisite end arch=%s exit=%lu completed=%d %s=%d ready=%d",
            redist->arch,exit,completed,remove ? "removed" : "installed",installed,ready);
    return ready;
}
static int rs_prepare_redists(void)
{
    for(unsigned i=0;i<sizeof(rs_redists)/sizeof(rs_redists[0]);i++) {
        const struct rs_redist *redist=&rs_redists[i];
        if(rs_redist_installed(redist)) {
            rs_logf("prerequisite ready arch=%s source=msi minimum=14.44.35211",redist->arch);
            continue;
        }
        /* New Burn's related-bundle cleanup would otherwise overlap two
         * three-process chains. Remove the real registered old bundle first. */
        int legacy=rs_bundle_registered(&rs_legacy_redists[i]);
        if(legacy<0 || (legacy && !rs_run_redist(&rs_legacy_redists[i],1))) return 0;
        if(!rs_run_redist(redist,0)) return 0;
    }
    return 1;
}
static int rs_reg_string(HKEY key,const WCHAR *name,const WCHAR *value)
{ return RegSetValueExW(key,name,0,REG_SZ,(const BYTE *)value,(DWORD)((wcslen(value)+1)*sizeof(WCHAR)))==ERROR_SUCCESS; }
static int rs_registry(const char *version)
{
    HKEY key;DWORD disposition;WCHAR wide_version[64];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,version,-1,wide_version,64)) return 0;
    if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Classes\\jagex",0,NULL,0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,NULL,&key,&disposition)) return 0;
    int ok=rs_reg_string(key,L"URL Protocol",L"Jagex Launcher");RegCloseKey(key);if(!ok) return 0;
    if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Classes\\jagex\\shell\\open\\command",0,NULL,0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,NULL,&key,&disposition)) return 0;
    ok=rs_reg_string(key,L"",L"\"" RS_LAUNCHER L"\" --config=\"%1\"");RegCloseKey(key);if(!ok) return 0;
    static const WCHAR uninstall[]=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Jagex Launcher";
    if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,uninstall,0,NULL,0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,NULL,&key,&disposition)) return 0;
    DWORD no_modify=1,no_repair=0;
    ok=rs_reg_string(key,L"DisplayName",L"Jagex Launcher") && rs_reg_string(key,L"DisplayIcon",L"\"" RS_LAUNCHER L"\"") &&
       rs_reg_string(key,L"DisplayVersion",wide_version) && rs_reg_string(key,L"InstallLocation",RS_INSTALL) &&
       rs_reg_string(key,L"InstallationMode",L"all-users") && rs_reg_string(key,L"Publisher",L"Jagex Ltd") &&
       rs_reg_string(key,L"InstallSource",RS_INSTALLER) &&
       rs_reg_string(key,L"UninstallString",L"\"" RS_INSTALL L"\\installer.exe\" --action=modify") &&
       !RegSetValueExW(key,L"NoModify",0,REG_DWORD,(BYTE *)&no_modify,sizeof(no_modify)) &&
       !RegSetValueExW(key,L"NoRepair",0,REG_DWORD,(BYTE *)&no_repair,sizeof(no_repair)) && !RegFlushKey(key);
    RegCloseKey(key);if(!ok) return 0;
    /* The old 32-bit view is removed only after the 64-bit replacement exists. */
    HKEY old;
    LONG error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",0,KEY_READ|KEY_WRITE|DELETE|KEY_WOW64_32KEY,&old);
    if(!error) {
        error=RegDeleteTreeW(old,L"Jagex Launcher");
        if(!error) error=RegFlushKey(old);
        RegCloseKey(old);
    }
    return !error || error==ERROR_FILE_NOT_FOUND;
}
static int rs_read_manifest(const WCHAR *path,struct rs_manifest *m)
{
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) return 0;
    char *text=malloc(RS_JSON_MAX+1);struct rs_json *j=calloc(1,sizeof(*j));DWORD bytes=0;int valid=0;
    LARGE_INTEGER size;
    if(text && j && GetFileSizeEx(file,&size) && size.QuadPart>0 && size.QuadPart<=RS_JSON_MAX &&
       ReadFile(file,text,(DWORD)size.QuadPart,&bytes,NULL) && bytes==(DWORD)size.QuadPart && !rs_parse(j,text,bytes)) {
        char id[65];
        if(!rs_string(j,rs_field(j,0,"id"),id,sizeof(id)) && !rs_manifest_parse(j,m,id)) valid=1;
    }
    free(j);free(text);CloseHandle(file);return valid;
}
/* Restart-safe publication: a complete staged directory is atomically renamed
 * into a previously absent destination. An incomplete final tree is never used.
 * Registry/metafile failures leave no completion marker and can be retried. */
static int rs_plain_path(WCHAR *path)
{
    for(WCHAR *p=path+3;;p++) {
        if(*p && *p!=L'\\' && *p!=L'/') continue;
        WCHAR saved=*p;*p=0;DWORD attr=GetFileAttributesW(path);*p=saved;
        if(attr==INVALID_FILE_ATTRIBUTES || (attr&FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
        if(!saved) return 1;
    }
}
static int rs_tree(const WCHAR *root,const struct rs_manifest *m,int hashes)
{
    WCHAR path[1024],name[256];
    unsigned char lengths[RS_PIECES*4],buffer[65536],actual[32];DWORD got=0;
    LARGE_INTEGER size;int ok=1;
    /* Official-installer diagnosis has no private piece-length receipt. It
     * still validates the published metafile and every regular file's size. */
    if(hashes>=0) {
        if(swprintf(path,1024,L"%ls\\.ssn-lengths",root)<0 || !rs_plain_path(path)) return 0;
        HANDLE sizes=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        if(sizes==INVALID_HANDLE_VALUE) return 0;
        ok=GetFileSizeEx(sizes,&size) && size.QuadPart==m->pieces*4 &&
            ReadFile(sizes,lengths,m->pieces*4,&got,NULL) && got==m->pieces*4;
        CloseHandle(sizes);if(!ok) return 0;
        uint64_t piece_bytes=0;
        for(unsigned i=0;i<m->pieces;i++) {
            uint32_t n=(uint32_t)lengths[i*4]|(uint32_t)lengths[i*4+1]<<8|
                (uint32_t)lengths[i*4+2]<<16|(uint32_t)lengths[i*4+3]<<24;
            if(!n || n>RS_PIECE_MAX || n>m->bytes-piece_bytes) return 0;
            piece_bytes+=n;
        }
        if(piece_bytes!=m->bytes) return 0;
    }
    uint64_t total=0;unsigned piece=0;uint32_t left=0;struct sha256_context hash;
    for(unsigned f=0;f<m->files;f++) {
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,m->file[f].name,-1,name,256) ||
           swprintf(path,1024,L"%ls\\%ls",root,name)<0 || !rs_plain_path(path)) return 0;
        HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(file==INVALID_HANDLE_VALUE) return 0;
        BY_HANDLE_FILE_INFORMATION info;
        ok=GetFileInformationByHandle(file,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) &&
            GetFileSizeEx(file,&size) && (uint64_t)size.QuadPart==m->file[f].size;
        uint64_t read_bytes=0;
        while(ok && hashes>0 && read_bytes<m->file[f].size) {
            if(!left) {
                if(piece>=m->pieces) { ok=0;break; }
                left=(uint32_t)lengths[piece*4]|(uint32_t)lengths[piece*4+1]<<8|
                    (uint32_t)lengths[piece*4+2]<<16|(uint32_t)lengths[piece*4+3]<<24;
                sha256_init(&hash);
            }
            DWORD take=sizeof(buffer);
            if(take>left) take=left;
            if(take>m->file[f].size-read_bytes) take=(DWORD)(m->file[f].size-read_bytes);
            if(!ReadFile(file,buffer,take,&got,NULL) || got!=take) { ok=0;break; }
            sha256_update(&hash,buffer,got);left-=got;read_bytes+=got;total+=got;
            if(!left) {
                char digest[65];sha256_final(&hash,actual);rs_hex(actual,32,digest);
                if(strcmp(digest,m->digest[piece++])) ok=0;
            }
        }
        CloseHandle(file);if(!ok) return 0;
    }
    return hashes<=0 || (total==m->bytes && !left && piece==m->pieces);
}
static int rs_committed(const char *id)
{
    HANDLE file=CreateFileW(RS_INSTALL L"\\.runescape-installed",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) return 0;
    char marker[65];DWORD got=0;int ok=ReadFile(file,marker,sizeof(marker),&got,NULL) && got==64 && !memcmp(marker,id,64);
    CloseHandle(file);return ok;
}
static int rs_publish(struct rs_manifest *m)
{
    DWORD attr=GetFileAttributesW(RS_INSTALL);
    if(attr==INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(L"C:\\Program Files",NULL);
        if(!MoveFileExW(RS_STAGE_DOS,RS_INSTALL,MOVEFILE_WRITE_THROUGH)) return 0;
    }
    if(!rs_tree(RS_INSTALL,m,1)) return 0;
    HANDLE launcher=rs_verify(RS_LAUNCHER,"launcher");if(launcher==INVALID_HANDLE_VALUE) return 0;
    int ok=CopyFileW(RS_INSTALLER,RS_INSTALL L"\\installer.exe",FALSE);
    CreateDirectoryW(L"C:\\ProgramData",NULL);CreateDirectoryW(L"C:\\ProgramData\\Jagex Launcher",NULL);
    if(ok) ok=CopyFileW(RS_INSTALL L"\\.ssn-metafile.json",RS_META L".part",FALSE) &&
        MoveFileExW(RS_META L".part",RS_META,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(ok) { char version[64];rs_version(RS_LAUNCHER,version,NULL);ok=strcmp(version,"unavailable") && rs_registry(version); }
    if(ok) {
        HANDLE marker=CreateFileW(RS_INSTALL L"\\.runescape-installed.part",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        DWORD written=0;ok=marker!=INVALID_HANDLE_VALUE;
        if(ok) { ok=WriteFile(marker,m->id,64,&written,NULL)&&written==64&&FlushFileBuffers(marker);CloseHandle(marker); }
        if(ok) ok=MoveFileExW(RS_INSTALL L"\\.runescape-installed.part",RS_INSTALL L"\\.runescape-installed",MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    }
    CloseHandle(launcher);
    if(ok) rs_logf("install complete method=ssn id=%s files=%u bytes=%llu",m->id,m->files,(unsigned long long)m->bytes);
    return ok;
}
/* The launcher gives its window 20 s (hard-coded) and exits with
 * "Timeout waiting for window to load" when a cold cefcache is too slow. The
 * restarted launcher finds the cache warm. Detect that from the launcher's own
 * log, only for text written during this attempt, and restart a bounded number
 * of times through the same elevated start path. */
static ULONGLONG rs_launcher_log_size(const WCHAR *path)
{
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size={0};
    if(!GetFileSizeEx(file,&size)) size.QuadPart=0;
    CloseHandle(file);return (ULONGLONG)size.QuadPart;
}
static int rs_launcher_log_timeout(const WCHAR *path,ULONGLONG from,ULONGLONG *end)
{
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size={0},at;
    int found=0;
    if(end) *end=0;
    if(GetFileSizeEx(file,&size) && end) *end=(ULONGLONG)size.QuadPart;
    if(size.QuadPart>=0 && (ULONGLONG)size.QuadPart>from) {
        ULONGLONG want=(ULONGLONG)size.QuadPart-from;
        if(want>(8u<<20)) want=8u<<20;
        at.QuadPart=size.QuadPart-(LONGLONG)want;
        char *text=malloc((size_t)want);
        DWORD got=0;
        if(text && SetFilePointerEx(file,at,NULL,FILE_BEGIN) && ReadFile(file,text,(DWORD)want,&got,NULL))
            found=rs_launcher_log_has_timeout(text,got);
        free(text);
    }
    CloseHandle(file);return found;
}
static int rs_launcher_timed_out(const WCHAR *begin_path,ULONGLONG begin_size)
{
    WCHAR path[260];SYSTEMTIME now;GetLocalTime(&now);
    if(rs_launcher_log_timeout(begin_path,begin_size,NULL)) return 1;
    /* Local midnight during the attempt moves the launcher to a new file. */
    return rs_launcher_log_path(path,260,now.wMonth,now.wDay,now.wYear) && wcscmp(path,begin_path) &&
        rs_launcher_log_timeout(path,0,NULL);
}
/* The launcher does not always exit on that timeout: it can leave an error
 * dialog up and keep running until the user dismisses it. While it runs, poll
 * the log text it appends and, on the timeout line, terminate every launcher
 * process so the normal exit path below restarts it. */
struct rs_launcher_watch {
    WCHAR path[260];
    ULONGLONG from;
    HANDLE stop;
    int killed;
};
static int rs_launcher_watch_poll(struct rs_launcher_watch *watch)
{
    ULONGLONG size=0;
    if(rs_launcher_log_timeout(watch->path,watch->from,&size)) {
        struct rs_launcher_processes processes={0};
        unsigned count=0;
        if(rs_launcher_snapshot(&processes)) {
            for(unsigned i=0;i<processes.count;i++) if(TerminateProcess(processes.handles[i+1],ERROR_TIMEOUT)) count++;
            rs_launcher_release(&processes);
        }
        rs_logf("launcher window timeout; launcher still running, terminated processes=%u",count);
        watch->killed=1;return 1;
    }
    /* Rescan a small tail so a line split across two polls is still seen. */
    if(size>watch->from+1024) watch->from=size-1024;
    return 0;
}
static DWORD WINAPI rs_launcher_watch_thread(void *parameter)
{
    struct rs_launcher_watch *watch=parameter;
    while(WaitForSingleObject(watch->stop,1000)==WAIT_TIMEOUT)
        if(rs_launcher_watch_poll(watch)) break;
    return 0;
}
static int rs_launch_launcher(const WCHAR *path,const WCHAR *arguments,const WCHAR *cwd,DWORD *exit)
{
    for(unsigned restarts=0;;restarts++) {
        WCHAR log[260]={0};SYSTEMTIME start;GetLocalTime(&start);
        ULONGLONG begin=rs_launcher_log_path(log,260,start.wMonth,start.wDay,start.wYear) ? rs_launcher_log_size(log) : 0;
        if(!log[0]) rs_log("launcher log path unavailable; window-timeout recovery disabled");
        struct rs_launcher_watch watch={.from=begin};
        HANDLE thread=NULL;
        if(log[0]) {
            wcscpy(watch.path,log);
            watch.stop=CreateEventW(NULL,TRUE,FALSE,NULL);
            if(watch.stop) thread=CreateThread(NULL,0,rs_launcher_watch_thread,&watch,0,NULL);
            if(!thread) rs_log("launcher log watcher unavailable; only the exit path is checked");
        }
        int launched=rs_launch_admin(path,arguments,cwd,exit);
        if(thread) { SetEvent(watch.stop);WaitForSingleObject(thread,INFINITE);CloseHandle(thread); }
        if(watch.stop) CloseHandle(watch.stop);
        if(!launched) return 0;
        Sleep(500); /* let the exiting launcher flush its log */
        if(!log[0] || !rs_launcher_timed_out(log,begin)) return 1;
        if(!rs_launcher_retry_allowed(restarts)) {
            rs_logf("launcher window timeout; retries exhausted attempts=%u",restarts+1);
            return 1;
        }
        rs_logf("launcher window timeout; restarting attempt=%u",restarts+1);
        Sleep(1000);
    }
}
int wmain(int argc,WCHAR **argv)
{
    (void)argv;if(argc!=1) return rs_fail(L"Unexpected bootstrap arguments.");
    WCHAR mode[32];if(!GetEnvironmentVariableW(L"RUNESCAPE_STARTUP_MODE",mode,32)) return rs_fail(L"Missing startup mode.");
    HANDLE installer=rs_verify(RS_INSTALLER,"installer");
    if(installer==INVALID_HANDLE_VALUE) return rs_fail(L"Installer publisher/chain verification failed. No Jagex code was executed. Close the title and retry after correcting console time or network access.");
    if(!wcscmp(mode,L"installer-trial")) {
        DWORD exit=ERROR_GEN_FAILURE;rs_log("installer begin silent=1 install=1");
        int completed=rs_launch(RS_INSTALLER,RS_INSTALLER_ARGUMENTS,L"C:\\",120000,&exit);
        HANDLE installed=rs_verify(RS_LAUNCHER,"launcher");
        struct rs_manifest *m=calloc(1,sizeof(*m));
        int complete=installed!=INVALID_HANDLE_VALUE && m && rs_read_manifest(RS_META,m) && rs_tree(RS_INSTALL,m,-1);
        rs_logf("installer end status=%lu installed=%d completed=%d",exit,complete,completed);
        free(m);
        if(installed!=INVALID_HANDLE_VALUE) CloseHandle(installed);
        CloseHandle(installer);return completed && !exit && complete ? 0 : 1;
    }
    if(!wcscmp(mode,L"ssn") || !wcscmp(mode,L"launcher-trial")) {
        struct rs_manifest *m=calloc(1,sizeof(*m));
        int existing=wcscmp(mode,L"launcher-trial") && GetFileAttributesW(RS_INSTALL)!=INVALID_FILE_ATTRIBUTES;
        const WCHAR *root=existing ? RS_INSTALL : RS_STAGE_DOS;
        const WCHAR *metadata=existing ? RS_INSTALL L"\\.ssn-metafile.json" : RS_STAGE_DOS L"\\.ssn-metafile.json";
        if(!m || !rs_read_manifest(metadata,m)) {
            free(m);CloseHandle(installer);return rs_fail(L"Missing or invalid SSN manifest.");
        }
        int installed=existing && rs_committed(m->id);
        if(!rs_tree(root,m,!installed)) {
            free(m);CloseHandle(installer);return rs_fail(L"SSN tree is incomplete or its manifest/pieces do not agree.");
        }
        if(!wcscmp(mode,L"launcher-trial")) {
            const WCHAR *trial_path=existing ? RS_LAUNCHER : RS_STAGE_DOS L"\\JagexLauncher.exe";
            HANDLE trial=rs_verify(trial_path,"launcher");
            free(m);
            if(trial==INVALID_HANDLE_VALUE) { CloseHandle(installer);return rs_fail(L"Trial launcher signature rejected."); }
            rs_log("launcher begin role=maintenance trial=1 backend=inherited");
            DWORD exit=1;int completed=rs_launch(trial_path,RS_BROWSER_ARGUMENTS,root,120000,&exit);
            CloseHandle(trial);CloseHandle(installer);return completed ? (int)exit : 1;
        }
        if(!installed) {
            HANDLE stage=rs_verify(existing ? RS_LAUNCHER : RS_STAGE_DOS L"\\JagexLauncher.exe","launcher");
            if(stage==INVALID_HANDLE_VALUE) { free(m);CloseHandle(installer);return rs_fail(L"Staged launcher signature rejected."); }
            CloseHandle(stage);
            if(!rs_publish(m)) { free(m);CloseHandle(installer);return rs_fail(L"Jagex publication/registration failed. Installation remains uncommitted; close the title and retry."); }
        }
        free(m);
    } else { CloseHandle(installer);return rs_fail(L"Unknown startup mode."); }
    HANDLE launcher=rs_verify(RS_LAUNCHER,"launcher");
    if(launcher==INVALID_HANDLE_VALUE) { CloseHandle(installer);return rs_fail(L"Installed launcher publisher verification failed."); }
    if(!rs_prepare_redists()) { CloseHandle(launcher);CloseHandle(installer);return rs_fail(L"Microsoft Visual C++ prerequisite installation did not complete. No launcher was started. See C:\\RuneScape\\VC_redist.*.install.log (or *.remove.log), close the title and retry."); }
    rs_log("launcher begin role=maintenance backend=inherited elevation=runas prerequisites=ready");
    DWORD exit=1;int completed=rs_launch_launcher(RS_LAUNCHER,RS_BROWSER_ARGUMENTS,RS_INSTALL,&exit);
    CloseHandle(launcher);CloseHandle(installer);return completed ? (int)exit : 1;
}
