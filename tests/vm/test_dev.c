/*
 * tests/vm/test_dev.c - Phase 4B behavioral tests for devd and isolated drivers
 * (uart_pl011, rtc_pl031, virtio_rng, virtio_blk, and fault_test restart).
 */
#include <stdbool.h>
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <fastix/vdso.h>
#include <fastix_sys/sys.h>
#include <libos/kprint.h>
#include <libos/mem.h>
#include <sel4/sel4.h>

#include "vmtest.h"

#define NS_PER_SEC   1000000000ULL
#define YEAR_2020    1577836800ULL /* 2020-01-01T00:00:00Z */
#define SECTOR_WORDS (FASTIX_BLK_SECTOR_SIZE / sizeof(uint64_t))

static seL4_CPtr devd_cptr(void)
{
    return (seL4_CPtr)fastix_cptr_l1(FASTIX_CAP_DEVD_EP);
}

static seL4_CPtr drv_cptr(uint32_t kind)
{
    return (seL4_CPtr)fastix_cptr_l1(FASTIX_CAP_DRV_FIRST + kind);
}

static int get_drv_ep(uint32_t kind)
{
    return (int)fastix_sys1(FASTIX_TEST_GET_DRV_EP, kind);
}

static int devd_info(uint64_t *drv_cnt, uint64_t *free_b, uint64_t *slots_u, uint64_t *dtb_sz)
{
    seL4_MessageInfo_t r = seL4_Call(devd_cptr(), seL4_MessageInfo_new(DEVD_OP_INFO, 0, 0, 0));
    int64_t rc = (int64_t)seL4_GetMR(0);
    if (rc != 0 || seL4_MessageInfo_get_length(r) < 5) {
        return rc < 0 ? (int)rc : -FASTIX_EIO;
    }
    if (drv_cnt) {
        *drv_cnt = seL4_GetMR(1);
    }
    if (free_b) {
        *free_b = seL4_GetMR(2);
    }
    if (slots_u) {
        *slots_u = seL4_GetMR(3);
    }
    if (dtb_sz) {
        *dtb_sz = seL4_GetMR(4);
    }
    return 0;
}

static int devd_drv_status(uint32_t kind, uint64_t *running, uint64_t *restarts,
                           uint64_t *paddr, uint64_t *irq)
{
    seL4_SetMR(0, kind);
    seL4_MessageInfo_t r = seL4_Call(devd_cptr(), seL4_MessageInfo_new(DEVD_OP_DRV_STATUS, 0, 0, 1));
    int64_t rc = (int64_t)seL4_GetMR(0);
    if (rc != 0 || seL4_MessageInfo_get_length(r) < 5) {
        return rc < 0 ? (int)rc : -FASTIX_EIO;
    }
    if (running) {
        *running = seL4_GetMR(1);
    }
    if (restarts) {
        *restarts = seL4_GetMR(2);
    }
    if (paddr) {
        *paddr = seL4_GetMR(3);
    }
    if (irq) {
        *irq = seL4_GetMR(4);
    }
    return 0;
}

