#include "runescape-launcher-retry.h"
#include <assert.h>
#include <stdio.h>

static int has(const char *text) { return rs_launcher_log_has_timeout(text,strlen(text)); }

int main(void)
{
    wchar_t path[256];
    assert(rs_launcher_log_path(path,256,10,24,2026));
    assert(!wcscmp(path,L"C:\\users\\steamuser\\AppData\\Local\\Jagex Launcher\\logs\\host.developer.10.24.2026.log"));
    assert(rs_launcher_log_path(path,256,1,2,2027));
    assert(!wcscmp(wcsrchr(path,L'\\')+1,L"host.developer.01.02.2027.log"));
    assert(!rs_launcher_log_path(path,256,0,1,2026) && !rs_launcher_log_path(path,256,13,1,2026));
    assert(!rs_launcher_log_path(path,256,1,0,2026) && !rs_launcher_log_path(path,256,1,32,2026));
    assert(!rs_launcher_log_path(path,16,10,24,2026)); /* truncation is a failure */

    assert(has("2026-10-26T08:41:29.123+08 ERROR - Timeout waiting for window to load\r\n"));
    assert(has("2026-10-26T08:41:28.000+08 INFO - Overall Progress\r\n"
               "2026-10-26T08:41:29.123+08 ERROR - Timeout waiting for window to load"));
    assert(has("ERROR - Timeout waiting for window to load")); /* unterminated, at buffer start */
    assert(!has(""));
    assert(!has("2026-10-26T08:41:29.123+08 INFO - Timeout waiting for window to load\r\n"));
    assert(!has("2026-10-26T08:41:29.123+08 ERROR - Timeout waiting for window\r\n"));
    assert(!has("2026-10-26T08:41:29.123+08 CONSOLE - ERROR - Timeout waiting for window to load\r\n"));
    assert(!has("2026-10-26T08:41:29.123+08 CONSOLE - x\n2026-10-26T08:41:30.000+08 INFO - ok\n"));
    /* A quoted occurrence does not mask a genuine one on a later line. */
    assert(has("2026 CONSOLE - ERROR - Timeout waiting for window to load\n"
               "2026 ERROR - Timeout waiting for window to load\n"));
    /* Length bound is honored: text past `length` is not read. */
    assert(!rs_launcher_log_has_timeout("ERROR - Timeout waiting for window to load",10));

    assert(RS_LAUNCHER_RETRY_MAX==2);
    assert(rs_launcher_retry_allowed(0) && rs_launcher_retry_allowed(1));
    assert(!rs_launcher_retry_allowed(2) && !rs_launcher_retry_allowed(3));
    puts("runescape launcher window-timeout retry policy PASS");
    return 0;
}
