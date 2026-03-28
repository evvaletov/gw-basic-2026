#include "portio.h"
#include "sound.h"
#include "graphics.h"
#include <string.h>

/*
 * Hardware I/O port emulation.
 *
 * Dispatch-by-port pattern (cf. virmem.c dispatch-by-address).
 * Default: reads return 0xFF (floating bus), writes silently discarded.
 */

/* --- 8253 PIT Channel 2 (ports 0x42-0x43) --- */

#define PIT_CLOCK  1193180  /* 8253 input clock frequency */

static uint8_t pit_ctrl;        /* last control word written to 0x43 */
static uint16_t pit_divisor;    /* frequency divisor (lobyte | hibyte<<8) */
static int pit_latch;           /* 0 = expecting lobyte, 1 = expecting hibyte */

static uint8_t pit_inp(uint16_t port)
{
    if (port == 0x42)
        return (uint8_t)(pit_divisor & 0xFF);
    return 0xFF;
}

static void pit_out(uint16_t port, uint8_t value)
{
    if (port == 0x43) {
        pit_ctrl = value;
        pit_latch = 0;
        return;
    }
    /* port 0x42: channel 2 data */
    if (pit_latch == 0) {
        pit_divisor = (pit_divisor & 0xFF00) | value;
        pit_latch = 1;
    } else {
        pit_divisor = (pit_divisor & 0x00FF) | ((uint16_t)value << 8);
        pit_latch = 0;
    }
}

/* --- PPI Port B (port 0x61) --- */

static uint8_t ppi_portb;

static void ppi_update_speaker(void)
{
    if ((ppi_portb & 0x03) == 0x03 && pit_divisor > 0)
        snd_start_continuous(PIT_CLOCK / pit_divisor);
    else
        snd_stop_continuous();
}

static uint8_t ppi_inp(void)
{
    return ppi_portb;
}

static void ppi_out(uint8_t value)
{
    uint8_t old = ppi_portb;
    ppi_portb = value;
    if ((old & 0x03) != (value & 0x03))
        ppi_update_speaker();
}

/* --- CGA Mode Control Register (port 0x3D8) --- */

static uint8_t cga_mode;

static void cga_mode_out(uint8_t value)
{
    uint8_t old_bits = cga_mode & 0x12;  /* bits 1 and 4 */
    cga_mode = value;
    uint8_t new_bits = value & 0x12;

    if (old_bits == new_bits)
        return;

    if (new_bits & 0x10)
        gfx_init(2);       /* 640x200 */
    else if (new_bits & 0x02)
        gfx_init(1);       /* 320x200 */
    else
        gfx_shutdown();     /* text mode */
}

/* --- CGA Color Select Register (port 0x3D9) --- */

static uint8_t cga_color;

static void cga_color_out(uint8_t value)
{
    cga_color = value;
    /* Background color (bits 0-3) → palette attribute 0 */
    gfx_palette_set(0, value & 0x0F);
    /* Palette select (bit 5): 0 = green/red/yellow, 1 = cyan/magenta/white */
    if (value & 0x20) {
        gfx_palette_set(1, 3);   /* cyan */
        gfx_palette_set(2, 5);   /* magenta */
        gfx_palette_set(3, 7);   /* white */
    } else {
        gfx_palette_set(1, 2);   /* green */
        gfx_palette_set(2, 4);   /* red */
        gfx_palette_set(3, 6);   /* yellow/brown */
    }
}

/* --- Game Port (port 0x201) --- */

#define GAME_PORT_NO_JOYSTICK  0xF0  /* bits 4-7 set = no buttons */

/* --- COM1 Serial (ports 0x3F8-0x3FE) --- */

static uint8_t com1_regs[7];  /* 0x3F8..0x3FE */

static uint8_t com1_inp(uint16_t port)
{
    int reg = port - 0x3F8;
    if (reg == 5)  /* LSR: transmitter ready */
        return 0x60;
    return com1_regs[reg];
}

static void com1_out(uint16_t port, uint8_t value)
{
    int reg = port - 0x3F8;
    if (reg >= 0 && reg < 7)
        com1_regs[reg] = value;
}

/* --- Public API --- */

uint8_t portio_inp(uint16_t port)
{
    /* PIT channel 2 */
    if (port == 0x42 || port == 0x43)
        return pit_inp(port);

    /* PPI port B */
    if (port == 0x61)
        return ppi_inp();

    /* CGA registers */
    if (port == 0x3D8)
        return cga_mode;
    if (port == 0x3D9)
        return cga_color;

    /* Game port */
    if (port == 0x201)
        return GAME_PORT_NO_JOYSTICK;

    /* COM1 */
    if (port >= 0x3F8 && port <= 0x3FE)
        return com1_inp(port);

    /* Floating bus */
    return 0xFF;
}

void portio_out(uint16_t port, uint8_t value)
{
    /* PIT channel 2 */
    if (port == 0x42 || port == 0x43) {
        pit_out(port, value);
        return;
    }

    /* PPI port B */
    if (port == 0x61) {
        ppi_out(value);
        return;
    }

    /* CGA registers */
    if (port == 0x3D8) {
        cga_mode_out(value);
        return;
    }
    if (port == 0x3D9) {
        cga_color_out(value);
        return;
    }

    /* COM1 */
    if (port >= 0x3F8 && port <= 0x3FE) {
        com1_out(port, value);
        return;
    }

    /* Default: silently discard */
}

void portio_reset(void)
{
    snd_stop_continuous();

    pit_ctrl = 0;
    pit_divisor = 0;
    pit_latch = 0;

    ppi_portb = 0;

    cga_mode = 0;
    cga_color = 0;

    memset(com1_regs, 0, sizeof(com1_regs));
}
