/* images.c - table of program and script images embedded in procd (see images.h). */
#include "images.h"

/* Symbols defined by the generated images.S. */
extern const uint8_t fastix_image_vmtest_start[], fastix_image_vmtest_end[];
extern const uint8_t fastix_image_hello_static_start[], fastix_image_hello_static_end[];
extern const uint8_t fastix_image_hello_pie_start[], fastix_image_hello_pie_end[];
extern const uint8_t fastix_image_exec_helper_start[], fastix_image_exec_helper_end[];

/* Embedded #! test scripts (used by execve tests until vfsd exists in Phase 4). */
static const uint8_t sh_basic[] = "#!  /bin/exec_helper   -flag1  \n# body\n";
static const uint8_t sh_chain1[] = "#!/bin/sh_chain2\n";
static const uint8_t sh_chain2[] = "#!/bin/sh_chain3\n";
static const uint8_t sh_chain3[] = "#!/bin/sh_chain4\n";
static const uint8_t sh_chain4[] = "#!/bin/exec_helper a4\n";
static const uint8_t sh_too_deep[] = "#!/bin/sh_chain1\n";
static const uint8_t sh_loop[] = "#!/bin/sh_loop\n";
static const uint8_t sh_bad[] = "#!   \n";
static const uint8_t not_elf[] = "this is not an ELF binary\n";

#define SCRIPT_IMG(path, buf) { path, (buf), (buf) + sizeof(buf) - 1 }

static const struct image images[] = {
    { "vmtest", fastix_image_vmtest_start, fastix_image_vmtest_end },
    { "hello_static", fastix_image_hello_static_start, fastix_image_hello_static_end },
    { "hello_pie", fastix_image_hello_pie_start, fastix_image_hello_pie_end },
    { "/bin/exec_helper", fastix_image_exec_helper_start, fastix_image_exec_helper_end },
    { "/bin/hello_static", fastix_image_hello_static_start, fastix_image_hello_static_end },
    { "/bin/hello_pie", fastix_image_hello_pie_start, fastix_image_hello_pie_end },
    SCRIPT_IMG("/bin/sh_basic", sh_basic),
    SCRIPT_IMG("/bin/sh_chain1", sh_chain1),
    SCRIPT_IMG("/bin/sh_chain2", sh_chain2),
    SCRIPT_IMG("/bin/sh_chain3", sh_chain3),
    SCRIPT_IMG("/bin/sh_chain4", sh_chain4),
    SCRIPT_IMG("/bin/sh_too_deep", sh_too_deep),
    SCRIPT_IMG("/bin/sh_loop", sh_loop),
    SCRIPT_IMG("/bin/sh_bad", sh_bad),
    SCRIPT_IMG("/bin/not_elf", not_elf),
};

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

const struct image *image_get(uint64_t index)
{
    return index < sizeof(images) / sizeof(images[0]) ? &images[index] : NULL;
}

const struct image *image_find(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        if (str_eq(images[i].name, path)) {
            return &images[i];
        }
    }
    return NULL;
}
