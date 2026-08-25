/*
 * ps3recomp - cellUsbd HLE implementation
 *
 * Stub. Init/term work, no USB devices are enumerated.
 * Games that require specific USB peripherals will need
 * proper device emulation.
 */

#include "cellUsbd.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

#define MAX_LDD_OPS 8
static u32 s_ldd_ops[MAX_LDD_OPS];
static int s_ldd_count = 0;
static s32 s_event_priority;
static s32 s_usbd_priority;
static s32 s_callback_priority;

/* ps3_hle_call forwards pointer arguments as raw guest EAs.  LDD callbacks
 * cannot run without an emulated USB device, so tracking identity is enough;
 * never dereference an LDD descriptor as a native host pointer. */
static u32 ldd_guest_ea(const CellUsbdLddOps* ops)
{
    return (u32)(uintptr_t)ops;
}

static s32 register_ldd(const CellUsbdLddOps* ops)
{
    const u32 ea = ldd_guest_ea(ops);
    if (!s_initialized) return (s32)CELL_USBD_ERROR_NOT_INITIALIZED;
    if (!ea) return (s32)CELL_USBD_ERROR_INVALID_ARGUMENT;
    if (s_ldd_count >= MAX_LDD_OPS)
        return (s32)CELL_USBD_ERROR_INVALID_ARGUMENT;
    s_ldd_ops[s_ldd_count++] = ea;
    return CELL_OK;
}

static s32 unregister_ldd(const CellUsbdLddOps* ops)
{
    const u32 ea = ldd_guest_ea(ops);
    if (!s_initialized) return (s32)CELL_USBD_ERROR_NOT_INITIALIZED;
    if (!ea) return (s32)CELL_USBD_ERROR_INVALID_ARGUMENT;
    for (int i = 0; i < s_ldd_count; i++) {
        if (s_ldd_ops[i] == ea) {
            for (int j = i; j < s_ldd_count - 1; j++)
                s_ldd_ops[j] = s_ldd_ops[j + 1];
            s_ldd_ops[--s_ldd_count] = 0;
            break;
        }
    }
    return CELL_OK;
}

/* API */

s32 cellUsbdInit(void)
{
    printf("[cellUsbd] Init()\n");
    if (s_initialized)
        return (s32)CELL_USBD_ERROR_ALREADY_INITIALIZED;
    s_ldd_count = 0;
    memset(s_ldd_ops, 0, sizeof(s_ldd_ops));
    s_event_priority = 0;
    s_usbd_priority = 0;
    s_callback_priority = 0;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellUsbdEnd(void)
{
    printf("[cellUsbd] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellUsbdSetThreadPriority2(s32 eventPriority, s32 usbdPriority,
                                s32 callbackPriority)
{
    /* USB worker threads are not created while enumeration is empty, but the
     * three-priority ABI and successful state update are observable. */
    s_event_priority = eventPriority;
    s_usbd_priority = usbdPriority;
    s_callback_priority = callbackPriority;
    printf("[cellUsbd] SetThreadPriority2(event=%d, usbd=%d, callback=%d)\n",
           eventPriority, usbdPriority, callbackPriority);
    return CELL_OK;
}

s32 cellUsbdRegisterLdd(const CellUsbdLddOps* ops)
{
    printf("[cellUsbd] RegisterLdd(ops=0x%08X)\n", ldd_guest_ea(ops));
    return register_ldd(ops);
}

s32 cellUsbdUnregisterLdd(const CellUsbdLddOps* ops)
{
    return unregister_ldd(ops);
}

s32 cellUsbdRegisterExtraLdd(const CellUsbdLddOps* ops, u16 vendorId,
                              u16 productId)
{
    printf("[cellUsbd] RegisterExtraLdd(ops=0x%08X, %04X:%04X)\n",
           ldd_guest_ea(ops), vendorId, productId);
    return register_ldd(ops);
}

s32 cellUsbdRegisterExtraLdd2(const CellUsbdLddOps* ops, u16 vendorId,
                               u16 productIdMin, u16 productIdMax)
{
    printf("[cellUsbd] RegisterExtraLdd2(ops=0x%08X, vendor=%04X, products=%04X-%04X)\n",
           ldd_guest_ea(ops), vendorId, productIdMin, productIdMax);
    return register_ldd(ops);
}

s32 cellUsbdUnregisterExtraLdd(const CellUsbdLddOps* ops)
{
    return unregister_ldd(ops);
}

s32 cellUsbdGetDeviceList(CellUsbdDeviceInfo* list, u32 maxDevices,
                            u32* numDevices)
{
    (void)list; (void)maxDevices;
    if (!s_initialized) return (s32)CELL_USBD_ERROR_NOT_INITIALIZED;
    if (!numDevices) return (s32)CELL_USBD_ERROR_INVALID_ARGUMENT;
    *numDevices = 0; /* no devices */
    return CELL_OK;
}

s32 cellUsbdOpenPipe(CellUsbdDeviceId deviceId, u32 endpoint,
                       CellUsbdPipeId* pipeId)
{
    (void)deviceId; (void)endpoint; (void)pipeId;
    return (s32)CELL_USBD_ERROR_NO_DEVICE;
}

s32 cellUsbdClosePipe(CellUsbdPipeId pipeId)
{
    (void)pipeId;
    return (s32)CELL_USBD_ERROR_PIPE_NOT_FOUND;
}

s32 cellUsbdBulkTransfer(CellUsbdPipeId pipeId, void* data,
                           u32 length, u32* transferred)
{
    (void)pipeId; (void)data; (void)length; (void)transferred;
    return (s32)CELL_USBD_ERROR_PIPE_NOT_FOUND;
}

s32 cellUsbdControlTransfer(CellUsbdPipeId pipeId, u8 requestType,
                              u8 request, u16 value, u16 index,
                              void* data, u32 length, u32* transferred)
{
    (void)pipeId; (void)requestType; (void)request;
    (void)value; (void)index; (void)data; (void)length; (void)transferred;
    return (s32)CELL_USBD_ERROR_PIPE_NOT_FOUND;
}
