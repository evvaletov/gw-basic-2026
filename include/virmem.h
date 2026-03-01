#ifndef GW_VIRMEM_H
#define GW_VIRMEM_H

#include <stdint.h>

/*
 * Virtual 8086 address space for DEF SEG / PEEK / POKE.
 *
 * Real GW-BASIC runs in a 1MB address space where programs routinely
 * peek and poke at the BIOS data area and CGA video buffer.  We emulate
 * just enough of that space to make classic tricks work:
 *
 *   0040:0000  BIOS data area (keyboard flags, timer ticks, cursor pos)
 *   B800:0000  CGA text video buffer (char/attr pairs, 80x25 = 4000 bytes)
 *
 * Everything else reads as 0 and writes are silently discarded.
 */

/* Read a byte from virtual memory at segment:offset */
uint8_t virmem_peek(uint16_t segment, uint16_t offset);

/* Write a byte to virtual memory at segment:offset */
void virmem_poke(uint16_t segment, uint16_t offset, uint8_t value);

#endif
