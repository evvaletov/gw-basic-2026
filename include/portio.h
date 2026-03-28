#ifndef PORTIO_H
#define PORTIO_H

#include <stdint.h>

/* Read a byte from an I/O port */
uint8_t portio_inp(uint16_t port);

/* Write a byte to an I/O port */
void portio_out(uint16_t port, uint8_t value);

/* Reset all port state (called on NEW, CLEAR, RUN) */
void portio_reset(void);

#endif
