/* Production envelope/parser/reconstruction with deterministic network input.
 * No Jagex binaries, no real network; outputs stay below the test cwd. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include "forever-download.h"
#define PW_RUNTIME_INSTALLER_CWD "test-runescape-install-work"
#include "runescape-redist.h"
static const unsigned char good[]={0x6e,0x2b,0x3b,0x61,0,2,0x1f,0x8b,8,0,0,0,0,0,0,3,
    1,3,0,0xfc,0xff,'a','b','c',0xc2,0x41,0x24,0x35,3,0,0,0};
static unsigned calls;
static int corrupt_piece,wrong_size,fail_fetch,mixed_pieces,corrupt_stored,unknown_method,ordered,ordered_corrupt=-1;
static unsigned redist_calls,redist_fail,active,max_active;
static char failure[1024],timing[1024];
static pthread_mutex_t mock_mutex=PTHREAD_MUTEX_INITIALIZER;
#define ORDERED_PIECES 24u
static char ordered_hex[ORDERED_PIECES][65],ordered_b64[ORDERED_PIECES][45];
static unsigned char ordered_data[ORDERED_PIECES][3];
static void b1_message(const char *text) { (void)text; }
static void io_log(const char *format,...)
{
    char line[1024];
    va_list ap;va_start(ap,format);vsnprintf(line,sizeof(line),format,ap);va_end(ap);
    pthread_mutex_lock(&mock_mutex);
    if(strncmp(line,"runescape: ssn timing",21)) memcpy(failure,line,sizeof(failure));
    else memcpy(timing,line,sizeof(timing));
    pthread_mutex_unlock(&mock_mutex);
}
static void b1_download_hop(void *opaque,int hop,int status,const char *host,uint64_t bytes)
{ (void)opaque;(void)hop;(void)status;(void)host;(void)bytes; }
static int rs_mock_open(struct forever_https *http,const char **step)
{ *step="setting receive timeout";*http=(struct forever_https){-1,-1,-1,4};return 0; }
static void rs_mock_close(struct forever_https *http) { http->tmpl=-1; }
static int rs_mock_download(int tmpl,const struct forever_download *d,const char **step,uint64_t *total,uint64_t *length)
{
    (void)step;assert(tmpl==4);
    if(!strcmp(d->authority,"download.visualstudio.microsoft.com")) {
        unsigned index=strstr(d->url,"VC_redist.x64.exe") ? 1 : 0;
        const struct rs_redist *redist=&rs_redists[index];
        if(strcmp(d->url,redist->url)) redist=&rs_legacy_redists[index];
        assert(!strcmp(d->url,redist->url) && !strcmp(d->sha256,redist->sha256));
        assert(d->mz && d->size==redist->size && d->cap==redist->size);
        assert(!strcmp(d->path_prefix,"/download/pr/"));
        redist_calls++;
        if(redist_fail==index+1) return EIO;
        int fd=open(d->destination,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);
        assert(!ftruncate(fd,redist->size) && !close(fd));
        *total=*length=redist->size;return 0;
    }
    pthread_mutex_lock(&mock_mutex);
    unsigned my_call=++calls;
    if(++active>max_active) max_active=active;
    pthread_mutex_unlock(&mock_mutex);
    assert(!strcmp(d->authority,"jagex.akamaized.net"));
    assert(!strcmp(d->path_prefix,"/direct6/launcher-win-x86_64/"));
    if(fail_fetch && my_call==(unsigned)fail_fetch) { pthread_mutex_lock(&mock_mutex);active--;pthread_mutex_unlock(&mock_mutex);return EIO; }
    char text[4096];const unsigned char *data=(const unsigned char *)text;size_t bytes;
    const char *id="0000000000000000000000000000000000000000000000000000000000000000";
    if(strstr(d->url,"alias.json")) {
        snprintf(text,sizeof(text),"{\"launcher-win-x86_64.production\":\"%s\"}",id);bytes=strlen(text);
    } else if(strstr(d->url,"catalog/")) {
        snprintf(text,sizeof(text),"{\"config\":{\"remote\":{\"baseUrl\":\"https://jagex.akamaized.net/direct6/launcher-win-x86_64/\",\"flags\":\"gzip\",\"type\":\"httpPiece\",\"pieceFormat\":\"pieces/{SubString:0,2,{TargetDigest}}/{TargetDigest}.solidpiece\"}},\"id\":\"%s\",\"metafile\":\"https://jagex.akamaized.net/direct6/launcher-win-x86_64/metafile/%s/metafile.json\"}",id,id);bytes=strlen(text);
    } else if(strstr(d->url,"metafile/") && ordered) {
        snprintf(text,sizeof(text),"{\"algorithm\":\"sha256\",\"id\":\"%s\",\"version\":\"3.0.0\",\"files\":[{\"attr\":0,\"name\":\"big\",\"size\":%u}],\"pieces\":{\"algorithm\":\"sha256\",\"hashPadding\":false,\"digests\":[",id,ORDERED_PIECES*3);
        for(unsigned i=0;i<ORDERED_PIECES;i++) { strcat(text,i?",\"":"\"");strcat(text,ordered_b64[i]);strcat(text,"\""); }
        strcat(text,"]}}");bytes=strlen(text);
    } else if(strstr(d->url,"metafile/")) {
        if(mixed_pieces) {
            /* Eighteen gzip pieces, then two stored pieces. Piece 19 crosses
             * the dir/b -> tail boundary; the last file is empty. */
            snprintf(text,sizeof(text),"{\"algorithm\":\"sha256\",\"id\":\"%s\",\"version\":\"3.0.0\",\"pad\":[{\"offset\":3,\"size\":7}],\"files\":[{\"attr\":0,\"name\":\"a\",\"size\":1},{\"attr\":0,\"name\":\"dir/b\",\"size\":54},{\"attr\":0,\"name\":\"tail\",\"size\":5},{\"attr\":0,\"name\":\"empty\",\"size\":0}],\"pieces\":{\"algorithm\":\"sha256\",\"hashPadding\":false,\"digests\":[",id);
            for(unsigned i=0;i<18;i++) strcat(text,"\"ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=\",");
            strcat(text,"\"y4N5rCCYqhZQKeOTilHaC87PwAj9Z5X0AReGR/lsWzQ=\",\"UK5h6EH6xOj55AuvKtNuyGiSLqSDaMGPlTXkfbVt1/s=\"]}}");
            bytes=strlen(text);
        } else {
            snprintf(text,sizeof(text),"{\"algorithm\":\"sha256\",\"id\":\"%s\",\"version\":\"3.0.0\",\"files\":[{\"attr\":0,\"name\":\"a\",\"size\":1},{\"attr\":0,\"name\":\"dir/b\",\"size\":%d},{\"attr\":0,\"name\":\"empty\",\"size\":0}],\"pieces\":{\"algorithm\":\"sha256\",\"hashPadding\":false,\"digests\":[\"ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=\"]}}",id,wrong_size?3:2);bytes=strlen(text);
        }
    } else {
        const char *stored_digest[2]={"cb8379ac2098aa165029e3938a51da0bcecfc008fd6795f401178647f96c5b34","50ae61e841fac4e8f9e40baf2ad36ec868922ea48368c18f9535e47db56dd7fb"};
        int ordered_index=-1;
        if(ordered) for(unsigned i=0;i<ORDERED_PIECES;i++) if(strstr(d->url,ordered_hex[i])) ordered_index=(int)i;
        if(ordered) {
            assert(ordered_index>=0);
            /* Later pieces finish first: workers complete out of order. */
            usleep((ORDERED_PIECES-(unsigned)ordered_index)*1500u);
            memcpy(text,good,6);text[5]=0;memcpy(text+6,ordered_data[ordered_index],3);bytes=9;
            if(ordered_index==ordered_corrupt) text[6]^=1;
        } else if(mixed_pieces && (strstr(d->url,stored_digest[0]) || strstr(d->url,stored_digest[1]))) {
            unsigned second=strstr(d->url,stored_digest[1])!=NULL;
            memcpy(text,good,6);text[5]=unknown_method?1:0;
            memcpy(text+6,second?"ghi":"def",3);bytes=9;
            if(corrupt_stored && !second) text[6]^=1;
        } else {
            assert(strstr(d->url,"pieces/ba/ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad.solidpiece"));
            memcpy(text,good,sizeof(good));if(corrupt_piece) text[21]='d';bytes=sizeof(good);
        }
    }
    assert(bytes<=d->cap);FILE *file=fopen(d->destination,"wb");assert(file);
    pthread_mutex_lock(&mock_mutex);active--;pthread_mutex_unlock(&mock_mutex);
    assert(fwrite(data,1,bytes,file)==bytes);assert(!fclose(file));*total=*length=bytes;return 0;
}
#define forever_https_open rs_mock_open
#define forever_https_close rs_mock_close
#define forever_download_run rs_mock_download
#include "runescape-install.h"
#undef forever_https_open
#undef forever_https_close
#undef forever_download_run
int main(void)
{
    unsigned char wire[sizeof(good)+1];unsigned char *out=malloc(RS_PIECE_MAX);size_t bytes=0;
    assert(out);
    assert(!rs_unpack(good,sizeof(good),out,&bytes) && bytes==3 && !memcmp(out,"abc",3));
    for(size_t length=0;length<sizeof(good);length++) assert(rs_unpack(good,length,out,&bytes));
    const unsigned mutations[]={0,5,9,21,24,28};
    for(unsigned i=0;i<sizeof(mutations)/sizeof(*mutations);i++) {
        memcpy(wire,good,sizeof(good));wire[mutations[i]]^=1;assert(rs_unpack(wire,sizeof(good),out,&bytes));
    }
    memcpy(wire,good,sizeof(good));wire[sizeof(good)]=0;assert(rs_unpack(wire,sizeof(wire),out,&bytes));
    const unsigned char stored[]={0x6e,0x2b,0x3b,0x61,0,0,'a','b','c'};
    assert(!rs_unpack(stored,sizeof(stored),out,&bytes) && bytes==3 && !memcmp(out,"abc",3));
    for(size_t length=0;length<=6;length++) assert(rs_unpack(stored,length,out,&bytes));
    for(unsigned i=0;i<6;i++) {
        memcpy(wire,stored,sizeof(stored));wire[i]^=1;
        assert(rs_unpack(wire,sizeof(stored),out,&bytes));
    }
    /* Stored data has no gzip CRC, so the mandatory manifest digest is
     * exercised below. Its payload cap must still be exact. */
    unsigned char *large_stored=malloc(RS_PIECE_MAX+7);assert(large_stored);
    memcpy(large_stored,stored,6);memset(large_stored+6,'a',RS_PIECE_MAX+1);
    assert(!rs_unpack(large_stored,RS_PIECE_MAX+6,out,&bytes) && bytes==RS_PIECE_MAX);
    assert(rs_unpack(large_stored,RS_PIECE_MAX+7,out,&bytes));free(large_stored);
    /* Valid stored-block gzip at the exact decoded cap, then a one-byte bomb.
     * The transport-sized input remains bounded; no decoder allocation grows. */
    for(unsigned extra=0;extra<2;extra++) {
        size_t length=RS_PIECE_MAX+extra,blocks=(length+65534)/65535,cap=length+blocks*5+24;
        unsigned char *payload=malloc(length),*large=malloc(cap);
        assert(payload && large);memset(payload,'a',length);memcpy(large,good,16);
        size_t at=16,used=0;
        while(used<length) {
            unsigned count=(unsigned)(length-used);if(count>65535) count=65535;
            large[at++]=(unsigned char)(used+count==length);
            large[at++]=(unsigned char)count;large[at++]=(unsigned char)(count>>8);
            large[at++]=(unsigned char)~count;large[at++]=(unsigned char)(~count>>8);
            memcpy(large+at,payload+used,count);at+=count;used+=count;
        }
        uint32_t crc=rs_crc(payload,length);
        for(unsigned i=0;i<4;i++) large[at++]=(unsigned char)(crc>>(i*8));
        for(unsigned i=0;i<4;i++) large[at++]=(unsigned char)(length>>(i*8));
        assert(at==cap);
        if(extra) assert(rs_unpack(large,cap,out,&bytes));
        else assert(!rs_unpack(large,cap,out,&bytes) && bytes==RS_PIECE_MAX);
        free(payload);free(large);
    }
    free(out);
    assert(!mkdir(PW_RUNTIME_INSTALLER_CWD,0700));
    redist_fail=2;
    assert(rs_acquire_redists()==EIO && redist_calls==2);
    assert(!calls && strstr(failure,"prerequisite acquisition failed"));
    redist_fail=0;
    assert(!rs_acquire_redists() && redist_calls==3);
    assert(!rs_acquire_redists() && redist_calls==3); /* both cached, no HTTP */
    char legacy_cache[256];
    snprintf(legacy_cache,sizeof(legacy_cache),PW_RUNTIME_INSTALLER_CWD "/ProgramData/Package Cache/%s",
             rs_legacy_redists[0].bundle);
    assert(!rs_mkdirs(legacy_cache));
    assert(!rs_acquire_redists() && redist_calls==4); /* only the cached legacy architecture */
    assert(!rs_install_native() && calls==4);
    unsigned char actual[32];
    assert(!rs_read(RS_STAGE "/a",actual,sizeof(actual),&bytes) && bytes==1 && actual[0]=='a');
    assert(!rs_read(RS_STAGE "/dir/b",actual,sizeof(actual),&bytes) && bytes==2 && !memcmp(actual,"bc",2));
    assert(!rs_read(RS_STAGE "/empty",actual,sizeof(actual),&bytes) && !bytes);
    assert(!rs_read(RS_STAGE "/.ssn-lengths",actual,sizeof(actual),&bytes) && bytes==4 && rs_le32(actual)==3);
    assert(access(RS_STAGE "/.runescape-installed",F_OK)); /* native transport cannot authorize execution */
    calls=0;corrupt_piece=1;assert(rs_install_native());
    assert(strstr(failure,"step=unpacking piece piece_index=0 file_index=0"));
    assert(access(RS_STAGE "/a",F_OK));
    calls=0;corrupt_piece=0;wrong_size=1;assert(rs_install_native());
    assert(strstr(failure,"step=checking reconstruction totals"));
    calls=0;wrong_size=0;fail_fetch=2;assert(rs_install_native());
    assert(strstr(failure,"step=fetching catalog"));
    calls=0;fail_fetch=0;assert(!rs_install_native()); /* interrupted attempt is recoverable */
    calls=0;mixed_pieces=1;assert(!rs_install_native() && calls==23);
    assert(!rs_read(RS_STAGE "/a",actual,sizeof(actual),&bytes) && bytes==1 && actual[0]=='a');
    unsigned char body[64];
    assert(!rs_read(RS_STAGE "/dir/b",body,sizeof(body),&bytes) && bytes==54);
    for(unsigned i=0;i<53;i++) assert(body[i]==(unsigned char)"bca"[i%3]);
    assert(body[53]=='d');
    assert(!rs_read(RS_STAGE "/tail",actual,sizeof(actual),&bytes) && bytes==5 && !memcmp(actual,"efghi",5));
    assert(!rs_read(RS_STAGE "/empty",actual,sizeof(actual),&bytes) && !bytes);
    unsigned char lengths[80];
    assert(!rs_read(RS_STAGE "/.ssn-lengths",lengths,sizeof(lengths),&bytes) && bytes==80);
    for(unsigned i=0;i<20;i++) assert(rs_le32(lengths+i*4)==3);
    calls=0;corrupt_stored=1;assert(rs_install_native()==EINVAL && (calls==22 || calls==23));
    assert(strstr(failure,"step=verifying piece digest piece_index=18 file_index=1"));
    assert(strstr(failure,"wire=9 decoded=3 file_offset=53 total=54"));
    calls=0;corrupt_stored=0;unknown_method=1;assert(rs_install_native()==EINVAL && (calls==22 || calls==23));
    assert(strstr(failure,"step=unpacking piece piece_index=18 file_index=1"));
    unknown_method=0;mixed_pieces=0;
    /* Concurrent fetch: 24 distinct stored pieces of one file, served so later
     * pieces complete first. Output must still be in manifest order. */
    static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(unsigned i=0;i<ORDERED_PIECES;i++) {
        ordered_data[i][0]='A'+i;ordered_data[i][1]='a'+i;ordered_data[i][2]='0'+(i%10);
        struct sha256_context hash;unsigned char sum[32];sha256_init(&hash);sha256_update(&hash,ordered_data[i],3);sha256_final(&hash,sum);
        for(unsigned n=0;n<32;n++) sprintf(ordered_hex[i]+n*2,"%02x",sum[n]);
        unsigned o=0;
        for(unsigned n=0;n<30;n+=3) {
            unsigned v=(unsigned)sum[n]<<16|(unsigned)sum[n+1]<<8|sum[n+2];
            for(int s=18;s>=0;s-=6) ordered_b64[i][o++]=b64[(v>>s)&63];
        }
        unsigned v=(unsigned)sum[30]<<16|(unsigned)sum[31]<<8;
        ordered_b64[i][o++]=b64[(v>>18)&63];ordered_b64[i][o++]=b64[(v>>12)&63];ordered_b64[i][o++]=b64[(v>>6)&63];
        ordered_b64[i][o++]='=';ordered_b64[i][o]=0;
    }
    ordered=1;calls=0;max_active=0;
    assert(!rs_install_native() && calls==3+ORDERED_PIECES);
    assert(max_active>1 && max_active<=RS_WORKERS); /* really parallel, window bounded */
    unsigned char big[ORDERED_PIECES*3+1];
    assert(!rs_read(RS_STAGE "/big",big,sizeof(big),&bytes) && bytes==ORDERED_PIECES*3);
    for(unsigned i=0;i<ORDERED_PIECES;i++) assert(!memcmp(big+i*3,ordered_data[i],3));
    unsigned char ordered_lengths[ORDERED_PIECES*4+1];
    assert(!rs_read(RS_STAGE "/.ssn-lengths",ordered_lengths,sizeof(ordered_lengths),&bytes) && bytes==ORDERED_PIECES*4);
    for(unsigned i=0;i<ORDERED_PIECES;i++) assert(rs_le32(ordered_lengths+i*4)==3);
    assert(strstr(timing,"ssn timing total pieces=24/24 workers=5") && strstr(timing,"worker_ms[fetch="));
    /* A corrupt piece in the middle, at the edges: abort at exactly that piece,
     * every earlier piece already written in order, none after it. */
    const int corrupt_at[]={0,11,23};
    for(unsigned c=0;c<3;c++) {
        char expect[160];
        ordered_corrupt=corrupt_at[c];calls=0;
        assert(rs_install_native()==EINVAL && calls<=3+ORDERED_PIECES);
        snprintf(expect,sizeof(expect),"step=verifying piece digest piece_index=%d file_index=0",corrupt_at[c]);
        assert(strstr(failure,expect));
        snprintf(expect,sizeof(expect),"file_offset=%d total=%d",corrupt_at[c]*3,corrupt_at[c]*3);
        assert(strstr(failure,expect));
        assert(access(RS_STAGE "/.runescape-installed",F_OK));
    }
    /* A failed transfer mid-run aborts cleanly and reports the fetch step. */
    ordered_corrupt=-1;fail_fetch=3+13;calls=0;
    assert(rs_install_native()==EIO && strstr(failure,"step=fetching piece"));
    fail_fetch=0;ordered=0;
    /* Removing a stage symlink unlinks the link, never its outside target. */
    assert(!mkdir("test-runescape-install-target",0700));
    int target=open("test-runescape-install-target/keep",O_WRONLY|O_CREAT|O_EXCL,0600);assert(target>=0);
    assert(!rs_write_all(target,(const unsigned char *)"safe",4));assert(!close(target));
    assert(!symlink("../../test-runescape-install-target",RS_STAGE "/outside"));
    assert(!rs_remove_stage(RS_STAGE,0));
    assert(access(RS_STAGE,F_OK) && errno==ENOENT);
    assert(!rs_read("test-runescape-install-target/keep",actual,sizeof(actual),&bytes) && bytes==4 && !memcmp(actual,"safe",4));
    assert(!rs_remove_stage("test-runescape-install-target",0));
    assert(!rs_remove_stage(PW_RUNTIME_INSTALLER_CWD,0));
    puts("runescape SSN reconstruction contracts passed");return 0;
}
