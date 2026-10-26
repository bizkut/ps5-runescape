/* Native sceHttp2 acquisition and bounded reconstruction. Publication and
 * registry writes belong to the immutable PE helper, after publisher checks. */
#ifndef RUNESCAPE_INSTALL_H
#define RUNESCAPE_INSTALL_H
#include "runescape-ssn.h"
#include "runescape-piece.h"
#include "runescape-redist.h"
#include <dirent.h>
#define RS_STAGE PW_RUNTIME_INSTALLER_CWD "/Jagex Launcher.stage"
#define RS_DOWNLOAD PW_RUNTIME_INSTALLER_CWD "/.jagex-download"
static int rs_remove_stage(const char *path, unsigned depth)
{
    struct stat st;
    if (depth > 8) return EINVAL;
    /* The PS5 title sandbox rejects lstat with EPERM. Decide "real directory"
     * with a no-follow directory open, so a symlink (e.g. one a Windows
     * program created in the stage) is unlinked itself, never traversed. */
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        if (errno != ENOTDIR && errno != ELOOP) return errno;
        return unlink(path) && errno != ENOENT ? errno : 0;
    }
    int is_dir = !fstat(fd,&st) && S_ISDIR(st.st_mode); close(fd);
    if (!is_dir) return ENOTDIR;
    /* opendir keeps the existing import surface (fdopendir would add one). */
    DIR *dir = opendir(path); if (!dir) return errno;
    int result = 0; struct dirent *entry;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char child[1024];
        if (snprintf(child,sizeof(child),"%s/%s",path,entry->d_name) >= (int)sizeof(child)) { result=ENAMETOOLONG; break; }
        result = rs_remove_stage(child,depth+1);
    }
    closedir(dir); if (!result && rmdir(path)) result=errno; return result;
}
static int rs_mkdirs(char *path)
{
    for (char *p = path+1;; p++) {
        if (*p && *p != '/') continue;
        char saved=*p; *p=0; struct stat st;
        if (mkdir(path,0777) && errno != EEXIST) { *p=saved; return errno; }
        if (stat(path,&st) || !S_ISDIR(st.st_mode)) { *p=saved; return EINVAL; }
        *p=saved; if (!saved) return 0;
    }
}
#include <pthread.h>
#include <time.h>
static uint64_t rs_now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
struct rs_hop { uint64_t t0,ttfb; };
static void rs_hop_note(void *opaque,int hop,int status,const char *host,uint64_t bytes)
{
    struct rs_hop *h=opaque;
    if(h && !h->ttfb) h->ttfb=rs_now()-h->t0; /* connect+TLS+request+first response headers */
    b1_download_hop(NULL,hop,status,host,bytes);
}
static int rs_fetch(int tmpl, const char *url, const char *destination, uint64_t cap, uint64_t *ttfb)
{
    char part[1024]; const char *step="download"; uint64_t total,length;
    if (snprintf(part,sizeof(part),"%s.part",destination) >= (int)sizeof(part)) return ENAMETOOLONG;
    struct rs_hop hop={.t0=rs_now()};
    struct forever_download request = {.url=url,.part=part,.destination=destination,.cap=cap,
        .authority="jagex.akamaized.net",.path_prefix="/direct6/launcher-win-x86_64/",.hop=rs_hop_note,.opaque=&hop};
    int result=forever_download_run(tmpl,&request,&step,&total,&length);
    if (ttfb) *ttfb=hop.ttfb;
    if (result) io_log("runescape: acquisition failed step=%s status=%d",step,result);
    return result;
}
static int rs_read(const char *path, unsigned char *buffer, size_t cap, size_t *bytes)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW); if (fd<0) return errno;
    *bytes=0; int result=0;
    while (*bytes<cap) {
        ssize_t n=read(fd,buffer+*bytes,cap-*bytes);
        if (n<0 && errno==EINTR) continue;
        if (n<0) { result=errno; break; }
        if (!n) break;
        *bytes+=(size_t)n;
    }
    unsigned char extra;
    if (!result && *bytes==cap && read(fd,&extra,1)!=0) result=EFBIG;
    close(fd); return result;
}
static int rs_write_all(int fd,const unsigned char *data,size_t bytes)
{
    while(bytes) { ssize_t n=write(fd,data,bytes); if(n<0 && errno==EINTR) continue;
        if(n<=0) return n<0 ? errno : EIO;
        data+=n; bytes-=(size_t)n; }
    return 0;
}
/* Keep the official bundles outside the immutable app so updates don't ship
 * Microsoft binaries. The PE gate rechecks their pinned bytes before execution. */
