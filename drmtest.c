#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>
#include <stdatomic.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <wayland-client.h>
#include <stdio.h>   // for popen etc.


/* --------------------------------------------------------- */
/* Display specification                                        */
/* --------------------------------------------------------- */

struct display_spec {
    char connector_name[64];

    uint32_t connector_id;
    uint32_t crtc_id;

    uint32_t width;
    uint32_t height;

    uint32_t refresh_mhz;

    drmModeModeInfo mode;
};

static bool hyprctl_command(const char *cmd, char *output, size_t out_size)
{
    char full_cmd[1024];
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (!wayland_display || wayland_display[0] == '\0')
        wayland_display = "wayland-0";

    if (getuid() == 0 && getenv("SUDO_USER")) {
        const char *user = getenv("SUDO_USER");
        snprintf(full_cmd, sizeof(full_cmd),
                 "sudo -u %s env XDG_RUNTIME_DIR=/run/user/$(id -u %s) "
                 "WAYLAND_DISPLAY=%s %s 2>&1",
                 user, user, wayland_display, cmd);
    } else {
        snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", cmd);
    }

    FILE *fp = popen(full_cmd, "r");
    if (!fp) {
        fprintf(stderr, "popen failed for: %s\n", full_cmd);
        return false;
    }

    char line[1024];
    bool got_output = false;
    if (output) {
        if (fgets(output, out_size, fp) != NULL) {
            output[strcspn(output, "\n")] = '\0';
            got_output = true;
        }
        while (fgets(line, sizeof(line), fp)) {} // discard rest
    } else {
        // Print all output (useful for debugging)
        while (fgets(line, sizeof(line), fp)) {
            fprintf(stderr, "hyprctl: %s", line);
        }
    }

    int status = pclose(fp);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "hyprctl command failed: %s\n", full_cmd);
        if (output && got_output) {
            fprintf(stderr, "Output: %s\n", output);
        }
        return false;
    }
    return true;
}

static void remove_headless_output(const char *output_name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "hyprctl output remove %s", output_name);
    hyprctl_command(cmd, NULL, 0);
    fprintf(stderr, "Removed headless output: %s\n", output_name);
}

static bool get_newest_headless_name(char *out_name, size_t out_size)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "hyprctl monitors all 2>/dev/null | "
             "grep -o 'HEADLESS-[0-9]*' | "
             "sed 's/HEADLESS-//' | "
             "sort -n | "
             "tail -n1");

    char num_str[32];
    if (!hyprctl_command(cmd, num_str, sizeof(num_str)))
        return false;

    if (strlen(num_str) == 0) return false;

    snprintf(out_name, out_size, "HEADLESS-%s", num_str);
    return true;
}

static bool create_headless_output(char *out_name, size_t out_size)
{
    if (!hyprctl_command("hyprctl output create headless", NULL, 0)) {
        fprintf(stderr, "Failed to create headless output\n");
        return false;
    }

    usleep(200000); // 200 ms

    if (!get_newest_headless_name(out_name, out_size)) {
        fprintf(stderr, "Failed to find newly created headless output\n");
        return false;
    }

    fprintf(stderr, "Created headless output: %s (default 1920x1080)\n", out_name);
    return true;
}

/* --------------------------------------------------------- */
/* Configuration                                               */
/* --------------------------------------------------------- */

#define SHM_NAME "/waycpy_fb"
#define HEADER_SIZE 4096

#define DEFAULT_BACKEND "auto"

#define STARTUP_TIMEOUT_MS 10000
#define FRAME_WAIT_MS 100

#define MAX_PATH_LEN 512

/* XRGB8888 */
#define WAYLAND_XRGB8888 1

/* --------------------------------------------------------- */
/* Shared memory ABI                                            */
/* --------------------------------------------------------- */

struct fb_header {
    _Atomic uint32_t seq;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;

    uint64_t frame_count;
};

/* --------------------------------------------------------- */
/* DRM buffer                                                   */
/* --------------------------------------------------------- */

struct drm_buffer {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t handle;
    uint32_t fb_id;
    uint64_t size;

    uint8_t *map;
};


/* --------------------------------------------------------- */
/* Child processes                                              */
/* --------------------------------------------------------- */

struct child_process {
    pid_t pid;
    char name[64];

    bool running;
};

static struct child_process compositor = {
    .pid = -1
};

