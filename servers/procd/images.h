/*
 * images.h - program images built into procd (until vfsd can load files).
 * Image 0 runs as PID 1 at boot.
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

#endif /* PROCD_IMAGES_H */
