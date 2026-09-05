/* waycpy_reader.c — standalone, no Wayland/DRM deps. Reads the seqlocked
 * shared-memory frame buffer waycpy writes to and dumps one consistent
 * frame as a PPM for visual inspection. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#define SHM_NAME "/waycpy_fb"
#define HEADER_SIZE 4096

struct fb_header {
    _Atomic uint32_t seq;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t frame_count;
};

static void save_as_ppm(const char *filename, uint8_t *pixels,
        uint32_t width, uint32_t height, uint32_t stride) {
    FILE *f = fopen(filename, "wb");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t *px = row + x * 4; /* XRGB8888: B,G,R,X in memory order */
            fputc(px[2], f); fputc(px[1], f); fputc(px[0], f);
        }
    }
    fclose(f);
}

int main(int argc, char *argv[]) {
    const char *out_path = argc > 1 ? argv[1] : "reader_frame.ppm";

    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (fd < 0) { perror("shm_open (is waycpy running?)"); return 1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return 1; }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    struct fb_header *header = (struct fb_header *)map;
    uint8_t *pixels = (uint8_t *)map + HEADER_SIZE;

    /* Grab one torn-free frame via seqlock retry, with a sane retry cap
     * so a dead/stuck writer can't hang us forever. */
    uint8_t *local_buf = NULL;
    size_t pixel_bytes = 0;
    bool got_frame = false;

    for (int attempt = 0; attempt < 100 && !got_frame; attempt++) {
        uint32_t s1 = atomic_load_explicit(&header->seq, memory_order_acquire);
        if (s1 & 1) { /* writer mid-frame, back off briefly */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000 }; /* 0.5ms */
            nanosleep(&ts, NULL);
            continue;
        }

        uint32_t width = header->width, height = header->height, stride = header->stride;
        pixel_bytes = (size_t)stride * height;

        if (!local_buf) local_buf = malloc(pixel_bytes);
        memcpy(local_buf, pixels, pixel_bytes);

        uint32_t s2 = atomic_load_explicit(&header->seq, memory_order_acquire);
        if (s2 == s1) {
            got_frame = true;
            fprintf(stderr, "Captured frame #%lu (%ux%u, stride %u)\n",
                (unsigned long)header->frame_count, width, height, stride);
            save_as_ppm(out_path, local_buf, width, height, stride);
        }
        /* else: torn read (writer started a new frame mid-copy), loop and retry */
    }

    if (!got_frame) {
        fprintf(stderr, "Gave up after 100 attempts, writer may be stuck or dead\n");
        free(local_buf);
        munmap(map, st.st_size);
        close(fd);
        return 1;
    }

    printf("Saved %s\n", out_path);
    free(local_buf);
    munmap(map, st.st_size);
    close(fd);
    return 0;
}