/*  SPI implementation code for MPU9250 class library
 *
 *  Copyright 2017 Tlera Corporation
 *  
 *  Created by Kris Winer
 *
 *  Adapted by Simon D. Levy July 2018
 *  
 *  Library may be used freely and without limit with attribution.
 */

#if defined(_SPI)

#include "MPU9250.h"
#include "CrossPlatformSPI.h"

#include <math.h>

// Support delay() cross-platform
#if defined(ARDUINO)
#include <Arduino.h>
#elif defined(__arm__)
#include <wiringPi.h>
#else
extern void delay(uint32_t msec);
#endif


MPU_Error_t MPU9250::begin(void)
{
    return runTests();
}

uint8_t MPU9250::readRegister(uint8_t address, uint8_t subAddress)
{
    uint8_t data = 0;
    cpspi_readRegisters(subAddress, 1, &data);
    return data;
}

void MPU9250::readRegisters(uint8_t address, uint8_t subAddress, uint8_t count, uint8_t * data)
{
    (void)address;
    cpspi_readRegisters(subAddress, count, data);
}


void MPU9250::writeRegister(uint8_t address, uint8_t subAddress, uint8_t data)
{
    (void)address;
    cpspi_writeRegister(subAddress, data);
}

#endif // defined (_SPI)