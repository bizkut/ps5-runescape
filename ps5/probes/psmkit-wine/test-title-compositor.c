/* Container-safe contract for the RuneScape title window compositor planner
 * and its embedded shader (Wine dlls/win32u/ps5_compositor.c). No Vulkan
 * driver is loaded: it checks z-order, visibility, ancestor clipping, the
 * composed view and letterbox, the single-pass single-window plan, the
 * conservative pass bounds, and the SPIR-V push-constant layout. The real
 * pixel readback lives in test-title-compositor-vulkan.c (host lavapipe). */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "wine/ps5_compositor.h"
#include "ps5_compositor_spv.h"

static struct ps5c_window_state window( uint64_t key, int l, int t, int r, int b, uint32_t top_level,
                                        uint32_t depth, uint16_t z0, uint16_t z1 )
{
    struct ps5c_window_state s = {.key = key, .visible = 1, .top_level = top_level, .client = {l, t, r, b},
            .clip = {l, t, r, b}, .z_depth = depth, .z = {z0, z1}};
    return s;
}

static void test_empty_and_invalid(void)
{
    struct ps5c_plan plan;
    struct ps5c_window_state s = window( 1, 0, 0, 10, 10, 1, 1, 0, 0 );
    uint8_t content = 1, none = 0;

    assert( ps5c_plan_compose( NULL, NULL, 0, 1920, 1080, &plan ) && plan.clear && !plan.pass_count && !plan.view.valid );
    assert( ps5c_plan_compose( &s, &none, 1, 1920, 1080, &plan ) && plan.clear );      /* nothing presented yet */
    s.visible = 0;
    assert( ps5c_plan_compose( &s, &content, 1, 1920, 1080, &plan ) && plan.clear );   /* hidden */
    s = window( 1, 0, 0, 10, 10, 1, 1, 0, 0 );
    s.clip = (struct ps5c_rect){20, 20, 30, 30};
    assert( ps5c_plan_compose( &s, &content, 1, 1920, 1080, &plan ) && plan.clear );   /* clipped away */
    assert( !ps5c_plan_compose( &s, NULL, 1, 1920, 1080, &plan ) );
    assert( !ps5c_plan_compose( &s, &content, 1, 0, 1080, &plan ) );
    assert( !ps5c_plan_compose( &s, &content, PS5C_MAX_WINDOWS + 1, 1920, 1080, &plan ) );
    assert( !ps5c_plan_compose( &s, &content, 1, 1920, 1080, NULL ) );
}

/* RuneLite's fixed 765x503 canvas alone: one pass over the whole 4K output
 * with the bars painted by the same pass (no clear, no second copy). */
static void test_single_window_one_pass(void)
{
    struct ps5c_window_state s = window( 0x400dc, 1537, 828, 2302, 1331, 1, 1, 7, 0 );
    struct ps5c_plan plan;
    uint8_t content = 1;

    assert( ps5c_plan_compose( &s, &content, 1, 3840, 2160, &plan ) );
    assert( !plan.clear && plan.pass_count == 1 && plan.window[0] == 0 );
    assert( plan.passes[0].background == 1 && plan.passes[0].dst_offset[0] == 0 && plan.passes[0].dst_offset[1] == 0 );
    assert( plan.passes[0].dst_size[0] == 3840 && plan.passes[0].dst_size[1] == 2160 );
    /* The same fit as the direct single-swapchain path. */
    {
        struct wine_ps5_viewport direct;
        assert( wine_ps5_viewport_fit( &direct, 765, 503, 3840, 2160 ) );
        assert( !memcmp( &direct, &plan.view.fit, sizeof(direct) ) );
        assert( plan.view.fit.left == 277 && plan.view.fit.width == 3285 && plan.view.fit.height == 2160 );
    }
    assert( plan.view.desktop.left == 1537 && plan.view.desktop.bottom == 1331 );
}

/* Bottom-first z-order: top-level order decides, children above parents,
 * siblings by their own position; invisible and content-less windows drop. */
static void test_z_order_and_view(void)
{
    struct ps5c_window_state s[6];
    uint8_t content[6] = {1, 1, 1, 1, 0, 1};
    struct ps5c_plan plan;

    s[0] = window( 10, 100, 100, 900, 600, 1, 1, 5, 0 );     /* frame (z 5) */
    s[1] = window( 11, 110, 130, 875, 633, 0, 2, 5, 1 );     /* canvas child, sibling pos 1 */
    s[2] = window( 12, 120, 140, 300, 200, 0, 2, 5, 0 );     /* panel child, below the canvas */
    s[3] = window( 13, 2000, 50, 2119, 84, 1, 1, 9, 0 );     /* separate top-level above */
    s[4] = window( 14, 0, 0, 3840, 2160, 1, 1, 9, 1 );       /* nothing presented: ignored */
    s[5] = window( 15, 50, 50, 60, 60, 1, 1, 2, 0 );         /* hidden below */
    s[5].visible = 0;
    s[1].clip = (struct ps5c_rect){110, 130, 900, 600};      /* clipped to the frame's client */
    assert( ps5c_plan_compose( s, content, 6, 3840, 2160, &plan ) );
    assert( plan.pass_count == 4 && !plan.clear );
    assert( plan.window[0] == 0 && plan.window[1] == 2 && plan.window[2] == 1 && plan.window[3] == 3 );
    /* Children never widen the view; top-level clients do. */
    assert( plan.view.desktop.left == 100 && plan.view.desktop.top == 50 &&
            plan.view.desktop.right == 2119 && plan.view.desktop.bottom == 600 );
    assert( plan.passes[0].background && !plan.passes[1].background && !plan.passes[3].background );
    /* The canvas pass uses its clipped rectangle, its sampling keeps the full client. */
    assert( plan.passes[2].clip[3] == 600 && plan.passes[2].client[3] == 633 );

    /* Without any top-level content the view falls back to everything drawn. */
    content[0] = content[3] = 0;
    assert( ps5c_plan_compose( s, content, 6, 3840, 2160, &plan ) && plan.pass_count == 2 );
    assert( plan.view.desktop.left == 110 && plan.view.desktop.bottom == 600 );
}

