/* drivers/common/entry.c - initializes the seL4 IPC buffer and calls drv_main. */
#include <stdint.h>

#include <fastix/dev_proto.h>
#include <sel4/sel4.h>

void drv_entry(const struct drv_bootinfo *bi);
_Noreturn void drv_main(const struct drv_bootinfo *bi);

void drv_entry(const struct drv_bootinfo *bi)
{
    seL4_SetIPCBuffer((seL4_IPCBuffer *)(uintptr_t)DRV_VADDR_IPCBUF);
    drv_main(bi);
}
