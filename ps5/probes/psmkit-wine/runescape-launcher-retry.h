/* Recovery policy for the Jagex Launcher's hard-coded ~20 s window-load
 * deadline. On a cold cefcache the launcher logs
 * "ERROR - Timeout waiting for window to load" and exits; starting it again
 * on the now-warm cache succeeds. Pure logic, shared by the bootstrap and its
 * host tests; the bootstrap owns all file and process access. */
#ifndef RUNESCAPE_LAUNCHER_RETRY_H
#define RUNESCAPE_LAUNCHER_RETRY_H
#include <stddef.h>
#include <string.h>
#include <wchar.h>

#define RS_LAUNCHER_LOG_DIR L"C:\\users\\steamuser\\AppData\\Local\\Jagex Launcher\\logs\\"
#define RS_LAUNCHER_TIMEOUT_LINE "ERROR - Timeout waiting for window to load"
/* Restarts after the first start; bounded so a persistent fault still ends. */
#define RS_LAUNCHER_RETRY_MAX 2u

/* host.developer.<MM.DD.YYYY>.log in the prefix's local date. */
static int rs_launcher_log_path(wchar_t *out,size_t capacity,unsigned month,unsigned day,unsigned year)
{
    if(month<1 || month>12 || day<1 || day>31 || year<1601 || year>9999) return 0;
    int n=swprintf(out,capacity,RS_LAUNCHER_LOG_DIR L"host.developer.%02u.%02u.%04u.log",month,day,year);
    return n>0 && (size_t)n<capacity;
}

/* True when a log line is the launcher's own window-load timeout error (not a
 * page CONSOLE line that merely quotes it). `data` need not be terminated. */
static int rs_launcher_log_has_timeout(const char *data,size_t length)
{
    static const char needle[]=RS_LAUNCHER_TIMEOUT_LINE;
    static const char console[]="CONSOLE - ";
    const size_t n=sizeof(needle)-1;
    for(size_t at=0;length>=n && at<=length-n;at++) {
        if(data[at]!=needle[0] || memcmp(data+at,needle,n)) continue;
        size_t start=at;
        while(start && data[start-1]!='\n') start--;
        int quoted=0;
        for(size_t i=start;i+sizeof(console)-1<=at;i++)
            if(!memcmp(data+i,console,sizeof(console)-1)) { quoted=1;break; }
        if(!quoted) return 1;
    }
    return 0;
}

/* attempts_done counts restarts already issued for this session. */
static int rs_launcher_retry_allowed(unsigned attempts_done)
{ return attempts_done<RS_LAUNCHER_RETRY_MAX; }
#endif