static struct child_process waycpy = {
    .pid = -1
};

/* --------------------------------------------------------- */
/* Globals                                                      */
/* --------------------------------------------------------- */

static volatile sig_atomic_t running = 1;

static int drm_fd = -1;

static drmModeCrtc *saved_crtc = NULL;

static struct drm_buffer drm_buf = {0};

static void *shm_map = NULL;
static size_t shm_map_size = 0;

static struct fb_header *shm_header = NULL;
static uint8_t *shm_pixels = NULL;

static uint64_t last_frame = 0;

/* --------------------------------------------------------- */
/* Signals                                                      */
/* --------------------------------------------------------- */

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* --------------------------------------------------------- */
/* Time                                                        */
/* --------------------------------------------------------- */

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(
        CLOCK_MONOTONIC,
        &ts);

    return
        (uint64_t)ts.tv_sec * 1000ULL +
        (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* --------------------------------------------------------- */
/* Process helpers                                             */
/* --------------------------------------------------------- */

static void kill_child(struct child_process *child)
{
    if (!child->running || child->pid <= 0)
        return;

    kill(child->pid, SIGTERM);

    for (int i = 0; i < 20; ++i) {

        int status;

        pid_t result =
            waitpid(
                child->pid,
                &status,
                WNOHANG);

        if (result == child->pid) {
            child->running = false;
            child->pid = -1;
            return;
        }

        usleep(50000);
    }

    kill(child->pid, SIGKILL);

    waitpid(
        child->pid,
        NULL,
        0);

    child->running = false;
    child->pid = -1;
}

static bool executable_exists(const char *name)
{
    char command[MAX_PATH_LEN];

    snprintf(
        command,
        sizeof(command),
        "command -v %s >/dev/null 2>&1",
        name);

    return system(command) == 0;
}

/* --------------------------------------------------------- */
/* DRM connector enumeration                                  */
/* --------------------------------------------------------- */

static bool select_display(
    int fd,
    const char *wanted_connector,
    struct display_spec *out)
{
    drmModeRes *res =
        drmModeGetResources(fd);

    if (!res) {
        perror("drmModeGetResources");
        return false;
    }

    bool found = false;

    /*
     * First pass:
     *
     * - connected connectors only
     * - preferred mode
     * - optional exact connector match
     */
    for (int i = 0; i < res->count_connectors; ++i) {

        drmModeConnector *conn =
            drmModeGetConnector(
                fd,
                res->connectors[i]);

        if (!conn)
            continue;

        if (conn->connection != DRM_MODE_CONNECTED ||
            conn->count_modes == 0) {

            drmModeFreeConnector(conn);
            continue;
        }

        char name[64];

        snprintf(
            name,
            sizeof(name),
            "%s-%u",
            drmModeGetConnectorTypeName(
                conn->connector_type),
            conn->connector_type_id);

        if (wanted_connector &&
            strcmp(
                wanted_connector,
                name) != 0) {

            drmModeFreeConnector(conn);
            continue;
        }

        int preferred = 0;

        for (int m = 0;
             m < conn->count_modes;
             ++m) {

            if (conn->modes[m].type &
                DRM_MODE_TYPE_PREFERRED) {

                preferred = m;
                break;
            }
        }

        drmModeModeInfo mode =
            conn->modes[preferred];

        uint32_t crtc_id = 0;

        if (conn->encoder_id) {

            drmModeEncoder *enc =
                drmModeGetEncoder(
                    fd,
                    conn->encoder_id);

            if (enc) {

                if (enc->crtc_id)
                    crtc_id = enc->crtc_id;

                else {

                    for (int c = 0;
                         c < res->count_crtcs;
                         ++c) {

                        if (enc->possible_crtcs &
                            (1 << c)) {

                            crtc_id =
                                res->crtcs[c];

                            break;
                        }
                    }
                }

                drmModeFreeEncoder(enc);
            }
        }

        if (!crtc_id) {

            for (int e = 0;
                 e < conn->count_encoders && !crtc_id;
                 ++e) {

                drmModeEncoder *enc =
                    drmModeGetEncoder(
                        fd,
                        conn->encoders[e]);

                if (!enc)
                    continue;

                for (int c = 0;
                     c < res->count_crtcs;
                     ++c) {

                    if (enc->possible_crtcs &
                        (1 << c)) {

                        crtc_id =
                            res->crtcs[c];

                        break;
                    }
                }

                drmModeFreeEncoder(enc);
            }
        }

        if (!crtc_id) {
            drmModeFreeConnector(conn);
            continue;
        }

        memset(
            out,
            0,
            sizeof(*out));

        strncpy(
            out->connector_name,
            name,
            sizeof(out->connector_name) - 1);

        out->connector_id =
            conn->connector_id;

        out->crtc_id =
            crtc_id;

        out->width =
            mode.hdisplay;

        out->height =
            mode.vdisplay;

        /*
         * DRM's mode vrefresh is traditionally integer Hz.
         * Calculate a more accurate value from the mode timing
         * when possible.
         */
        if (mode.clock &&
            mode.htotal &&
            mode.vtotal) {

            double hz =
                ((double)mode.clock * 1000.0) /
                ((double)mode.htotal *
                 (double)mode.vtotal);

            out->refresh_mhz =
                (uint32_t)(hz * 1000.0 + 0.5);

        } else {

            out->refresh_mhz =
                mode.vrefresh * 1000;
        }

        out->mode = mode;

        fprintf(
            stderr,
            "DRM: selected %s\n"
            "     connector: %u\n"
            "     CRTC:      %u\n"
            "     mode:      %ux%u @ %.3f Hz\n",
            out->connector_name,
            out->connector_id,
            out->crtc_id,
            out->width,
            out->height,
            out->refresh_mhz / 1000.0);

        found = true;

        drmModeFreeConnector(conn);
        break;
    }

    drmModeFreeResources(res);

    return found;
}

/* --------------------------------------------------------- */
/* DRM dumb buffer                                              */
/* --------------------------------------------------------- */

static bool create_dumb_buffer(
    int fd,
    uint32_t width,
    uint32_t height,
    struct drm_buffer *buf)
{
    struct drm_mode_create_dumb create = {
        .width = width,
        .height = height,
        .bpp = 32
    };

    if (drmIoctl(
            fd,
            DRM_IOCTL_MODE_CREATE_DUMB,
            &create) < 0) {

        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return false;
    }

    buf->width = width;
    buf->height = height;
    buf->stride = create.pitch;
    buf->handle = create.handle;
    buf->size = create.size;

    /*
     * Legacy AddFB:
     *
     * depth = 24
     * bpp   = 32
     *
     * Corresponds to XRGB8888.
     */
    if (drmModeAddFB(
            fd,
            width,
            height,
            24,
            32,
            buf->stride,
            buf->handle,
            &buf->fb_id) < 0) {

        perror("drmModeAddFB");

        struct drm_mode_destroy_dumb destroy = {
            .handle = buf->handle
        };

        drmIoctl(
            fd,
            DRM_IOCTL_MODE_DESTROY_DUMB,
            &destroy);

        memset(buf, 0, sizeof(*buf));

        return false;
    }

    struct drm_mode_map_dumb map_req = {
        .handle = buf->handle
    };

    if (drmIoctl(
            fd,
            DRM_IOCTL_MODE_MAP_DUMB,
            &map_req) < 0) {

        perror("DRM_IOCTL_MODE_MAP_DUMB");

        drmModeRmFB(
            fd,
            buf->fb_id);

        struct drm_mode_destroy_dumb destroy = {
            .handle = buf->handle
        };

        drmIoctl(
            fd,
            DRM_IOCTL_MODE_DESTROY_DUMB,
            &destroy);

        memset(buf, 0, sizeof(*buf));

        return false;
    }

    buf->map =
        mmap(
            NULL,
            buf->size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            map_req.offset);

    if (buf->map == MAP_FAILED) {

        perror("mmap");

        drmModeRmFB(
            fd,
            buf->fb_id);

        struct drm_mode_destroy_dumb destroy = {
            .handle = buf->handle
        };

        drmIoctl(
            fd,
            DRM_IOCTL_MODE_DESTROY_DUMB,
            &destroy);

        memset(buf, 0, sizeof(*buf));

        return false;
    }

    memset(
        buf->map,
        0,
        buf->size);

    return true;
}

static void destroy_dumb_buffer(
    int fd,
    struct drm_buffer *buf)
{
    if (!buf->handle)
        return;

    if (buf->map &&
        buf->map != MAP_FAILED) {

        munmap(
            buf->map,
            buf->size);
    }

    if (buf->fb_id)
        drmModeRmFB(
            fd,
            buf->fb_id);

    struct drm_mode_destroy_dumb destroy = {
        .handle = buf->handle
    };

    drmIoctl(
        fd,
        DRM_IOCTL_MODE_DESTROY_DUMB,
        &destroy);

    memset(
        buf,
        0,
        sizeof(*buf));
}

/* --------------------------------------------------------- */
/* Shared memory reader                                        */
/* --------------------------------------------------------- */

static bool open_waycpy_shm(void)
{
    int fd =
        shm_open(
            SHM_NAME,
            O_RDONLY,
            0);

    if (fd < 0)
        return false;

    struct stat st;

    if (fstat(fd, &st) < 0) {
        close(fd);
        return false;
    }

    if (st.st_size < HEADER_SIZE) {
        close(fd);
        return false;
    }

    shm_map_size =
        (size_t)st.st_size;

    shm_map =
        mmap(
            NULL,
            shm_map_size,
            PROT_READ,
            MAP_SHARED,
            fd,
            0);

    close(fd);

    if (shm_map == MAP_FAILED) {

        shm_map = NULL;
        shm_map_size = 0;

        return false;
    }

    shm_header =
        (struct fb_header *)shm_map;

    /*
     * We only know the complete mapping size now.
     * Validate the header before using pixels.
     */
    if (shm_header->width == 0 ||
        shm_header->height == 0 ||
        shm_header->stride < shm_header->width * 4 ||
        shm_header->format != WAYLAND_XRGB8888) {

        fprintf(
            stderr,
            "DRM: invalid waycpy framebuffer header\n");

        munmap(
            shm_map,
            shm_map_size);

        shm_map = NULL;
        shm_header = NULL;
        shm_map_size = 0;

        return false;
    }

    size_t required =
        HEADER_SIZE +
        (size_t)shm_header->stride *
        shm_header->height;

    if (required > shm_map_size) {

        fprintf(
            stderr,
            "DRM: waycpy shared memory is too small\n");

        munmap(
            shm_map,
            shm_map_size);

        shm_map = NULL;
        shm_header = NULL;
        shm_map_size = 0;

        return false;
    }

    shm_pixels =
        (uint8_t *)shm_map +
        HEADER_SIZE;

    fprintf(
        stderr,
        "DRM: waycpy framebuffer:\n"
        "     %ux%u\n"
        "     stride %u\n"
        "     format %u\n",
        shm_header->width,
        shm_header->height,
        shm_header->stride,
        shm_header->format);

    return true;
}

static void close_waycpy_shm(void)
{
    if (shm_map) {

        munmap(
            shm_map,
            shm_map_size);
    }

    shm_map = NULL;
    shm_header = NULL;
    shm_pixels = NULL;
    shm_map_size = 0;
}

/*
 * Copy one stable frame.
 *
 * Returns:
 *
 *   true  = frame copied
 *   false = producer was writing / frame changed
 */
static bool copy_waycpy_frame(void)
{
    if (!shm_header)
        return false;

    uint32_t seq1 = atomic_load_explicit(&shm_header->seq, memory_order_acquire);
    if (seq1 & 1)
        return false;

    uint32_t src_width = shm_header->width;
    uint32_t src_height = shm_header->height;
    uint32_t src_stride = shm_header->stride;
    uint32_t format = shm_header->format;

    if (format != WAYLAND_XRGB8888)
        return false;

    uint32_t dst_width = drm_buf.width;
    uint32_t dst_height = drm_buf.height;

    // If sizes match, we can do a fast row-by-row copy
    if (src_width == dst_width && src_height == dst_height) {
        for (uint32_t y = 0; y < dst_height; ++y) {
            memcpy(drm_buf.map + (size_t)y * drm_buf.stride,
                   shm_pixels + (size_t)y * src_stride,
                   (size_t)dst_width * 4);
        }
    } else {
        // Scale using nearest-neighbor
        for (uint32_t y = 0; y < dst_height; ++y) {
            uint32_t src_y = (uint32_t)((float)y * src_height / dst_height);
            uint8_t *dst_row = drm_buf.map + (size_t)y * drm_buf.stride;
            uint8_t *src_row = shm_pixels + (size_t)src_y * src_stride;

            for (uint32_t x = 0; x < dst_width; ++x) {
                uint32_t src_x = (uint32_t)((float)x * src_width / dst_width);
                memcpy(dst_row + (size_t)x * 4,
                       src_row + (size_t)src_x * 4, 4);
            }
        }
    }

    // Verify producer didn't change the frame during copy
    uint32_t seq2 = atomic_load_explicit(&shm_header->seq, memory_order_acquire);
    if (seq1 != seq2 || (seq2 & 1))
        return false;

    return true;
}

/* --------------------------------------------------------- */
/* Config generation                                           */
/* --------------------------------------------------------- */

static bool write_sway_config(
    const struct display_spec *spec,
    const char *path,
    const char *wayland_socket)
{
    FILE *f = fopen(path, "w");

    if (!f) {
        perror("fopen sway config");
        return false;
    }

    /*
     * WLR_BACKENDS=headless creates the headless
     * wlroots output. Sway names it HEADLESS-1 in
     * the normal single-output case.
     */
    fprintf(
        f,
        "output HEADLESS-1 resolution %ux%u@%.3fHz\n"
        "seat seat0 hide_cursor 0\n"
        "default_border none\n"
        "default_floating_border none\n"
        "focus_follows_mouse no\n",
        spec->width,
        spec->height,
        spec->refresh_mhz / 1000.0);

    fclose(f);

    (void)wayland_socket;

    return true;
}

static bool write_hyprland_config(
    const struct display_spec *spec,
    const char *path)
{
    FILE *f = fopen(path, "w");

    if (!f) {
        perror("fopen hyprland config");
        return false;
    }

    /*
     * Current Hyprland headless instances use
     * HEADLESS-0 for their initial headless output.
     */
    fprintf(
        f,
        "monitor = HEADLESS-0, %ux%u@%.3f, 0x0, 1\n"
        "misc {\n"
        "    disable_hyprland_logo = true\n"
        "    disable_splash_rendering = true\n"
        "}\n",
        spec->width,
        spec->height,
        spec->refresh_mhz / 1000.0);

    fclose(f);

    return true;
}

/* --------------------------------------------------------- */
/* Start Sway                                                  */
/* --------------------------------------------------------- */

static bool start_sway(
    const struct display_spec *spec,
    const char *config_path,
    const char *wayland_socket)
{
    if (!executable_exists("sway")) {

        fprintf(
            stderr,
            "Sway backend requested but 'sway' was not found\n");

        return false;
    }

    if (!write_sway_config(
            spec,
            config_path,
            wayland_socket)) {

        return false;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork sway");
        return false;
    }

    if (pid == 0) {

        setenv(
            "WLR_BACKENDS",
            "headless",
            1);

        setenv(
            "WLR_LIBINPUT_NO_DEVICES",
            "1",
            1);

        setenv(
            "WLR_HEADLESS_OUTPUTS",
            "1",
            1);

        setenv(
            "WLR_RENDERER",
            "pixman",
            1);

        setenv(
            "WAYLAND_DISPLAY",
            wayland_socket,
            1);

        setenv(
            "XDG_CURRENT_DESKTOP",
            "sway",
            1);

        execlp(
            "sway",
            "sway",
            "-c",
            config_path,
            (char *)NULL);

        perror("exec sway");
        _exit(127);
    }

    compositor.pid = pid;
    compositor.running = true;

    strncpy(
        compositor.name,
        "sway",
        sizeof(compositor.name) - 1);

    return true;
}

/* --------------------------------------------------------- */
/* Start Hyprland                                               */
/* --------------------------------------------------------- */

static bool start_hyprland(
    const struct display_spec *spec,
    const char *config_path,
    const char *wayland_socket)
{
    if (!executable_exists("Hyprland")) {

        fprintf(
            stderr,
            "Hyprland backend requested but 'Hyprland' was not found\n");

        return false;
    }

    if (!write_hyprland_config(
            spec,
            config_path)) {

        return false;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork Hyprland");
        return false;
    }

    if (pid == 0) {

        setenv(
            "WLR_BACKENDS",
            "headless",
            1);

        setenv(
            "WLR_LIBINPUT_NO_DEVICES",
            "1",
            1);

        setenv(
            "WLR_HEADLESS_OUTPUTS",
            "1",
            1);

        setenv(
            "WAYLAND_DISPLAY",
            wayland_socket,
            1);

        setenv(
            "HYPRLAND_NO_SD_NOTIFY",
            "1",
            1);

        execlp(
            "Hyprland",
            "Hyprland",
            "--config",
            config_path,
            (char *)NULL);

        perror("exec Hyprland");
        _exit(127);
    }

    compositor.pid = pid;
    compositor.running = true;

    strncpy(
        compositor.name,
        "Hyprland",
        sizeof(compositor.name) - 1);

    return true;
}

/* --------------------------------------------------------- */
/* Start waycpy                                                 */
/* --------------------------------------------------------- */

static bool start_waycpy(
    const char *program,
    const char *wayland_socket,
    const char *output_name)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork waycpy");
        return false;
    }

    if (pid == 0) {

        setenv(
            "WAYLAND_DISPLAY",
            wayland_socket,
            1);

        execl(
            program,
            program,
            output_name,
            (char *)NULL);

        perror("exec waycpy");
        _exit(127);
    }

    waycpy.pid = pid;
    waycpy.running = true;

    strncpy(
        waycpy.name,
        "waycpy",
        sizeof(waycpy.name) - 1);

    return true;
}

