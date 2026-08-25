/*
 * ps3recomp - cellMouse HLE implementation
 *
 * Mouse input. Tracks mouse state from host events and provides data
 * in PS3 CellMouseData format.
 *
 * The host input layer should call cellMouse_moveEvent(), cellMouse_buttonEvent(),
 * and cellMouse_wheelEvent() when mouse events occur.
 */

#include "cellMouse.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* HLE pointer arguments are 32-bit guest effective addresses. */
extern void vm_write8 (unsigned long long a, unsigned char  v);
extern void vm_write16(unsigned long long a, unsigned short v);
extern void vm_write32(unsigned long long a, unsigned int   v);

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    int  connected;
    u32  mode;          /* relative or absolute */
    u8   buttons;       /* current button state */
    /* Accumulated delta since last GetData */
    s32  acc_dx;
    s32  acc_dy;
    s32  acc_wheel;
    int  updated;
    /* Ring buffer for data list mode */
    CellMouseData ring[CELL_MOUSE_MAX_DATA_LIST_NUM];
    u32  ring_write;
    u32  ring_count;
} MousePortState;

static int            s_mouse_initialized = 0;
static u32            s_mouse_max_connect = 0;
static MousePortState s_mouse_ports[CELL_MOUSE_MAX_MICE];

static void mouse_write_data(u32 ea, const CellMouseData* data)
{
    vm_write8((unsigned long long)ea + 0, data->update);
    vm_write8((unsigned long long)ea + 1, data->buttons);
    vm_write8((unsigned long long)ea + 2, (u8)data->x_axis);
    vm_write8((unsigned long long)ea + 3, (u8)data->y_axis);
    vm_write8((unsigned long long)ea + 4, (u8)data->wheel);
    vm_write8((unsigned long long)ea + 5, (u8)data->tilt);
}

static void mouse_clear_data(u32 ea)
{
    CellMouseData zero;
    memset(&zero, 0, sizeof(zero));
    mouse_write_data(ea, &zero);
}

/* ---------------------------------------------------------------------------
 * Host event injection
 * -----------------------------------------------------------------------*/

void cellMouse_moveEvent(u32 port, s8 dx, s8 dy)
{
    if (!s_mouse_initialized || port >= CELL_MOUSE_MAX_MICE)
        return;

    MousePortState* ms = &s_mouse_ports[port];
    if (!ms->connected) ms->connected = 1;

    ms->acc_dx += dx;
    ms->acc_dy += dy;
    ms->updated = 1;
}

void cellMouse_buttonEvent(u32 port, u8 button_mask, int pressed)
{
    if (!s_mouse_initialized || port >= CELL_MOUSE_MAX_MICE)
        return;

    MousePortState* ms = &s_mouse_ports[port];
    if (!ms->connected) ms->connected = 1;

    if (pressed)
        ms->buttons |= button_mask;
    else
        ms->buttons &= ~button_mask;
    ms->updated = 1;
}

void cellMouse_wheelEvent(u32 port, s8 wheel)
{
    if (!s_mouse_initialized || port >= CELL_MOUSE_MAX_MICE)
        return;

    MousePortState* ms = &s_mouse_ports[port];
    if (!ms->connected) ms->connected = 1;

    ms->acc_wheel += wheel;
    ms->updated = 1;
}

/* Push a snapshot into the ring buffer */
static void mouse_push_ring(MousePortState* ms)
{
    CellMouseData d;
    d.update  = ms->updated ? 1 : 0;
    d.buttons = ms->buttons;

    /* Clamp accumulated deltas to s8 range */
    s32 dx = ms->acc_dx;
    s32 dy = ms->acc_dy;
    s32 wh = ms->acc_wheel;
    if (dx > 127) dx = 127; if (dx < -128) dx = -128;
    if (dy > 127) dy = 127; if (dy < -128) dy = -128;
    if (wh > 127) wh = 127; if (wh < -128) wh = -128;

    d.x_axis = (s8)dx;
    d.y_axis = (s8)dy;
    d.wheel  = (s8)wh;
    d.tilt   = 0;

    u32 idx = ms->ring_write % CELL_MOUSE_MAX_DATA_LIST_NUM;
    ms->ring[idx] = d;
    ms->ring_write++;
    if (ms->ring_count < CELL_MOUSE_MAX_DATA_LIST_NUM)
        ms->ring_count++;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellMouseInit(u32 max_connect)
{
    printf("[cellMouse] Init(max_connect=%u)\n", max_connect);

    if (s_mouse_initialized)
        return CELL_MOUSE_ERROR_ALREADY_INITIALIZED;

    if (max_connect == 0 || max_connect > CELL_MOUSE_MAX_MICE)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    s_mouse_initialized = 1;
    s_mouse_max_connect = max_connect;
    memset(s_mouse_ports, 0, sizeof(s_mouse_ports));

    /* Assume mouse 0 is always connected on the host */
    s_mouse_ports[0].connected = 1;
    s_mouse_ports[0].mode = CELL_MOUSE_MODE_RELATIVE;

    return CELL_OK;
}

s32 cellMouseEnd(void)
{
    printf("[cellMouse] End()\n");

    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    s_mouse_initialized = 0;
    return CELL_OK;
}

s32 cellMouseGetData(u32 port_no, CellMouseData* data_guest)
{
    u32 ea = (u32)(uintptr_t)data_guest;

    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= s_mouse_max_connect || !ea)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    MousePortState* ms = &s_mouse_ports[port_no];

    if (!ms->connected) {
        mouse_clear_data(ea);
        return CELL_MOUSE_ERROR_NO_DEVICE;
    }

    CellMouseData data;
    data.update  = ms->updated ? 1 : 0;
    data.buttons = ms->buttons;

    /* Clamp accumulated deltas to s8 range */
    s32 dx = ms->acc_dx;
    s32 dy = ms->acc_dy;
    s32 wh = ms->acc_wheel;
    if (dx > 127) dx = 127; if (dx < -128) dx = -128;
    if (dy > 127) dy = 127; if (dy < -128) dy = -128;
    if (wh > 127) wh = 127; if (wh < -128) wh = -128;

    data.x_axis = (s8)dx;
    data.y_axis = (s8)dy;
    data.wheel  = (s8)wh;
    data.tilt   = 0;

    mouse_write_data(ea, &data);

    /* Reset accumulated state */
    ms->acc_dx = 0;
    ms->acc_dy = 0;
    ms->acc_wheel = 0;
    ms->updated = 0;

    return CELL_OK;
}

