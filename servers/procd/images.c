/* images.c - table of program images embedded by the build (images.S). */
#include "images.h"

/* Symbols defined by the generated images.S. */
extern const uint8_t fastix_image_vmtest_start[], fastix_image_vmtest_end[];
extern const uint8_t fastix_image_hello_static_start[], fastix_image_hello_static_end[];
extern const uint8_t fastix_image_hello_pie_start[], fastix_image_hello_pie_end[];

static const struct image images[] = {
    { "vmtest", fastix_image_vmtest_start, fastix_image_vmtest_end },
    { "hello_static", fastix_image_hello_static_start, fastix_image_hello_static_end },
    { "hello_pie", fastix_image_hello_pie_start, fastix_image_hello_pie_end },
};

const struct image *image_get(uint64_t index)
{
    return index < sizeof(images) / sizeof(images[0]) ? &images[index] : NULL;
}