/* --------------------------------------------------------- */
/* Wait for shared memory                                      */
/* --------------------------------------------------------- */

static bool wait_for_waycpy(
    const struct display_spec *spec)
{
    uint64_t start =
        monotonic_ms();

    while (running) {

        if (open_waycpy_shm()) {

            if (shm_header->width != spec->width || shm_header->height != spec->height) {
                fprintf(stderr,
                        "WARNING: Wayland capture resolution (%ux%u) differs from physical (%ux%u). "
                        "Scaling will be applied.\n",
                        shm_header->width, shm_header->height,
                        spec->width, spec->height);
                // Do not return false – we can continue with scaling
            }

            if (shm_header->format !=
                WAYLAND_XRGB8888) {

                fprintf(
                    stderr,
                    "ERROR: waycpy format %u is not XRGB8888\n",
                    shm_header->format);

                close_waycpy_shm();

                return false;
            }

            fprintf(
                stderr,
                "DRM: framebuffer geometry matches physical display\n");

            return true;
        }

        if (monotonic_ms() - start >
            STARTUP_TIMEOUT_MS) {

            fprintf(
                stderr,
                "Timed out waiting for %s\n",
                SHM_NAME);

            return false;
        }

        usleep(10000);
    }

    return false;
}

/* --------------------------------------------------------- */
/* DRM scanout                                                  */
/* --------------------------------------------------------- */

