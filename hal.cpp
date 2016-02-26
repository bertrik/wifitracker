#include "Arduino.h"
#include "hal.h"

// serial functions
void serial_init(uint32_t speed)
{
    Serial.begin(speed);
}

void serial_putc(char c)
{
    Serial.write(c);
}

int serial_getc(void)
{
    return Serial.read();
}

bool serial_avail(void)
{
    return (Serial.available() > 0);
}