s32 cellMouseGetDataList(u32 port_no, CellMouseDataList* data_guest)
{
    u32 ea = (u32)(uintptr_t)data_guest;

    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= s_mouse_max_connect || !ea)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    MousePortState* ms = &s_mouse_ports[port_no];

    if (!ms->connected) {
        vm_write32(ea, 0);
        for (u32 i = 0; i < CELL_MOUSE_MAX_DATA_LIST_NUM; i++)
            mouse_clear_data(ea + 4 + i * (u32)sizeof(CellMouseData));
        return CELL_MOUSE_ERROR_NO_DEVICE;
    }

    /* Push current state into ring if there's pending data */
    if (ms->updated) {
        mouse_push_ring(ms);
        ms->acc_dx = 0;
        ms->acc_dy = 0;
        ms->acc_wheel = 0;
        ms->updated = 0;
    }

    u32 list_num = ms->ring_count;
    vm_write32(ea, list_num);
    u32 start = 0;
    if (ms->ring_write > CELL_MOUSE_MAX_DATA_LIST_NUM)
        start = ms->ring_write - CELL_MOUSE_MAX_DATA_LIST_NUM;

    for (u32 i = 0; i < list_num; i++) {
        u32 idx = (start + i) % CELL_MOUSE_MAX_DATA_LIST_NUM;
        mouse_write_data(ea + 4 + i * (u32)sizeof(CellMouseData), &ms->ring[idx]);
    }

    /* Clear ring */
    ms->ring_count = 0;
    ms->ring_write = 0;

    return CELL_OK;
}

s32 cellMouseGetInfo(CellMouseInfo* info_guest)
{
    u32 ea = (u32)(uintptr_t)info_guest;
    const u32 vendor_offset = 0x0C;
    const u32 product_offset = vendor_offset + CELL_MOUSE_MAX_INFO_DEVICES * 2;
    const u32 status_offset = product_offset + CELL_MOUSE_MAX_INFO_DEVICES * 2;

    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (!ea)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    u32 connected = 0;
    for (u32 i = 0; i < CELL_MOUSE_MAX_INFO_DEVICES; i++) {
        int on = i < s_mouse_max_connect && i < CELL_MOUSE_MAX_MICE &&
                 s_mouse_ports[i].connected;
        vm_write16((unsigned long long)ea + vendor_offset + i * 2,
                   on ? 0x054C : 0); /* Sony */
        vm_write16((unsigned long long)ea + product_offset + i * 2,
                   on ? 0x0001 : 0);
        vm_write8((unsigned long long)ea + status_offset + i,
                  on ? CELL_MOUSE_STATUS_CONNECTED : CELL_MOUSE_STATUS_DISCONNECTED);
        if (on) connected++;
    }

    vm_write32((unsigned long long)ea + 0, s_mouse_max_connect);
    vm_write32((unsigned long long)ea + 4, connected);
    vm_write32((unsigned long long)ea + 8, 0);

    return CELL_OK;
}

s32 cellMouseSetTabletMode(u32 port_no, u32 mode)
{
    printf("[cellMouse] SetTabletMode(port=%u, mode=%u)\n", port_no, mode);

    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= CELL_MOUSE_MAX_MICE)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    s_mouse_ports[port_no].mode = mode;
    return CELL_OK;
}

s32 cellMouseClearBuf(u32 port_no)
{
    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= CELL_MOUSE_MAX_MICE)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    MousePortState* ms = &s_mouse_ports[port_no];
    ms->acc_dx = 0;
    ms->acc_dy = 0;
    ms->acc_wheel = 0;
    ms->updated = 0;
    ms->ring_count = 0;
    ms->ring_write = 0;

    return CELL_OK;
}