static int rs_acquire_redists(void)
{
    struct forever_https http={-1,-1,-1,-1};
    const char *step="creating prerequisite directory";
    char directory[]=PW_RUNTIME_INSTALLER_CWD "/RuneScape";
    int result=rs_mkdirs(directory);
    for(unsigned i=0;!result && i<4;i++) {
        const struct rs_redist *redist=i<2 ? &rs_redists[i] : &rs_legacy_redists[i-2];
        char path[1024],part[1024];struct stat st;
        if(i>=2) {
            snprintf(path,sizeof(path),PW_RUNTIME_INSTALLER_CWD "/ProgramData/Package Cache/%s",redist->bundle);
            if(stat(path,&st)) {
                if(errno==ENOENT) continue;
                result=errno;break;
            }
        }
        snprintf(path,sizeof(path),"%s/%s",directory,redist->name);
        if(!stat(path,&st) && S_ISREG(st.st_mode) && st.st_size==redist->size) continue;
        if(http.tmpl<0 && (result=forever_https_open(&http,&step))) break;
        snprintf(part,sizeof(part),"%s/.%s.part",directory,redist->name);
        b1_message("Downloading official Microsoft Visual C++ prerequisites...");
        struct forever_download request={.url=redist->url,.part=part,.destination=path,
            .size=redist->size,.cap=redist->size,.sha256=redist->sha256,.mz=1,
            .authority="download.visualstudio.microsoft.com",.path_prefix="/download/pr/",
            .hop=b1_download_hop};
        uint64_t total=0,length=0;
        result=forever_download_run(http.tmpl,&request,&step,&total,&length);
        unlink(part);
        if(!result) io_log("runescape: prerequisite acquired arch=%s bytes=%llu sha256=%s",
                          redist->arch,(unsigned long long)total,redist->sha256);
    }
    forever_https_close(&http);
    if(result) {
        io_log("runescape: prerequisite acquisition failed status=%d step=%s",result,step);
        b1_message("Microsoft prerequisite download failed. Close the title and retry.");
    }
    return result;
}
/* Piece pipeline. RS_WORKERS workers each own an HTTPS context (own template
 * and keep-alive connection) and fetch+gunzip+verify whole pieces into a ring
 * of RS_WORKERS decoded slots. A worker may claim piece p only once piece
 * p-RS_WORKERS has been consumed, so the ring never overwrites unconsumed data.
 * The installing thread writes pieces strictly in manifest order, so file
 * layout, piece lengths and every digest check are unchanged.
 * Memory bound: RS_WORKERS*(RS_PIECE_MAX decoded + RS_PIECE_MAX+64 KiB wire)
 * = 5*(4+4.06) MiB ~= 40 MiB, independent of piece count. */
