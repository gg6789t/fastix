/* vdso.c - VDSO clock page (see vdso.h). */
#include <stddef.h>
#include <stdint.h>

#include <arch/counter.h>
#include <fastix/config.h>
#include <libos/assert.h>

#include "heap.h"
#include "vdso.h"

extern const uint8_t arch_vdso_code_start[], arch_vdso_code_end[];

static struct frame *page;
static struct frame *code_page;
static struct fastix_vdso_data *data; /* procd's writable view */

int vdso_init(void)
{
    int err = frame_new(&page);
    if (err != 0) {
        return err;
    }
    /* Map the frame's own cap here; processes map copies of it. */
    err = vm_map_cap(seL4_CapInitThreadVSpace, page->cap, FASTIX_PROCD_VDSO_VADDR, true, false,
                     heap_keep_pt, NULL);
    if (err != 0) {
        return err;
    }
    data = (struct fastix_vdso_data *)(uintptr_t)FASTIX_PROCD_VDSO_VADDR;

    /* Seqlock write: odd while updating, even when stable. */
    atomic_store_explicit(&data->seq, 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    data->shift = FASTIX_VDSO_SHIFT;
    data->mult = fastix_vdso_mult(arch_counter_freq());
    data->counter_base = arch_read_counter();
    data->mono_base_ns = 0;
    data->realtime_offset_ns = 0;
    data->boottime_offset_ns = 0;
    data->tz_offset_sec = 0;
    atomic_store_explicit(&data->seq, 2, memory_order_release);

    size_t code_len = (size_t)(arch_vdso_code_end - arch_vdso_code_start);
    libos_assert(code_len <= FASTIX_PAGE_SIZE);
    err = frame_new(&code_page);
    if (err != 0) {
        return err;
    }
    return frame_write(code_page, 0, arch_vdso_code_start, code_len, true);
}

struct frame *vdso_frame(void)
{
    return page;
}

struct frame *vdso_code_frame(void)
{
    return code_page;
}

uint64_t vdso_mono_ns(void)
{
    return fastix_vdso_mono_ns(data, arch_read_counter());
}

uint64_t vdso_realtime_offset_ns(void)
{
    return data->realtime_offset_ns;
}

void vdso_set_realtime_ns(uint64_t epoch_ns)
{
    uint64_t mono = vdso_mono_ns();
    uint64_t off = epoch_ns >= mono ? (epoch_ns - mono) : 0;
    uint32_t seq = atomic_load_explicit(&data->seq, memory_order_relaxed);

    atomic_store_explicit(&data->seq, seq + 1U, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    data->realtime_offset_ns = off;
    atomic_store_explicit(&data->seq, seq + 2U, memory_order_release);
}