static void t_devd_and_uart(void)
{
    uint64_t drv_cnt = 0, free_b = 0, slots_u = 0, dtb_sz = 0;
    uint64_t run = 0, restarts = 0, pa = 0, irq = 0;
    int ok = devd_info(&drv_cnt, &free_b, &slots_u, &dtb_sz) == 0;
    ok &= (drv_cnt == FASTIX_DRV_COUNT && dtb_sz > 1024 && free_b > 0);
    ok &= devd_drv_status(FASTIX_DRV_UART, &run, &restarts, &pa, &irq) == 0;
    ok &= (run == 1 && restarts == 0 && pa == 0x09000000ULL && irq == 33U);
    ok &= get_drv_ep(FASTIX_DRV_UART) == 0;

    if (ok) {
        seL4_MessageInfo_t r = seL4_Call(drv_cptr(FASTIX_DRV_UART),
                                         seL4_MessageInfo_new(UART_OP_INFO, 0, 0, 0));
        ok &= (int64_t)seL4_GetMR(0) == 0 && seL4_MessageInfo_get_length(r) >= 5;
        uint64_t tx0 = seL4_GetMR(3);
        ok &= (seL4_GetMR(1) == 0x09000000ULL && seL4_GetMR(2) == 33U);

        static const char msg[] = "  uart_pl011: ok\n";
        size_t len = sizeof(msg) - 1;
        uint64_t words[6] = { 0 };
        memcpy(words, msg, len);
        seL4_SetMR(0, len);
        size_t nw = (len + 7U) / 8U;
        for (size_t i = 0; i < nw; i++) {
            seL4_SetMR(1U + i, words[i]);
        }
        (void)seL4_Call(drv_cptr(FASTIX_DRV_UART),
                        seL4_MessageInfo_new(UART_OP_WRITE, 0, 0, 1U + nw));
        ok &= seL4_GetMR(0) == len;

        (void)seL4_Call(drv_cptr(FASTIX_DRV_UART), seL4_MessageInfo_new(UART_OP_INFO, 0, 0, 0));
        ok &= (int64_t)seL4_GetMR(0) == 0 && seL4_GetMR(3) == tx0 + len;
    }
    vmtest_report("devd_dtb_and_uart_pl011", ok);
}

static void t_rtc_pl031(void)
{
    uint64_t run = 0, restarts = 0, pa = 0, irq = 0;
    int ok = devd_drv_status(FASTIX_DRV_RTC, &run, &restarts, &pa, &irq) == 0;
    ok &= (run == 1 && pa == 0x09010000ULL && irq == 34U);
    ok &= get_drv_ep(FASTIX_DRV_RTC) == 0;

    if (ok) {
        (void)seL4_Call(drv_cptr(FASTIX_DRV_RTC), seL4_MessageInfo_new(RTC_OP_READ, 0, 0, 0));
        int64_t rc = (int64_t)seL4_GetMR(0);
        uint64_t epoch_sec = seL4_GetMR(1);
        ok &= (rc == 0 && epoch_sec > YEAR_2020);

        /* Check VDSO CLOCK_REALTIME matches the RTC within 5 seconds. */
        uint64_t vdso_rt_ns = fastix_clock_realtime_ns();
        uint64_t vdso_sec = vdso_rt_ns / NS_PER_SEC;
        uint64_t diff_vdso = vdso_sec >= epoch_sec ? (vdso_sec - epoch_sec) : (epoch_sec - vdso_sec);
        ok &= (diff_vdso <= 5ULL);

        /* Check procd clock_gettime(CLOCK_REALTIME) matches too. */
        uint64_t args[1] = { FASTIX_CLOCK_REALTIME }, more[FASTIX_CALL_MORE] = { 0 };
        ok &= fastix_call(FASTIX_SYS_clock_gettime, 1, args, more) == 0;
        uint64_t sys_sec = more[0] / NS_PER_SEC;
        uint64_t diff_sys = sys_sec >= epoch_sec ? (sys_sec - epoch_sec) : (epoch_sec - sys_sec);
        ok &= (diff_sys <= 5ULL);
    }
    vmtest_report("driver_rtc_pl031_wallclock", ok);
}

static int rng_read(uint8_t *out, size_t len)
{
    seL4_SetMR(0, len);
    seL4_MessageInfo_t r = seL4_Call(drv_cptr(FASTIX_DRV_RNG),
                                     seL4_MessageInfo_new(RNG_OP_READ, 0, 0, 1));
    int64_t rc = (int64_t)seL4_GetMR(0);
    if (rc < 0) {
        return (int)rc;
    }
    size_t got = (size_t)rc;
    size_t nw = (got + 7U) / 8U;
    if (seL4_MessageInfo_get_length(r) < 1U + nw) {
        return -FASTIX_EIO;
    }
    uint8_t tmp[FASTIX_IPC_INLINE_DATA_MAX];
    for (size_t i = 0; i < nw; i++) {
        uint64_t w = seL4_GetMR(1U + i);
        memcpy(tmp + i * 8U, &w, 8U);
    }
    memcpy(out, tmp, got);
    return (int)got;
}

static bool is_all_zero(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0) {
            return false;
        }
    }
    return true;
}

