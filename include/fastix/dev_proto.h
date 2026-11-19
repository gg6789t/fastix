/*
 * fastix/dev_proto.h - single source of truth for devd, isolated drivers,
 * and procd <-> devd / client <-> driver IPC protocols (AGENTS.md 5.5, 6.3, 14.5).
 */
#ifndef FASTIX_DEV_PROTO_H
#define FASTIX_DEV_PROTO_H

#include <fastix/config.h>

/* Driver kinds managed by devd. */
#define FASTIX_DRV_UART     0
#define FASTIX_DRV_RTC      1
#define FASTIX_DRV_RNG      2
#define FASTIX_DRV_BLK      3
#define FASTIX_DRV_FAULT    4
#define FASTIX_DRV_COUNT    5

/*
 * devd fixed CSpace slots (single-level 12-bit CNode, guard = 52).
 * Slot index == CPtr.
 */
#define DEVD_CAP_NULL           0
#define DEVD_CAP_SELF_CNODE     1
#define DEVD_CAP_SELF_VSPACE    2
#define DEVD_CAP_SELF_TCB       3
#define DEVD_CAP_SERVICE_EP     4
#define DEVD_CAP_PROCD_EP       5   /* badged FASTIX_BADGE_SERVER (Rank 20 -> Rank 10) */
#define DEVD_CAP_IRQ_CONTROL    6
#define DEVD_CAP_SCHED_CONTROL  7
#define DEVD_CAP_ASID_POOL      8
#define DEVD_CAP_RAM_UT         9   /* 4 MB RAM untyped for devd's own allocator */
#define DEVD_CAP_RTC_PROCD_EP   10  /* narrow badged FASTIX_BADGE_SERVER cap for rtc_pl031 */
#define DEVD_CAP_DEV_UT_FIRST   16
#define DEVD_CAP_DEV_UT_LAST    (DEVD_CAP_DEV_UT_FIRST + FASTIX_DEVD_MAX_DEV_UTS - 1)
#define DEVD_CAP_FREE_FIRST     64

/* devd virtual address layout (below 47-bit user limit). */
#define DEVD_VADDR_IPCBUF       0x0000000010000000ULL
#define DEVD_VADDR_BOOTINFO     0x0000000010010000ULL
#define DEVD_VADDR_DTB          0x0000000010020000ULL   /* up to 64 KB DTB */
#define DEVD_VADDR_DTB_MAX_SIZE 65536
#define DEVD_VADDR_SCRATCH      0x0000000010100000ULL   /* 1 page scratch for loading/probing */
#define DEVD_VADDR_STACK_TOP    0x0000000010200000ULL   /* 16 KB stack below */

/* devd endpoint badge layout: type[63:60] | drv_id[31:0]. */
#define DEVD_BADGE_TYPE_SHIFT   60
#define DEVD_BADGE_CLIENT       0x0ULL
#define DEVD_BADGE_FAULT        0x1ULL
#define DEVD_BADGE_ID_MASK      0xFFFFFFFFULL

/*
 * Isolated driver fixed CSpace slots (single-level 6-bit CNode, guard = 58).
 * Every driver receives ONLY the capabilities it needs.
 */
#define DRV_CAP_NULL            0
#define DRV_CAP_SERVICE_EP      1
#define DRV_CAP_REPLY           2
#define DRV_CAP_IRQ_NTFN        3
#define DRV_CAP_IRQ_HANDLER     4
#define DRV_CAP_MMIO_FRAME      5
#define DRV_CAP_DMA_FRAME_0     6
#define DRV_CAP_DMA_FRAME_1     7
#define DRV_CAP_PROCD_EP        8   /* populated ONLY for rtc_pl031 */

/* Isolated driver virtual address layout (plain integer defines for start.S too). */
#define DRV_VADDR_IPCBUF        0x0000000000800000
#define DRV_VADDR_BOOTINFO      0x0000000000810000
#define DRV_VADDR_MMIO          0x0000000000820000
#define DRV_VADDR_DMA_0         0x0000000000830000
#define DRV_VADDR_DMA_1         0x0000000000831000
#define DRV_VADDR_STACK_TOP     0x0000000000900000

