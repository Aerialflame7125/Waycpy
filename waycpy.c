#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>
#include <wayland-client.h>
#include "wlr-screencopy-client-protocol.h"
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#define MAX_OUTPUTS 16
#define SHM_NAME "/waycpy_fb"
#define HEADER_SIZE 4096
#define WL_SHM_FORMAT_XRGB8888 1

struct fb_header {
    _Atomic uint32_t seq;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t frame_count;
};

static struct zwlr_screencopy_manager_v1 *screencopy_manager = NULL;
static struct wl_shm *shm = NULL;

struct output_entry {
    struct wl_output *wl_output;
    char name[64];
};

static struct output_entry outputs[MAX_OUTPUTS];
static int output_count = 0;

static volatile sig_atomic_t running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    running = 0;
}

struct shm_state {
    int fd;
    void *map;
    size_t map_size;

    struct fb_header *header;
    uint8_t *pixels;

    struct wl_buffer *wl_buffer;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;

    bool ready;
};

static struct shm_state g_shm = {
    .fd = -1
};

struct frame_state {
    bool done;
    bool failed;
};

/* --------------------------------------------------------- */
/* wl_output                                                  */
/* --------------------------------------------------------- */

static void output_handle_geometry(
    void *data,
    struct wl_output *output,
    int32_t x,
    int32_t y,
    int32_t physical_width,
    int32_t physical_height,
    int32_t subpixel,
    const char *make,
    const char *model,
    int32_t transform)
{
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_handle_mode(
    void *data,
    struct wl_output *output,
    uint32_t flags,
    int32_t width,
    int32_t height,
    int32_t refresh)
{
    (void)data;
    (void)output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

static void output_handle_done(
    void *data,
    struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_handle_scale(
    void *data,
    struct wl_output *output,
    int32_t factor)
{
    (void)data;
    (void)output;
    (void)factor;
}

static void output_handle_name(
    void *data,
    struct wl_output *output,
    const char *name)
{
    (void)output;

    struct output_entry *entry = data;

    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
}

static void output_handle_description(
    void *data,
    struct wl_output *output,
    const char *description)
{
    (void)data;
    (void)output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .name = output_handle_name,
    .description = output_handle_description,
};

/* --------------------------------------------------------- */
/* Registry                                                   */
/* --------------------------------------------------------- */

static void registry_handle_global(
    void *data,
    struct wl_registry *registry,
    uint32_t id,
    const char *interface,
    uint32_t version)
{
    (void)data;

    if (strcmp(interface,
               zwlr_screencopy_manager_v1_interface.name) == 0) {

        uint32_t bind_version = version < 3 ? version : 3;

        screencopy_manager =
            wl_registry_bind(
                registry,
                id,
                &zwlr_screencopy_manager_v1_interface,
                bind_version);

    } else if (strcmp(interface, wl_output_interface.name) == 0) {

        if (output_count >= MAX_OUTPUTS)
            return;

        struct output_entry *entry = &outputs[output_count++];

        memset(entry, 0, sizeof(*entry));

        uint32_t bind_version = version < 4 ? version : 4;

        entry->wl_output =
            wl_registry_bind(
                registry,
                id,
                &wl_output_interface,
                bind_version);

        wl_output_add_listener(
            entry->wl_output,
            &output_listener,
            entry);

    } else if (strcmp(interface, wl_shm_interface.name) == 0) {

        shm = wl_registry_bind(
            registry,
            id,
            &wl_shm_interface,
            1);
    }
}

static void registry_handle_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t id)
{
    (void)data;
    (void)registry;
    (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* --------------------------------------------------------- */
/* Shared memory                                               */
/* --------------------------------------------------------- */

static bool setup_shm(uint32_t width, uint32_t height, uint32_t stride, uint32_t format)
{
    if (format != WL_SHM_FORMAT_XRGB8888) {
        fprintf(stderr, "Unsupported format in setup_shm: %u\n", format);
        return false;
    }

    size_t pixel_bytes = (size_t)stride * height;
    size_t total = HEADER_SIZE + pixel_bytes;

    int fd = shm_open(
        SHM_NAME,
        O_CREAT | O_RDWR,
        0600);

    if (fd < 0) {
        perror("shm_open");
        return false;
    }

    if (ftruncate(fd, (off_t)total) < 0) {
        perror("ftruncate");
        close(fd);
        return false;
    }

    void *map = mmap(
        NULL,
        total,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);

    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return false;
    }

    g_shm.fd = fd;
    g_shm.map = map;
    g_shm.map_size = total;

    g_shm.header = (struct fb_header *)map;
    g_shm.pixels = (uint8_t *)map + HEADER_SIZE;

    g_shm.width = width;
    g_shm.height = height;
    g_shm.stride = stride;
    g_shm.format = format;
    
    atomic_store_explicit(
        &g_shm.header->seq,
        0,
        memory_order_relaxed);

    g_shm.header->width = width;
    g_shm.header->height = height;
    g_shm.header->stride = stride;
    g_shm.header->format = format;
    g_shm.header->frame_count = 0;

    /*
     * The wl_shm buffer directly references our shared memory.
     */
    struct wl_shm_pool *pool =
        wl_shm_create_pool(
            shm,
            fd,
            (int32_t)total);

    if (!pool) {
        fprintf(stderr,
                "wl_shm_create_pool failed\n");

        munmap(map, total);
        close(fd);

        memset(&g_shm, 0, sizeof(g_shm));
        g_shm.fd = -1;

        return false;
    }

    g_shm.wl_buffer =
        wl_shm_pool_create_buffer(
            pool,
            HEADER_SIZE,
            width,
            height,
            stride,
            WL_SHM_FORMAT_XRGB8888);

    wl_shm_pool_destroy(pool);

    if (!g_shm.wl_buffer) {
        fprintf(stderr,
                "wl_shm_pool_create_buffer failed\n");

        munmap(map, total);
        close(fd);

        memset(&g_shm, 0, sizeof(g_shm));
        g_shm.fd = -1;

        return false;
    }

    g_shm.ready = true;

    fprintf(
        stderr,
        "waycpy: shared memory ready: %s\n"
        "        size:   %zu bytes\n"
        "        mode:   %ux%u\n"
        "        stride: %u\n"
        "        format: XRGB8888\n",
        SHM_NAME,
        total,
        width,
        height,
        stride);

    return true;
}

/* --------------------------------------------------------- */
/* Screencopy callbacks                                        */
/* --------------------------------------------------------- */

static void frame_handle_buffer(
    void *data,
    struct zwlr_screencopy_frame_v1 *frame,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t stride)
{
    (void)data;

    /* Accept both XRGB8888 and ARGB8888 as equivalent */
    uint32_t effective_format = format;
    if (format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888) {
        effective_format = WL_SHM_FORMAT_XRGB8888;   // normalize to XRGB8888
    } else {
        fprintf(stderr, "Unsupported screencopy format: %u\n", format);
        exit(1);
    }

    if (!g_shm.ready) {
        if (!setup_shm(width, height, stride, effective_format)) {
            fprintf(stderr, "waycpy: failed to initialize shared memory\n");
            exit(1);
        }
    } else {
        /* Check geometry and format compatibility */
        if (width != g_shm.width || height != g_shm.height ||
            stride != g_shm.stride || effective_format != g_shm.format) {
            fprintf(stderr,
                    "waycpy: output format changed:\n"
                    "  old: %ux%u stride=%u format=%u\n"
                    "  new: %ux%u stride=%u format=%u\n"
                    "Restart waycpy.\n",
                    g_shm.width, g_shm.height, g_shm.stride, g_shm.format,
                    width, height, stride, effective_format);
            exit(1);
        }
    }

    /* Mark producer writing */
    atomic_fetch_add_explicit(&g_shm.header->seq, 1, memory_order_relaxed);
    zwlr_screencopy_frame_v1_copy(frame, g_shm.wl_buffer);
}

static void frame_handle_flags(
    void *data,
    struct zwlr_screencopy_frame_v1 *frame,
    uint32_t flags)
{
    (void)data;
    (void)frame;
    (void)flags;
}

static void frame_handle_buffer_done(
    void *data,
    struct zwlr_screencopy_frame_v1 *frame)
{
    (void)data;
    (void)frame;
}

static void frame_handle_ready(
    void *data,
    struct zwlr_screencopy_frame_v1 *frame,
    uint32_t tv_sec_hi,
    uint32_t tv_sec_lo,
    uint32_t tv_nsec)
{
    (void)frame;
    (void)tv_sec_hi;
    (void)tv_sec_lo;
    (void)tv_nsec;

    struct frame_state *state = data;

    g_shm.header->frame_count++;

    /*
     * Release makes all preceding pixel writes visible
     * before the consumer sees this even sequence number.
     */
    atomic_fetch_add_explicit(
        &g_shm.header->seq,
        1,
        memory_order_release);

    state->done = true;
}

static void frame_handle_failed(
    void *data,
    struct zwlr_screencopy_frame_v1 *frame)
{
    (void)frame;

    struct frame_state *state = data;

    fprintf(stderr,
            "waycpy: frame capture failed\n");

    state->failed = true;
}

static void frame_handle_damage(
    void *data,
    struct zwlr_screencopy_frame_v1 *frame,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height)
{
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void frame_handle_linux_dmabuf(
    void *data,
    struct zwlr_screencopy_frame_v1 *frame,
    uint32_t format,
    uint32_t width,
    uint32_t height)
{
    (void)data;
    (void)frame;
    (void)format;
    (void)width;
    (void)height;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
    .damage = frame_handle_damage,
    .linux_dmabuf = frame_handle_linux_dmabuf,
    .buffer_done = frame_handle_buffer_done,
};

/* --------------------------------------------------------- */
/* main                                                        */
/* --------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *target_name =
        argc > 1 ? argv[1] : "HEADLESS-1";

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    fprintf(stderr,
            "waycpy: connecting to Wayland\n");

    struct wl_display *display =
        wl_display_connect(NULL);

    if (!display) {
        fprintf(stderr,
                "waycpy: failed to connect to Wayland display\n");
        return 1;
    }

    struct wl_registry *registry =
        wl_display_get_registry(display);

    wl_registry_add_listener(
        registry,
        &registry_listener,
        NULL);

    /*
     * First roundtrip discovers globals.
     */
    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr,
                "waycpy: registry roundtrip failed\n");
        return 1;
    }

    /*
     * Second roundtrip lets wl_output.name events arrive.
     */
    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr,
                "waycpy: output roundtrip failed\n");
        return 1;
    }

    struct wl_output *target = NULL;

    for (int i = 0; i < output_count; ++i) {

        fprintf(stderr,
                "waycpy: output %d: %s\n",
                i,
                outputs[i].name);

        if (strcmp(
                outputs[i].name,
                target_name) == 0) {

            target = outputs[i].wl_output;
        }
    }

    if (!screencopy_manager) {
        fprintf(stderr,
                "waycpy: zwlr_screencopy_manager_v1 unavailable\n");
        return 1;
    }

    if (!shm) {
        fprintf(stderr,
                "waycpy: wl_shm unavailable\n");
        return 1;
    }

    if (!target) {
        fprintf(stderr,
                "waycpy: output '%s' not found\n",
                target_name);
        return 1;
    }

    fprintf(stderr,
            "waycpy: capturing output '%s'\n",
            target_name);

    while (running) {

        struct zwlr_screencopy_frame_v1 *frame =
            zwlr_screencopy_manager_v1_capture_output(
                screencopy_manager,
                0,
                target);

        if (!frame) {
            fprintf(stderr,
                    "waycpy: capture_output failed\n");
            break;
        }

        struct frame_state state = {0};

        zwlr_screencopy_frame_v1_add_listener(
            frame,
            &frame_listener,
            &state);

        while (
            running &&
            !state.done &&
            !state.failed) {

            if (wl_display_dispatch(display) < 0) {
                running = 0;
                break;
            }
        }

        zwlr_screencopy_frame_v1_destroy(frame);

        if (state.failed)
            break;
    }

    fprintf(stderr,
            "waycpy: shutting down\n");

    if (g_shm.wl_buffer)
        wl_buffer_destroy(g_shm.wl_buffer);

    if (g_shm.map)
        munmap(g_shm.map, g_shm.map_size);

    if (g_shm.fd >= 0)
        close(g_shm.fd);

    /*
     * Leave the named object around only if another process
     * wants to inspect it. drmtest normally unlinks it.
     */
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    return 0;
}