static bool start_scanout(
    const struct display_spec *spec)
{
    saved_crtc =
        drmModeGetCrtc(
            drm_fd,
            spec->crtc_id);

    if (!saved_crtc) {

        perror("drmModeGetCrtc");

        return false;
    }

    if (!create_dumb_buffer(
            drm_fd,
            spec->width,
            spec->height,
            &drm_buf)) {

        return false;
    }

    /*
     * Wait until we have an actual stable frame.
     */
    uint64_t start =
        monotonic_ms();

    bool copied = false;

    while (running &&
           monotonic_ms() - start <
               STARTUP_TIMEOUT_MS) {

        if (copy_waycpy_frame()) {
            copied = true;
            break;
        }

        usleep(1000);
    }

    if (!copied) {

        fprintf(
            stderr,
            "Timed out waiting for first stable frame\n");

        return false;
    }

    fprintf(
        stderr,
        "DRM: setting %s to %ux%u @ %.3f Hz\n",
        spec->connector_name,
        spec->width,
        spec->height,
        spec->refresh_mhz / 1000.0);

    if (drmModeSetCrtc(
            drm_fd,
            spec->crtc_id,
            drm_buf.fb_id,
            0,
            0,
            &spec->connector_id,
            1,
            &spec->mode) < 0) {

        perror(
            "drmModeSetCrtc");

        return false;
    }

    fprintf(
        stderr,
        "DRM: scanout active\n");

    return true;
}