static void t_virtio_rng(void)
{
    uint64_t run = 0, pa = 0, irq = 0;
    int ok = devd_drv_status(FASTIX_DRV_RNG, &run, NULL, &pa, &irq) == 0;
    ok &= (run == 1 && pa >= 0x0a000000ULL && pa < 0x0a004000ULL && irq >= 48U && irq <= 79U);
    ok &= get_drv_ep(FASTIX_DRV_RNG) == 0;

    if (ok) {
        uint8_t a[32], b[32];
        ok &= rng_read(a, sizeof(a)) == (int)sizeof(a);
        ok &= rng_read(b, sizeof(b)) == (int)sizeof(b);
        ok &= !is_all_zero(a, sizeof(a)) && !is_all_zero(b, sizeof(b));
        ok &= memcmp(a, b, sizeof(a)) != 0;

        /* Invalid lengths must return -EINVAL without crashing driver. */
        ok &= rng_read(a, 0) == -FASTIX_EINVAL;
        ok &= rng_read(a, FASTIX_IPC_INLINE_DATA_MAX + 1) == -FASTIX_EINVAL;
        ok &= rng_read(a, 16) == 16;
    }
    vmtest_report("driver_virtio_rng_entropy", ok);
}

static int blk_write_sector(uint64_t sector, const uint8_t *buf)
{
    seL4_SetMR(0, sector);
    for (size_t i = 0; i < SECTOR_WORDS; i++) {
        uint64_t w = 0;
        memcpy(&w, buf + i * sizeof(uint64_t), sizeof(uint64_t));
        seL4_SetMR(1U + i, w);
    }
    (void)seL4_Call(drv_cptr(FASTIX_DRV_BLK),
                    seL4_MessageInfo_new(BLK_OP_WRITE, 0, 0, 1U + SECTOR_WORDS));
    return (int)(int64_t)seL4_GetMR(0);
}

static int blk_read_sector(uint64_t sector, uint8_t *buf)
{
    seL4_SetMR(0, sector);
    seL4_MessageInfo_t r = seL4_Call(drv_cptr(FASTIX_DRV_BLK),
                                     seL4_MessageInfo_new(BLK_OP_READ, 0, 0, 1));
    int64_t rc = (int64_t)seL4_GetMR(0);
    if (rc != 0) {
        return (int)rc;
    }
    if (seL4_MessageInfo_get_length(r) < 1U + SECTOR_WORDS) {
        return -FASTIX_EIO;
    }
    for (size_t i = 0; i < SECTOR_WORDS; i++) {
        uint64_t w = seL4_GetMR(1U + i);
        memcpy(buf + i * sizeof(uint64_t), &w, sizeof(uint64_t));
    }
    return 0;
}

static void t_virtio_blk(void)
{
    uint64_t run = 0, pa = 0, irq = 0;
    int ok = devd_drv_status(FASTIX_DRV_BLK, &run, NULL, &pa, &irq) == 0;
    ok &= (run == 1 && pa >= 0x0a000000ULL && pa < 0x0a004000ULL && irq >= 48U && irq <= 79U);
    ok &= get_drv_ep(FASTIX_DRV_BLK) == 0;

    if (ok) {
        (void)seL4_Call(drv_cptr(FASTIX_DRV_BLK), seL4_MessageInfo_new(BLK_OP_INFO, 0, 0, 0));
        ok &= (int64_t)seL4_GetMR(0) == 0;
        uint64_t cap = seL4_GetMR(1);
        uint64_t ssz = seL4_GetMR(2);
        ok &= (cap >= 64ULL && ssz == FASTIX_BLK_SECTOR_SIZE);

        uint8_t w0[FASTIX_BLK_SECTOR_SIZE], w7[FASTIX_BLK_SECTOR_SIZE];
        uint8_t r0[FASTIX_BLK_SECTOR_SIZE], r7[FASTIX_BLK_SECTOR_SIZE];
        for (size_t i = 0; i < FASTIX_BLK_SECTOR_SIZE; i++) {
            w0[i] = (uint8_t)((i * 7U + 0xA5U) & 0xFFU);
            w7[i] = (uint8_t)((i * 13U + 0x3CU) & 0xFFU);
        }

        ok &= blk_write_sector(0, w0) == 0;
        ok &= blk_write_sector(7, w7) == 0;
        memset(r0, 0, sizeof(r0));
        memset(r7, 0, sizeof(r7));
        ok &= blk_read_sector(0, r0) == 0;
        ok &= blk_read_sector(7, r7) == 0;
        ok &= memcmp(w0, r0, FASTIX_BLK_SECTOR_SIZE) == 0;
        ok &= memcmp(w7, r7, FASTIX_BLK_SECTOR_SIZE) == 0;

        /* Out-of-bounds sector numbers must be rejected cleanly without crashing driver. */
        ok &= blk_read_sector(cap, r0) == -FASTIX_EINVAL;
        ok &= blk_write_sector(cap, w0) == -FASTIX_EINVAL;
        ok &= blk_read_sector(UINT64_MAX, r0) == -FASTIX_EINVAL;
        ok &= blk_read_sector(7, r7) == 0 && memcmp(w7, r7, FASTIX_BLK_SECTOR_SIZE) == 0;
    }
    vmtest_report("driver_virtio_blk_rw", ok);
}

