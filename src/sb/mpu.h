#ifndef MPU_H
#define MPU_H
#include <stdint.h>

void    mpu_init(uint16_t port);    /* port 0 = disabled */
int     mpu_owns(uint16_t port);
uint8_t mpu_in(uint16_t port);
void    mpu_out(uint16_t port, uint8_t v);

#endif
