/*
 * Simple abstraction layer above Arduino hardware
 */

#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stdbool.h>

// SPI functions
void spi_init(uint32_t speed, int flags);
void spi_select(bool enable);
uint8_t spi_transfer(uint8_t in);

// serial functions
void serial_init(uint32_t speed);
void serial_putc(char c);
int serial_getc(void);
bool serial_avail(void);

// time functions
int32_t time_millis(void);

// non-volatile functions
uint8_t nv_read(int addr);
void nv_write(int addr, uint8_t data);

#endif /* HAL_H */