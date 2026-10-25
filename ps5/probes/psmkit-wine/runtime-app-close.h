#ifndef PW_RUNTIME_APP_CLOSE_H
#define PW_RUNTIME_APP_CLOSE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
/* SDK samples/control payload agree on size/alignment and this prefix. Other
 * field interpretations differ; consume no undocumented fields. RuneLite M2
 * qualified this own-PID lookup plus KillApp on .39, not the global BigApp getter. */
struct pw_title_app_info {
    uint32_t app_id;
    uint8_t opaque[92];
} __attribute__((aligned(8)));
_Static_assert(sizeof(struct pw_title_app_info) == 96 &&
               _Alignof(struct pw_title_app_info) == 8 &&
               offsetof(struct pw_title_app_info, app_id) == 0, "qualified app-info ABI");
extern int sceKernelGetAppInfo(pid_t, struct pw_title_app_info *);
extern int sceSystemServiceKillApp(int, int, int, int);
#endif