/* --------------------------------------------------------- */
/* Main frame loop                                              */
/* --------------------------------------------------------- */

static void run_frame_loop(void)
{
    uint64_t last_report =
        monotonic_ms();

    uint64_t copied_frames = 0;

    while (running) {

        if (copy_waycpy_frame()) {

            copied_frames++;
        }

        /*
         * We are deliberately using legacy SetCrtc here.
         *
         * The first version is about proving:
         *
         * compositor -> screencopy -> SHM -> DRM
         *
         * The next version can replace this with page flips
         * and double buffering.
         */

        if (monotonic_ms() - last_report >= 1000) {
            copied_frames = 0;
            last_report = monotonic_ms();
        }

        /*
         * Don't busy-spin completely.
         */
        usleep(1000);
    }
}

/* --------------------------------------------------------- */
/* Cleanup                                                      */
/* --------------------------------------------------------- */

static void restore_crtc(
    const struct display_spec *spec)
{
    if (!saved_crtc)
        return;

    /*
     * If the old CRTC had a framebuffer, restore it.
     */
    if (saved_crtc->buffer_id) {

        if (drmModeSetCrtc(
                drm_fd,
                saved_crtc->crtc_id,
                saved_crtc->buffer_id,
                saved_crtc->x,
                saved_crtc->y,
                &spec->connector_id,
                1,
                &saved_crtc->mode) < 0) {

            perror(
                "drmModeSetCrtc restore");
        }
    }
}

