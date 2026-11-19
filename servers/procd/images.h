/*
 * images.h - program and script images built into procd (until vfsd loads files).
 * Image 0 runs as PID 1 at boot; execve/posix_spawn look up by path via image_find.
 */
#ifndef PROCD_IMAGES_H
#define PROCD_IMAGES_H

#include <stddef.h>
#include <stdint.h>

struct image {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
};

/* The image with this index, or NULL. */
const struct image *image_get(uint64_t index);

/* Finds an embedded ELF or script image by path (e.g. "/bin/exec_helper"), or NULL. */
const struct image *image_find(const char *path);

#endif /* PROCD_IMAGES_H */