#define RS_WORKERS 5
#define RS_TIMING_EVERY 16
struct rs_time { uint64_t ttfb,fetch,read,unpack,digest,create,write,sync,stall,wire_bytes,decoded_bytes; };
struct rs_slot { unsigned char *decoded; unsigned piece; int ready,result; const char *step; size_t wire,length; };
struct rs_pipe;
struct rs_worker { struct rs_pipe *pipe; unsigned id; struct forever_https http; unsigned char *wire; pthread_t thread; int started; };
struct rs_pipe {
    pthread_mutex_t mutex; pthread_cond_t cond;
    unsigned next,consumed,pieces,workers; int abort,failed,started; uint64_t t0;
    const struct rs_manifest *manifest;
    struct rs_slot slot[RS_WORKERS]; struct rs_worker worker[RS_WORKERS]; struct rs_time time;
};
static void rs_timing_log(const char *label,struct rs_pipe *pipe,unsigned pieces)
{
    pthread_mutex_lock(&pipe->mutex); struct rs_time t=pipe->time; pthread_mutex_unlock(&pipe->mutex);
    io_log("runescape: ssn timing %s pieces=%u/%u workers=%u wall_ms=%llu wire_kb=%llu decoded_kb=%llu "
           "worker_ms[fetch=%llu ttfb=%llu read=%llu gunzip=%llu sha=%llu] consumer_ms[stall=%llu create=%llu write=%llu sync=%llu]",
           label,pieces,pipe->pieces,pipe->workers,(unsigned long long)((rs_now()-pipe->t0)/1000000u),
           (unsigned long long)(t.wire_bytes/1024),(unsigned long long)(t.decoded_bytes/1024),
           (unsigned long long)(t.fetch/1000000u),(unsigned long long)(t.ttfb/1000000u),
           (unsigned long long)(t.read/1000000u),(unsigned long long)(t.unpack/1000000u),
           (unsigned long long)(t.digest/1000000u),(unsigned long long)(t.stall/1000000u),
           (unsigned long long)(t.create/1000000u),(unsigned long long)(t.write/1000000u),
           (unsigned long long)(t.sync/1000000u));
}
static void *rs_worker_run(void *argument)
{
    struct rs_worker *worker=argument; struct rs_pipe *pipe=worker->pipe; char destination[1024];
    snprintf(destination,sizeof(destination),"%s.%u",RS_DOWNLOAD,worker->id);
    for(;;) {
        pthread_mutex_lock(&pipe->mutex);
        while(!pipe->abort && !pipe->failed && pipe->next<pipe->pieces && pipe->next>=pipe->consumed+RS_WORKERS)
            pthread_cond_wait(&pipe->cond,&pipe->mutex);
        if(pipe->abort || pipe->failed || pipe->next>=pipe->pieces) { pthread_mutex_unlock(&pipe->mutex); break; }
        unsigned piece=pipe->next++; pthread_mutex_unlock(&pipe->mutex);
        struct rs_slot *slot=&pipe->slot[piece%RS_WORKERS]; struct rs_time t={0};
        const char *step="fetching piece"; int result; size_t bytes=0,length=0; char url[512];
        const char *digest=pipe->manifest->digest[piece];
        snprintf(url,sizeof(url),RS_CDN "pieces/%.2s/%s.solidpiece",digest,digest);
        uint64_t t0=rs_now();
        result=rs_fetch(worker->http.tmpl,url,destination,RS_PIECE_MAX+65536,&t.ttfb);
        t.fetch=rs_now()-t0;
        if(!result) {
            step="reading piece"; t0=rs_now();
            result=rs_read(destination,worker->wire,RS_PIECE_MAX+65536,&bytes); t.read=rs_now()-t0;
        }
        if(!result) {
            step="unpacking piece"; t0=rs_now();
            result=rs_unpack(worker->wire,bytes,slot->decoded,&length); t.unpack=rs_now()-t0;
            if(result) length=0;
        }
        if(!result) {
            struct sha256_context hash; t0=rs_now(); sha256_init(&hash); sha256_update(&hash,slot->decoded,length);
            step="verifying piece digest"; result=forever_download_digest(&hash,digest); t.digest=rs_now()-t0;
        }
        pthread_mutex_lock(&pipe->mutex);
        slot->piece=piece; slot->result=result; slot->step=step; slot->wire=bytes; slot->length=length; slot->ready=1;
        if(result) pipe->failed=1;
        pipe->time.ttfb+=t.ttfb; pipe->time.fetch+=t.fetch; pipe->time.read+=t.read;
        pipe->time.unpack+=t.unpack; pipe->time.digest+=t.digest;
        pthread_cond_broadcast(&pipe->cond); pthread_mutex_unlock(&pipe->mutex);
        if(result) break;
    }
    unlink(destination);
    return NULL;
}
static void rs_pipe_stop(struct rs_pipe *pipe)
{
    if(!pipe->started) return;
    pthread_mutex_lock(&pipe->mutex); pipe->abort=1; pthread_cond_broadcast(&pipe->cond); pthread_mutex_unlock(&pipe->mutex);
    for(unsigned i=0;i<RS_WORKERS;i++) {
        struct rs_worker *w=&pipe->worker[i];
        if(w->started) pthread_join(w->thread,NULL);
        w->started=0; forever_https_close(&w->http); free(w->wire); w->wire=NULL;
        free(pipe->slot[i].decoded); pipe->slot[i].decoded=NULL;
    }
    pipe->started=0;
}
static int rs_pipe_start(struct rs_pipe *pipe,const struct rs_manifest *manifest)
{
    const char *step; int result=0;
    pthread_mutex_init(&pipe->mutex,NULL); pthread_cond_init(&pipe->cond,NULL);
    pipe->manifest=manifest; pipe->pieces=manifest->pieces; pipe->t0=rs_now(); pipe->started=1;
    pipe->workers=manifest->pieces<RS_WORKERS ? manifest->pieces : RS_WORKERS;
    for(unsigned i=0;i<RS_WORKERS;i++) pipe->worker[i].http=(struct forever_https){-1,-1,-1,-1};
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,1u<<20);
    for(unsigned i=0;!result && i<pipe->workers;i++) {
        struct rs_worker *w=&pipe->worker[i]; w->pipe=pipe; w->id=i;
        pipe->slot[i].decoded=malloc(RS_PIECE_MAX); w->wire=malloc(RS_PIECE_MAX+65536);
        if(!pipe->slot[i].decoded || !w->wire) { result=ENOMEM; break; }
        if((result=forever_https_open(&w->http,&step))) break;
        if((result=pthread_create(&w->thread,&attr,rs_worker_run,w))) break;
        w->started=1;
    }
    pthread_attr_destroy(&attr);
    if(result) rs_pipe_stop(pipe);
    return result;
}
static int rs_install_native(void)
{
    struct forever_https http={-1,-1,-1,-1}; const char *step="allocation";
    struct rs_json *json=calloc(1,sizeof(*json));
    struct rs_manifest *manifest=calloc(1,sizeof(*manifest));
    struct rs_pipe *pipe=calloc(1,sizeof(*pipe));
    char *text=malloc(RS_JSON_MAX+1), id[65],url[512],path[1024];
    int result=ENOMEM,fd=-1,length_fd=-1; size_t bytes=0,length=0;
    unsigned file=0,piece=0; uint64_t offset=0,total=0;
    if(!json||!manifest||!pipe||!text) goto done;
    b1_message("Downloading the current production Jagex Launcher...");
    step="removing stage";
    if((result=rs_remove_stage(RS_STAGE,0))) goto done;
    step="creating stage"; strcpy(path,RS_STAGE); if((result=rs_mkdirs(path))) goto done;
    if((result=forever_https_open(&http,&step))) goto done;
    step="fetching alias";
    if((result=rs_fetch(http.tmpl,RS_CDN "alias.json",RS_DOWNLOAD,RS_JSON_MAX,NULL))) goto done;
    step="reading alias";
    if((result=rs_read(RS_DOWNLOAD,(unsigned char *)text,RS_JSON_MAX,&bytes))) goto done;
    step="parsing alias";
    if(rs_parse(json,text,bytes)||rs_string(json,rs_field(json,0,"launcher-win-x86_64.production"),id,sizeof(id))||!rs_id(id)) { result=EINVAL;goto done; }
    snprintf(url,sizeof(url),RS_CDN "catalog/%s/catalog.json",id);
    step="fetching catalog";
    if((result=rs_fetch(http.tmpl,url,RS_DOWNLOAD,RS_JSON_MAX,NULL))) goto done;
    step="reading catalog";
    if((result=rs_read(RS_DOWNLOAD,(unsigned char *)text,RS_JSON_MAX,&bytes))) goto done;
    snprintf(url,sizeof(url),RS_CDN "metafile/%s/metafile.json",id);
    step="validating catalog identity";
    if(rs_parse(json,text,bytes)||!rs_equal(json,rs_field(json,0,"id"),id)||
       !rs_equal(json,rs_field(json,0,"metafile"),url)) { result=EINVAL;goto done; }
    int config=rs_field(json,0,"config"),remote=rs_field(json,config,"remote");
    step="validating catalog remote";
    if(!rs_equal(json,rs_field(json,remote,"baseUrl"),RS_CDN)||
       !rs_equal(json,rs_field(json,remote,"type"),"httpPiece")||
       !rs_equal(json,rs_field(json,remote,"flags"),"gzip")||
       !rs_equal(json,rs_field(json,remote,"pieceFormat"),"pieces/{SubString:0,2,{TargetDigest}}/{TargetDigest}.solidpiece")) { result=EINVAL;goto done; }
    step="fetching metafile";
    if((result=rs_fetch(http.tmpl,url,RS_STAGE "/.ssn-metafile.json",RS_JSON_MAX,NULL))) goto done;
    step="reading metafile";
    if((result=rs_read(RS_STAGE "/.ssn-metafile.json",(unsigned char *)text,RS_JSON_MAX,&bytes))) goto done;
    step="parsing manifest";
    if(rs_parse(json,text,bytes)||rs_manifest_parse(json,manifest,id)) { result=EINVAL;goto done; }
    step="creating piece lengths";
    length_fd=open(RS_STAGE "/.ssn-lengths",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0666);
    if(length_fd<0) { result=errno;goto done; }
    step="starting piece workers";
    if((result=rs_pipe_start(pipe,manifest))) goto done;
    for(piece=0;piece<manifest->pieces;piece++) {
        bytes=length=0;
        struct rs_slot *slot=&pipe->slot[piece%RS_WORKERS];
        uint64_t wait_start=rs_now();
        pthread_mutex_lock(&pipe->mutex);
        while(!(slot->ready && slot->piece==piece)) pthread_cond_wait(&pipe->cond,&pipe->mutex);
        pipe->time.stall+=rs_now()-wait_start;
        pthread_mutex_unlock(&pipe->mutex);
        bytes=slot->wire; length=slot->length;
        if(slot->result) { step=slot->step; result=slot->result; goto done; }
        unsigned char encoded_length[4]={(unsigned char)length,(unsigned char)(length>>8),
            (unsigned char)(length>>16),(unsigned char)(length>>24)};
        step="writing piece length";
        if((result=rs_write_all(length_fd,encoded_length,sizeof(encoded_length)))) goto done;
        step="checking remaining manifest bytes";
        if(length>manifest->bytes-total) { result=EINVAL;goto done; }
        size_t at=0; uint64_t t_create=0,t_write=0,t_sync=0,t0;
        while(at<length || (file<manifest->files && !manifest->file[file].size)) {
            step="checking file span";
            if(file>=manifest->files) { result=EINVAL;goto done; }
            struct rs_file *f=&manifest->file[file];
            if(fd<0) {
                snprintf(path,sizeof(path),RS_STAGE "/%s",f->name);
                char *slash=strrchr(path,'/'); *slash=0;
                t0=rs_now();
                step="creating file parents";
                if((result=rs_mkdirs(path))) goto done;
                step="creating file"; *slash='/'; fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0666);
                if(fd<0) { result=errno;goto done; }
                t_create+=rs_now()-t0;
            }
            uint64_t remaining=f->size-offset; size_t take=length-at;
            if(take>remaining) take=(size_t)remaining;
            step="writing file span"; t0=rs_now();
            if((result=rs_write_all(fd,slot->decoded+at,take))) goto done;
            t_write+=rs_now()-t0;
            offset+=take; at+=take; total+=take;
            if(offset==f->size) {
                step="syncing file"; t0=rs_now();
                if(fsync(fd)) { result=errno;goto done; }
                step="closing file";
                if(close(fd)) { fd=-1;result=errno;goto done; }
                t_sync+=rs_now()-t0;
                fd=-1;offset=0;file++;
            }
        }
        pthread_mutex_lock(&pipe->mutex);
        pipe->time.create+=t_create; pipe->time.write+=t_write; pipe->time.sync+=t_sync;
        pipe->time.wire_bytes+=bytes; pipe->time.decoded_bytes+=length;
        slot->ready=0; pipe->consumed=piece+1;
        pthread_cond_broadcast(&pipe->cond);
        pthread_mutex_unlock(&pipe->mutex);
        if(!((piece+1)%RS_TIMING_EVERY)) rs_timing_log("progress",pipe,piece+1);
        char message[160]; snprintf(message,sizeof(message),"Installing Jagex Launcher: piece %u of %u",piece+1,manifest->pieces); b1_message(message);
    }
    rs_pipe_stop(pipe);
    rs_timing_log("total",pipe,manifest->pieces);
    step="checking reconstruction totals";
    if(file!=manifest->files || offset || total!=manifest->bytes) { result=EINVAL;goto done; }
    step="syncing piece lengths";
    if(fsync(length_fd)) { result=errno;goto done; }
    step="closing piece lengths";
    if(close(length_fd)) { length_fd=-1;result=errno;goto done; }
    length_fd=-1;
    /* This marker means only reconstruction completed, never trust accepted. */
    io_log("runescape: reconstruction complete id=%s files=%u bytes=%llu",id,manifest->files,(unsigned long long)total);
    result=0;
done:
    if(fd>=0) close(fd);
    if(length_fd>=0) close(length_fd);
    if(pipe) { rs_pipe_stop(pipe); free(pipe); }
    forever_https_close(&http); unlink(RS_DOWNLOAD);
    if(result) { io_log("runescape: reconstruction failed status=%d step=%s piece_index=%u file_index=%u wire=%llu decoded=%llu file_offset=%llu total=%llu",
            result,step,piece,file,(unsigned long long)bytes,(unsigned long long)length,
            (unsigned long long)offset,(unsigned long long)total);
        b1_message("Jagex Launcher acquisition failed. Close the title and retry."); }
    free(json);free(manifest);free(text);return result;
}
#endif