/* procd server-control IPC labels (FASTIX_BADGE_SERVER, Rank 20 -> Rank 10). */
#define FASTIX_SRV_SET_REALTIME_NS  0xE00   /* MR0 = epoch_ns -> updates VDSO realtime offset */
#define FASTIX_SRV_REGISTER_DRV     0xE01   /* MR0 = drv_kind, extraCap0 = driver service_ep */

/* procd server-id values stored in badge pid field for FASTIX_BADGE_SERVER. */
#define FASTIX_SRVID_DEVD           1
#define FASTIX_SRVID_RTC            2

/* devd client IPC labels (Rank 50/30 -> Rank 20). */
#define DEVD_OP_INFO            0x100   /* -> MR1=drv_count, MR2=free_bytes, MR3=slots_used, MR4=dtb_size */
#define DEVD_OP_DRV_STATUS      0x101   /* MR0=drv_kind -> MR1=running, MR2=restarts, MR3=mmio_paddr, MR4=irq */

/* uart_pl011 driver IPC labels. */
#define UART_OP_WRITE           0x200   /* MR0=len(<=48), MR1..MR6=bytes -> MR0=written */
#define UART_OP_READ            0x201   /* MR0=max_len(<=48) -> MR0=nread, MR1..MR6=bytes */
#define UART_OP_INFO            0x202   /* -> MR1=mmio_paddr, MR2=irq, MR3=tx_bytes, MR4=rx_irqs */

/* rtc_pl031 driver IPC labels. */
#define RTC_OP_READ             0x210   /* -> MR0=0, MR1=epoch_sec */

/* virtio_rng driver IPC labels. */
#define RNG_OP_READ             0x220   /* MR0=len(1..48) -> MR0=nread, MR1..MR6=random bytes */

/* virtio_blk driver IPC labels. */
#define BLK_OP_INFO             0x230   /* -> MR0=0, MR1=capacity_sectors, MR2=sector_size */
#define BLK_OP_READ             0x231   /* MR0=sector -> MR0=0, MR1..MR64=512 bytes */
#define BLK_OP_WRITE            0x232   /* MR0=sector, MR1..MR64=512 bytes -> MR0=0 */

/* fault_test driver IPC labels (proves devd fault isolation & restart). */
#define FAULT_OP_PING           0x240   /* MR0=val -> MR0=0, MR1=val+1, MR2=restart_gen */
#define FAULT_OP_CRASH          0x241   /* triggers deliberate data abort mid-call (caller gets -EIO) */

#ifndef __ASSEMBLER__

#include <stdint.h>

struct devd_ut_desc {
    uint64_t paddr;
    uint32_t size_bits;
    uint32_t slot;
};

struct devd_bootinfo {
    uint64_t ram_ut_paddr;
    uint32_t ram_ut_bits;
    uint32_t dev_ut_count;
    uint64_t dtb_vaddr;
    uint32_t dtb_size;
    uint32_t _pad;
    struct devd_ut_desc dev_uts[FASTIX_DEVD_MAX_DEV_UTS];
};

struct drv_bootinfo {
    uint32_t kind;          /* FASTIX_DRV_* */
    uint32_t restart_gen;   /* 0 on first boot, incremented on each devd restart */
    uint64_t mmio_paddr;    /* physical base of this device (exact DTB base) */
    uint32_t mmio_offset;   /* byte offset inside the mapped 4 KB DRV_VADDR_MMIO page */
    uint32_t mmio_size;     /* device MMIO region size from DTB */
    uint32_t irq;           /* GIC IRQ number (0 if none) */
    uint32_t dma_count;     /* number of DMA frames mapped (0, 1, or 2) */
};

/* Entry point implemented by every isolated driver (called by drivers/common/entry.c). */
_Noreturn void drv_main(const struct drv_bootinfo *bi);

#endif /* !__ASSEMBLER__ */
#endif /* FASTIX_DEV_PROTO_H */