static void cleanup(const struct display_spec *spec, const char *headless_name)
{
    running = 0;

    kill_child(&waycpy);
    kill_child(&compositor);

    close_waycpy_shm();

    if (drm_fd >= 0) {
        restore_crtc(spec);
        destroy_dumb_buffer(drm_fd, &drm_buf);
    }

    if (saved_crtc) {
        drmModeFreeCrtc(saved_crtc);
        saved_crtc = NULL;
    }

    if (drm_fd >= 0) {
        close(drm_fd);
        drm_fd = -1;
    }

    shm_unlink(SHM_NAME);

    // Remove headless output if we created one
    if (headless_name && headless_name[0] != '\0') {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "hyprctl output remove %s", headless_name);
        hyprctl_command(cmd, NULL, 0);
        fprintf(stderr, "Removed headless output: %s\n", headless_name);
    }
}

/* --------------------------------------------------------- */
/* Usage                                                        */
/* --------------------------------------------------------- */

static void usage(const char *argv0)
{
    fprintf(
        stderr,
        "\n"
        "Usage:\n"
        "  %s /dev/dri/cardN [connector] [backend] [waycpy]\n"
        "\n"
        "backend:\n"
        "  auto\n"
        "  sway\n"
        "  hyprland\n"
        "\n"
        "Examples:\n"
        "  %s /dev/dri/card0\n"
        "  %s /dev/dri/card0 DP-1\n"
        "  %s /dev/dri/card0 DP-1 sway\n"
        "  %s /dev/dri/card0 HDMI-A-1 hyprland\n"
        "\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0);
}

/* --------------------------------------------------------- */
/* main                                                        */
/* --------------------------------------------------------- */

static void resolve_exe_dir(char *out, size_t out_size)
{
    char buf[MAX_PATH_LEN];

    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        strncpy(out, ".", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    buf[len] = '\0';

    /* Strip the filename — find the last '/' */
    char *slash = strrchr(buf, '/');
    if (slash && slash != buf)
        *slash = '\0';   /* keep the directory portion */
    else
        buf[0] = '.', buf[1] = '\0';

    strncpy(out, buf, out_size - 1);
    out[out_size - 1] = '\0';
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *device    = argv[1];
    const char *connector = argc >= 3 ? argv[2] : NULL;
    const char *backend   = argc >= 4 ? argv[3] : DEFAULT_BACKEND;

    char waycpy_default[MAX_PATH_LEN];
    if (argc >= 5) {
        strncpy(waycpy_default, argv[4], sizeof(waycpy_default) - 1);
        waycpy_default[sizeof(waycpy_default) - 1] = '\0';
    } else {
        char exe_dir[MAX_PATH_LEN];
        resolve_exe_dir(exe_dir, sizeof(exe_dir));
        snprintf(waycpy_default, sizeof(waycpy_default), "%s/waycpy", exe_dir);
    }
    const char *waycpy_program = waycpy_default;
    
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Remove stale SHM object
    shm_unlink(SHM_NAME);

    // Open DRM device
    drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror(device);
        return 1;
    }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    // Select physical display
    struct display_spec spec;
    if (!select_display(drm_fd, connector, &spec)) {
        fprintf(stderr, "No usable connected DRM connector found\n");
        close(drm_fd);
        return 1;
    }

    // Determine which Wayland socket to use (existing compositor or new one)
    char wayland_socket[64];
    const char *env_socket = getenv("WAYLAND_DISPLAY");
    if (env_socket && env_socket[0] != '\0') {
        strncpy(wayland_socket, env_socket, sizeof(wayland_socket)-1);
    } else {
        strcpy(wayland_socket, "wayland-0");
    }
    wayland_socket[sizeof(wayland_socket)-1] = '\0';

    bool compositor_spawned = false;
    char headless_name[64] = {0};
    char config_path[MAX_PATH_LEN] = {0};

    // ------------------------------------------------------------------
    // 1. Use existing Hyprland if possible
    // ------------------------------------------------------------------
    if (strcmp(backend, "hyprland") == 0 || strcmp(backend, "hypr") == 0 ||
        strcmp(backend, "auto") == 0) {

        // Check if Hyprland is running
        if (system("pidof Hyprland >/dev/null 2>&1") == 0) {
            fprintf(stderr, "Hyprland detected – using existing instance\n");

            if (!create_headless_output(headless_name, sizeof(headless_name))) {
                cleanup(&spec, headless_name);
                return 1;
            }

            // We won't spawn a new compositor
            compositor_spawned = false;
            // Set compositor.name to indicate we're using an existing one
            strncpy(compositor.name, "Hyprland (existing)", sizeof(compositor.name)-1);
            
        }
    } else if (strcmp(backend, "sway") == 0) {
        // Explicit Sway – spawn it
        snprintf(config_path, sizeof(config_path),
                 "/tmp/waycpy-test-%ld.conf", (long)getpid());
        if (!start_sway(&spec, config_path, wayland_socket)) {
            fprintf(stderr, "Failed to start Sway\n");
            cleanup(&spec, headless_name);
            unlink(config_path);
            return 1;
        }
        compositor_spawned = true;
        snprintf(headless_name, sizeof(headless_name), "HEADLESS-1");
    } else {
        fprintf(stderr, "Unknown backend: %s\n", backend);
        usage(argv[0]);
        cleanup(&spec, headless_name);
        return 1;
    }

    // If we spawned a compositor, give it time to start up
    if (compositor_spawned) {
        uint64_t compositor_start = monotonic_ms();
        while (running && access("/run/user", F_OK) < 0) {
            usleep(1000);
            if (monotonic_ms() - compositor_start > STARTUP_TIMEOUT_MS)
                break;
        }
    }

    // ------------------------------------------------------------------
    // 2. Start waycpy
    // ------------------------------------------------------------------
    if (!start_waycpy(waycpy_program, wayland_socket, headless_name)) {
        fprintf(stderr, "Failed to start waycpy\n");
        cleanup(&spec, headless_name);
        if (compositor_spawned) unlink(config_path);
        return 1;
    }

    // ------------------------------------------------------------------
    // 3. Wait for shared memory
    // ------------------------------------------------------------------
    if (!wait_for_waycpy(&spec)) {
        fprintf(stderr, "waycpy framebuffer startup failed\n");
        cleanup(&spec, headless_name);
        if (compositor_spawned) unlink(config_path);
        return 1;
    }

    // ------------------------------------------------------------------
    // 4. Start DRM scanout
    // ------------------------------------------------------------------
    if (!start_scanout(&spec)) {
        fprintf(stderr, "Failed to start DRM scanout\n");
        cleanup(&spec, headless_name);
        if (compositor_spawned) unlink(config_path);
        return 1;
    }

    // ------------------------------------------------------------------
    // 5. Print status and run
    // ------------------------------------------------------------------
    fprintf(stderr,
        "\n"
        "=============================================\n"
        " waycpy DRM bridge active\n"
        "=============================================\n"
        " Physical: %s\n"
        " Mode:     %ux%u @ %.3f Hz\n"
        " Backend:  %s\n"
        " Dummy:    %s\n"
        " SHM:      %s\n"
        "\n"
        " Press Ctrl+C to stop.\n"
        "=============================================\n"
        "\n",
        spec.connector_name,
        spec.width,
        spec.height,
        spec.refresh_mhz / 1000.0,
        compositor.name,
        headless_name,
        SHM_NAME);

    run_frame_loop();

    // ------------------------------------------------------------------
    // 6. Cleanup
    // ------------------------------------------------------------------
    cleanup(&spec, headless_name);
    if (compositor_spawned) unlink(config_path);

    return 0;
}