static void t_fault_restart(void)
{
    uint64_t free0 = 0, slots0 = 0, run = 0, restarts = 0;
    int ok = devd_info(NULL, &free0, &slots0, NULL) == 0;
    ok &= devd_drv_status(FASTIX_DRV_FAULT, &run, &restarts, NULL, NULL) == 0;
    ok &= (run == 1 && restarts == 0);
    ok &= get_drv_ep(FASTIX_DRV_FAULT) == 0;

    if (ok) {
        seL4_SetMR(0, 41);
        (void)seL4_Call(drv_cptr(FASTIX_DRV_FAULT), seL4_MessageInfo_new(FAULT_OP_PING, 0, 0, 1));
        ok &= (int64_t)seL4_GetMR(0) == 0 && seL4_GetMR(1) == 42 && seL4_GetMR(2) == 0;

        /* Crash and recover 3 times; verify caller gets -EIO, driver restarts with
         * incremented generation on the same endpoint, and 0 bytes/slots leak in devd. */
        for (uint64_t g = 1; g <= 3 && ok; g++) {
            (void)seL4_Call(drv_cptr(FASTIX_DRV_FAULT),
                            seL4_MessageInfo_new(FAULT_OP_CRASH, 0, 0, 0));
            ok &= (int64_t)seL4_GetMR(0) == -FASTIX_EIO;

            /* Ask devd first (it always answers at once): if the restart failed, nothing
             * listens on the driver endpoint and a PING would block forever. */
            ok &= devd_drv_status(FASTIX_DRV_FAULT, &run, &restarts, NULL, NULL) == 0;
            ok &= (run == 1 && restarts == g);
            if (!ok) {
                break;
            }
            seL4_SetMR(0, 100 + g);
            (void)seL4_Call(drv_cptr(FASTIX_DRV_FAULT),
                            seL4_MessageInfo_new(FAULT_OP_PING, 0, 0, 1));
            ok &= (int64_t)seL4_GetMR(0) == 0 && seL4_GetMR(1) == 101 + g && seL4_GetMR(2) == g;
        }

        uint64_t free1 = 0, slots1 = 0;
        ok &= devd_info(NULL, &free1, &slots1, NULL) == 0;
        ok &= (free1 == free0 && slots1 == slots0);

        /* Verify other isolated drivers remained completely unaffected. */
        (void)seL4_Call(drv_cptr(FASTIX_DRV_RTC), seL4_MessageInfo_new(RTC_OP_READ, 0, 0, 0));
        ok &= (int64_t)seL4_GetMR(0) == 0 && seL4_GetMR(1) > YEAR_2020;

        uint8_t sec[FASTIX_BLK_SECTOR_SIZE];
        ok &= blk_read_sector(0, sec) == 0;
    }
    vmtest_report("devd_driver_fault_restart_zero_leak", ok);
}

void run_dev_tests(void)
{
    t_devd_and_uart();
    t_rtc_pl031();
    t_virtio_rng();
    t_virtio_blk();
    t_fault_restart();
}