/* Every output pixel whose centre lies in a later pass's visible rectangle is
 * inside that pass's dispatch rectangle, at awkward non-integer scales. */
static void test_conservative_bounds(void)
{
    static const uint32_t outputs[][2] = {{3840, 2160}, {1920, 1080}, {1279, 721}, {97, 61}};
    for (unsigned o = 0; o < sizeof(outputs) / sizeof(outputs[0]); o++)
    {
        struct ps5c_window_state s[2];
        uint8_t content[2] = {1, 1};
        struct ps5c_plan plan;
        const struct ps5c_push *p;
        unsigned covered = 0;

        s[0] = window( 1, 13, 7, 787, 545, 1, 1, 0, 0 );
        s[1] = window( 2, 300, 211, 517, 333, 0, 2, 0, 0 );
        assert( ps5c_plan_compose( s, content, 2, outputs[o][0], outputs[o][1], &plan ) && plan.pass_count == 2 );
        p = &plan.passes[1];
        for (int32_t y = 0; y < (int32_t)outputs[o][1]; y++)
            for (int32_t x = 0; x < (int32_t)outputs[o][0]; x++)
            {
                float dx = (x + 0.5f) * p->scale[0] + p->offset[0], dy = (y + 0.5f) * p->scale[1] + p->offset[1];
                if (dx < p->clip[0] || dx >= p->clip[2] || dy < p->clip[1] || dy >= p->clip[3]) continue;
                assert( x >= p->dst_offset[0] && x < p->dst_offset[0] + p->dst_size[0] );
                assert( y >= p->dst_offset[1] && y < p->dst_offset[1] + p->dst_size[1] );
                covered++;
            }
        assert( covered );
        assert( p->dst_offset[0] >= 0 && p->dst_offset[0] + p->dst_size[0] <= (int32_t)outputs[o][0] );
        assert( p->dst_offset[1] >= 0 && p->dst_offset[1] + p->dst_size[1] <= (int32_t)outputs[o][1] );
    }
}

/* The SPIR-V push-constant block must match struct ps5c_push byte for byte. */
static void test_shader_layout( const char *spirv_out )
{
    static const size_t offsets[] = {offsetof(struct ps5c_push, dst_offset), offsetof(struct ps5c_push, dst_size),
            offsetof(struct ps5c_push, scale), offsetof(struct ps5c_push, offset), offsetof(struct ps5c_push, client),
            offsetof(struct ps5c_push, clip), offsetof(struct ps5c_push, background)};
    const uint32_t *words = ps5c_compose_spv, count = sizeof(ps5c_compose_spv) / sizeof(*words);
    uint32_t block = 0, found = 0, member_offset[8];
    FILE *file;

    assert( words[0] == 0x07230203 && sizeof(struct ps5c_push) == 68 );
    /* The struct (OpTypeStruct 30) behind the PushConstant (storage class 9)
     * OpTypePointer (32); member access chains add pointers to non-structs. */
    for (uint32_t i = 5; i < count; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 32 && words[i + 2] == 9)
            for (uint32_t j = 5; j < count; j += words[j] >> 16)
                if ((words[j] & 0xffff) == 30 && words[j + 1] == words[i + 3])
                {
                    assert( !block );
                    block = words[i + 3];
                }
    assert( block );
    for (uint32_t i = 5; i < count; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 72 && words[i + 1] == block && words[i + 3] == 35) /* OpMemberDecorate Offset */
        {
            assert( words[i + 2] < 8 );
            member_offset[words[i + 2]] = words[i + 4];
            found++;
        }
    assert( found == 7 );
    for (uint32_t m = 0; m < 7; m++) assert( member_offset[m] == offsets[m] );
    if (spirv_out)
    {
        assert( (file = fopen( spirv_out, "wb" )) );
        assert( fwrite( ps5c_compose_spv, sizeof(ps5c_compose_spv), 1, file ) == 1 );
        fclose( file );
    }
}

int main( int argc, char **argv )
{
    test_empty_and_invalid();
    test_single_window_one_pass();
    test_z_order_and_view();
    {
        /* A window's GDI layer (surface incl. frame, shown in its client) sits
         * below its own swapchain whatever the scene order; the view is the
         * visible client, not the surface. */
        struct ps5c_window_state s[2];
        uint8_t content[2] = {1, 1};
        struct ps5c_plan plan;
        s[0] = window( 5, 100, 100, 300, 250, 1, 1, 3, 0 );
        s[0].layer = 1;
        s[1] = window( 5 | (1ull << 63), 96, 70, 304, 254, 1, 1, 3, 0 );
        s[1].clip = (struct ps5c_rect){100, 100, 300, 250};
        s[1].layer = 0;
        assert( ps5c_plan_compose( s, content, 2, 1920, 1080, &plan ) && plan.pass_count == 2 );
        assert( plan.window[0] == 1 && plan.window[1] == 0 && plan.passes[0].background );
        assert( plan.view.desktop.left == 100 && plan.view.desktop.top == 100 &&
                plan.view.desktop.right == 300 && plan.view.desktop.bottom == 250 );
    }
    test_conservative_bounds();
    test_shader_layout( argc > 1 ? argv[1] : NULL );
    puts( "TITLE_COMPOSITOR_PLAN_PASS scope=PLANNER_AND_SPIRV_LAYOUT_NOT_GPU_PIXELS" );
    return 0;
}
