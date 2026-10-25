#!/usr/bin/env python3
"""Execute production instance/surface/swapchain/present identity logging."""
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
spec = spec_from_file_location('attribution_functions', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
functions = '\n\n'.join(extract.definition(here / 'runtime-vulkan.c', name) for name in (
    'title_create_instance', 'title_create_swapchain', 'title_compare_u32', 'title_queue_present'))
functions += '\n' + extract.definition(args.wine / 'dlls/win32u/vulkan.c', 'nulldrv_vulkan_surface_create')
harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define WINE_PS5_RUNESCAPE 1
typedef void *HWND;
typedef int BOOL;
typedef uintptr_t UINT_PTR;
typedef struct { int left, top, right, bottom; } RECT;
struct client_surface { int live; } client_storage;
struct vulkan_instance { struct { VkInstance instance; } host; PFN_vkCreateHeadlessSurfaceEXT p_vkCreateHeadlessSurfaceEXT; };
static char logs[16384];
static void io_log(const char *format, ...) {
    size_t used = strlen(logs);va_list args;va_start(args, format);
    vsnprintf(logs + used, sizeof(logs) - used, format, args);va_end(args);
    used = strlen(logs);assert(used + 2 < sizeof(logs));strcat(logs, "\n");
}
#define MESSAGE(...) io_log(__VA_ARGS__)
static VkResult create_status, present_status, surface_status;
static unsigned create_calls, present_calls, release_calls, overlay_calls;
static int title_vulkan = 1;
static VkResult native_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *instance) {
    assert(info->enabledExtensionCount == 2);
    assert(!strcmp(info->ppEnabledExtensionNames[0], VK_KHR_SURFACE_EXTENSION_NAME));
    assert(!strcmp(info->ppEnabledExtensionNames[1], VK_KHR_DISPLAY_EXTENSION_NAME));
    create_calls++;if (!create_status) *instance = (VkInstance)(uintptr_t)0x100;return create_status;
}
static VkResult native_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                const VkAllocationCallbacks *allocator, VkSwapchainKHR *chain) {
    assert(info->surface == (VkSurfaceKHR)(uintptr_t)0x500);
    assert(info->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!create_status) *chain = (VkSwapchainKHR)(uintptr_t)0x700;return create_status;
}
static VkResult native_present(VkQueue queue, const VkPresentInfoKHR *info) { present_calls++;return present_status; }
static PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    assert(!strcmp(name, "vkCreateInstance"));return (PFN_vkVoidFunction)native_instance;
}
static PFN_vkVoidFunction vk_common_GetDeviceProcAddr(VkDevice device, const char *name) {
    assert(!strcmp(name, "vkCreateSwapchainKHR"));return (PFN_vkVoidFunction)native_swapchain;
}
static void voice_overlay_vk_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info, VkSwapchainKHR chain) { overlay_calls++; }
static unsigned pw_title_voice_overlay(void) { return 0; }
static VkResult voice_overlay_vk_present(VkQueue queue, const VkPresentInfoKHR *info, PFN_vkQueuePresentKHR present, unsigned session) {
    return present(queue, info);
}
static int64_t title_now_ns(void) { return INT64_C(1000000000); }
static PFN_vkQueuePresentKHR title_real_present = native_present;
static pthread_mutex_t title_present_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t title_present_window_ns, title_present_last_ns;
static uint32_t title_present_ms_x10[4096];
static unsigned title_present_count;
static _Atomic int64_t title_present_completed_ns;
static _Atomic int title_hang_reported;
static unsigned NtUserGetDpiForWindow(HWND hwnd) { return 96; }
static void NtUserGetClientRect(HWND hwnd, RECT *rect, unsigned dpi) { *rect = (RECT){0, 0, 100, 80}; }
static struct client_surface *nulldrv_client_surface_create(HWND hwnd) { return &client_storage; }
static void client_surface_release(struct client_surface *client) { assert(client == &client_storage);release_calls++; }
static VkResult native_surface(VkInstance instance, uint32_t width, uint32_t height, VkSurfaceKHR *surface) {
    assert(instance == (VkInstance)(uintptr_t)0x100 && width == 100 && height == 80);
    if (!surface_status) *surface = (VkSurfaceKHR)(uintptr_t)0x500;return surface_status;
}
static void *__wine_ps5_get_vulkan_proc(const char *name) {
    assert(!strcmp(name, "wineCreateDisplaySurface"));return (void *)native_surface;
}
static VkResult native_headless(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT *info,
                               const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    *surface = (VkSurfaceKHR)(uintptr_t)0x600;return VK_SUCCESS;
}
'''
cases = r'''
int main(void) {
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME};
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "DO_NOT_LOG_SESSION_APPLICATION", .pEngineName = "mesa zink"};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = extensions};
    VkInstance instance;
    assert(!title_create_instance(&info, NULL, &instance));
    assert(create_calls == 1 && info.enabledExtensionCount == 1 && strstr(logs, "engine=zink instance="));
    application.pEngineName = "DXVK";logs[0] = 0;
    assert(!title_create_instance(&info, NULL, &instance) && strstr(logs, "engine=dxvk"));
    application.pEngineName = "DO_NOT_LOG_UNKNOWN_ENGINE";logs[0] = 0;
    assert(!title_create_instance(&info, NULL, &instance) && strstr(logs, "engine=other"));
    assert(!strstr(logs, "DO_NOT_LOG"));
    create_status = VK_ERROR_INITIALIZATION_FAILED;logs[0] = 0;
    assert(title_create_instance(&info, NULL, &instance) == create_status && !strstr(logs, "DO_NOT_LOG"));
    create_status = 0;
    struct vulkan_instance driver = {.host.instance = (VkInstance)(uintptr_t)0x100, .p_vkCreateHeadlessSurfaceEXT = native_headless};
    struct client_surface *client;VkSurfaceKHR surface;
    logs[0] = 0;
    assert(!nulldrv_vulkan_surface_create((HWND)(uintptr_t)0xabc, 0, &driver, &surface, &client));
    assert(surface == (VkSurfaceKHR)(uintptr_t)0x500 && strstr(logs, "hwnd=") && strstr(logs, "surface=500 result=0"));
    surface_status = VK_ERROR_SURFACE_LOST_KHR;logs[0] = 0;
    assert(nulldrv_vulkan_surface_create((HWND)(uintptr_t)0xabc, 0, &driver, &surface, &client) == surface_status);
    assert(!client && release_calls == 1 && strstr(logs, "surface=0 result="));
    title_vulkan = 0;logs[0] = 0;
    assert(!nulldrv_vulkan_surface_create((HWND)(uintptr_t)0xabc, 0, &driver, &surface, &client) && !logs[0]);
    assert(surface == (VkSurfaceKHR)(uintptr_t)0x600);
    VkSwapchainCreateInfoKHR swap_info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = (VkSurfaceKHR)(uintptr_t)0x500, .imageExtent = {100, 80}};
    VkSwapchainKHR chain;
    assert(!title_create_swapchain((VkDevice)(uintptr_t)0x300, &swap_info, NULL, &chain));
    assert(!swap_info.imageUsage && overlay_calls == 1 && strstr(logs, "surface=500 swapchain=700"));
    VkResult per_chain = VK_ERROR_OUT_OF_DATE_KHR;
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1, .pSwapchains = &chain, .pResults = &per_chain};
    logs[0] = 0;
    assert(!title_queue_present(NULL, &present) && !strstr(logs, "first Vulkan present"));
    present_status = VK_ERROR_OUT_OF_DATE_KHR;
    assert(title_queue_present(NULL, &present) == present_status && !strstr(logs, "first Vulkan present"));
    per_chain = present_status = VK_SUCCESS;
    assert(!title_queue_present(NULL, &present) && present_calls == 3);
    assert(strstr(logs, "first Vulkan present accepted result=0 swapchain=700 count=1"));
    puts("Vulkan attribution: whitelisted engine/instance, HWND/surface, swapchain/present correlation and failure/default guards PASS");
    return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
source = args.out / 'test-vulkan-attribution.c'
source.write_text(harness + functions + cases)
binary = source.with_suffix('')
headers = here.parents[2] / 'vkd3d-proton/khronos/Vulkan-Headers/include'
subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Werror', '-pthread',
    '-D__PROSPERO__', '-I' + str(headers), str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
