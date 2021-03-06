/* 
   MPU9250.cpp: implementation of MPU9250 class methods

   Copyright (C) 2018 Simon D. Levy

   Adapted from https://github.com/kriswiner/MPU9250/blob/master/MPU9250_MS5637_AHRS_t3.ino

   This file is part of MPU.

   MPU is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   MPU is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with MPU.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "MPU9250.h"

#include <math.h>

MPU9250::MPU9250(Ascale_t ascale, Gscale_t gscale, Mscale_t mscale, 
        Mmode_t mmode, uint8_t sampleRateDivisor, bool passthru) : 
    MPUIMU(ascale, gscale, sampleRateDivisor)
{
    _mRes = getMres(mscale);
    _mScale = mscale;
    _mMode = mmode;

    _sampleRateDivisor = sampleRateDivisor;

    _passthru = passthru;
}


void MPU9250::pushGyroBiases(uint8_t data[12])
{
    writeMPURegister(XG_OFFSET_H, data[0]);
    writeMPURegister(XG_OFFSET_L, data[1]);
    writeMPURegister(YG_OFFSET_H, data[2]);
    writeMPURegister(YG_OFFSET_L, data[3]);
    writeMPURegister(ZG_OFFSET_H, data[4]);
    writeMPURegister(ZG_OFFSET_L, data[5]);
}

void MPU9250::readAccelOffsets(uint8_t data[12], int32_t accel_bias_reg[3])
{
    readMPURegisters(XA_OFFSET_H, 2, &data[0]); // Read factory accelerometer trim values
    accel_bias_reg[0] = (int32_t) (((int16_t)data[0] << 8) | data[1]);
    readMPURegisters(YA_OFFSET_H, 2, &data[0]);
    accel_bias_reg[1] = (int32_t) (((int16_t)data[0] << 8) | data[1]);
    readMPURegisters(ZA_OFFSET_H, 2, &data[0]);
    accel_bias_reg[2] = (int32_t) (((int16_t)data[0] << 8) | data[1]);
}

MPUIMU::Error_t MPU9250::runTests(void) 
{ 
    // Read the WHO_AM_I register, this is a good test of communication
    if (getId() != 0x71) {
        return ERROR_IMU_ID;
    }

    reset(); // start by resetting MPU9250

    if (!selfTest()) {
        return ERROR_SELFTEST;
    }

    // Calibrate gyro and accelerometers, load biases in bias registers.
    // Comment out if using pre-measured, pre-stored accel/gyro offset biases.
    calibrate(); 

    initMPU6500(_aScale, _gScale, _sampleRateDivisor, _passthru); 

    // check AK8963 WHO AM I register, expected value is 0x48 (decimal 72)
    if (getAK8963CID() != 0x48) {
        return ERROR_MAG_ID;
    }

    // Get magnetometer calibration from AK8963 ROM
    initAK8963(_mScale, _mMode, _magCalibration);

    return ERROR_NONE;
}

float MPU9250::getMres(Mscale_t mscale) {
    switch (mscale)
    {
        // Possible magnetometer scales (and their register bit settings) are:
        // 14 bit resolution (0) and 16 bit resolution (1)
        // We multiply by 10 to convert microteslas to milligauss
        case MFS_14BITS:
            _mRes = 10.*4912./8190.; // Proper scale to return milliGauss
            return _mRes;
        case MFS_16BITS:
            _mRes = 10.*4912./32760.0; // Proper scale to return milliGauss
            return _mRes;
    }

    // For type safety
    return 0.f;
}

void MPU9250::accelWakeOnMotion()
{
    // Set accelerometer sample rate configuration
    // It is possible to get a 4 kHz sample rate from the accelerometer by choosing 1 for
    // accel_fchoice_b bit [3]; in this case the bandwidth is 1.13 kHz
    uint8_t c = readMPURegister(ACCEL_CONFIG2); // get current ACCEL_CONFIG2 register value
    c = c & ~0x0F; // Clear accel_fchoice_b (bit 3) and A_DLPFG (bits [2:0])  
    c = c | 0x01;  // Set accelerometer rate to 1 kHz and bandwidth to 184 Hz
    writeMPURegister(ACCEL_CONFIG2, c); // Write new ACCEL_CONFIG2 register value

    // Configure Interrupts and Bypass Enable
    // Set interrupt pin active high, push-pull, hold interrupt pin level HIGH until interrupt cleared,
    // clear on read of INT_STATUS, and enable I2C_BYPASS_EN so additional chips 
    // can join the I2C bus and all can be controlled by the Arduino as master 
    writeMPURegister(INT_PIN_CFG, 0x12);  // INT is 50 microsecond pulse and any read to clear  
    writeMPURegister(INT_ENABLE, 0x41);   // Enable data ready (bit 0) and wake on motion (bit 6)  interrupt

    // enable wake on motion detection logic (bit 7) and compare current sample to previous sample (bit 6)
    writeMPURegister(MOT_DETECT_CTRL, 0xC0);  

    // set accel threshold for wake up at  mG per LSB, 1 - 255 LSBs == 0 - 1020 mg), pic 0x19 for 25 mg
    writeMPURegister(MOT_THR, 0x19);

    // set sample rate in low power mode
    /* choices are 0 == 0.24 Hz, 1 == 0.49 Hz, 2 == 0.98 Hz, 3 == 1.958 Hz, 4 == 3.91 Hz, 5 == 7.81 Hz
     *             6 == 15.63 Hz, 7 == 31.25 Hz, 8 == 62.50 Hz, 9 = 125 Hz, 10 == 250 Hz, and 11 == 500 Hz
     */
    writeMPURegister(LP_ACCEL_ODR, 0x02);

    c = readMPURegister(PWR_MGMT_1);
    writeMPURegister(PWR_MGMT_1, c | 0x20);     // Write bit 5 to enable accel cycling

    gyroMagSleep();
    delay(100); // Wait for all registers to reset 

}

void MPU9250::reset()
{
    if (!_passthru) {
    	writeMPURegister(USER_CTRL, 0); // disable internal I2C bus
    }

    // reset device
    writeMPURegister(PWR_MGMT_1, 0x80); // Set bit 7 to reset MPU9250

    if (!_passthru) {
    	writeMPURegister(USER_CTRL, I2C_MST_EN); // re-enable internal I2C bus
    }
    delay(100); // Wait for all registers to reset 
}



bool MPU9250::checkWakeOnMotion()
{
    return (readMPURegister(INT_STATUS) & 0x40);
}


float MPU9250::readTemperature()
{
    int16_t t = MPUIMU::readRawTemperature();
    return ((float)t) / 333.87f + 21.0f; // Gyro chip temperature in degrees Centigrade)
}

void MPU9250::initMPU6500(Ascale_t ascale, Gscale_t gscale, uint8_t sampleRateDivisor, bool passthru)
{  
    // wake up device
    //writeMPURegister(PWR_MGMT_1, 0x00); // Clear sleep mode bit (6), enable all sensors 
    writeMPURegister(PWR_MGMT_1, 0x80); // Clear sleep mode bit (6), enable all sensors 
    delay(100); // Wait for all registers to reset 

    // get stable time source
    writeMPURegister(PWR_MGMT_1, 0x01);  // Auto select clock source to be PLL gyroscope reference if ready else
    delay(200); 

    // ----------------------------------

    // Configure Gyro and Thermometer
    // Disable FSYNC and set thermometer and gyro bandwidth to 41 and 42 Hz, respectively; 
    // minimum delay time for this setting is 5.9 ms, which means sensor fusion update rates cannot
    // be higher than 1 / 0.0059 = 170 Hz
    // DLPF_CFG = bits 2:0 = 011; this limits the sample rate to 1000 Hz for both
    // With the MPU9250, it is possible to get gyro sample rates of 32 kHz (!), 8 kHz, or 1 kHz
    if (passthru) writeMPURegister(CONFIG, 0x03);  

    // Set sample rate = gyroscope output rate/(1 + SMPLRT_DIV)
    writeMPURegister(SMPLRT_DIV, sampleRateDivisor);  // Use a 200 Hz rate; a rate consistent with the filter update rate 
    // determined inset in CONFIG above

    // Set gyroscope full scale range
    // Range selects FS_SEL and AFS_SEL are 0 - 3, so 2-bit values are left-shifted into positions 4:3
    uint8_t c = readMPURegister(GYRO_CONFIG); // get current GYRO_CONFIG register value
    // c = c & ~0xE0; // Clear self-test bits [7:5] 
    c = c & ~0x02; // Clear Fchoice bits [1:0] 
    c = c & ~0x18; // Clear AFS bits [4:3]
    c = c | gscale << 3; // Set full scale range for the gyro
    // c =| 0x00; // Set Fchoice for the gyro to 11 by writing its inverse to bits 1:0 of GYRO_CONFIG
    writeMPURegister(GYRO_CONFIG, c ); // Write new GYRO_CONFIG value to register

    // Set accelerometer full-scale range configuration
    c = readMPURegister(ACCEL_CONFIG); // get current ACCEL_CONFIG register value
    // c = c & ~0xE0; // Clear self-test bits [7:5] 
    c = c & ~0x18;  // Clear AFS bits [4:3]
    c = c | ascale << 3; // Set full scale range for the accelerometer 
    writeMPURegister(ACCEL_CONFIG, c); // Write new ACCEL_CONFIG register value

    // Set accelerometer sample rate configuration
    // It is possible to get a 4 kHz sample rate from the accelerometer by choosing 1 for
    // accel_fchoice_b bit [3]; in this case the bandwidth is 1.13 kHz
    c = readMPURegister(ACCEL_CONFIG2); // get current ACCEL_CONFIG2 register value
    c = c & ~0x0F; // Clear accel_fchoice_b (bit 3) and A_DLPFG (bits [2:0])  
    c = c | 0x03;  // Set accelerometer rate to 1 kHz and bandwidth to 41 Hz
    writeMPURegister(ACCEL_CONFIG2, c); // Write new ACCEL_CONFIG2 register value

    // The accelerometer, gyro, and thermometer are set to 1 kHz sample rates, 
    // but all these rates are further reduced by a factor of 5 to 200 Hz because of the SMPLRT_DIV setting

    // Configure Interrupts and Bypass Enable
    // Set interrupt pin active high, push-pull, hold interrupt pin level HIGH until interrupt cleared,
    // clear on read of INT_STATUS, and enable I2C_BYPASS_EN so additional chips 
    // can join the I2C bus and all can be controlled by the Arduino as master
    if (passthru) {
        //writeMPURegister(INT_PIN_CFG, 0x22);    
        writeMPURegister(INT_PIN_CFG, 0x12);  // INT is 50 microsecond pulse and any read to clear  
    }

    else {

        // enable master mode
        writeMPURegister(USER_CTRL, I2C_MST_EN);
    }

    writeMPURegister(INT_ENABLE, 0x01);  // Enable data ready (bit 0) interrupt
    delay(100);
}

void MPU9250::calibrateMagnetometer(void)
{
    uint16_t ii = 0, sample_count = 0;
    int32_t mag_bias[3] = {0, 0, 0}, mag_scale[3] = {0, 0, 0};
    int16_t mag_max[3] = {-32767, -32767, -32767}, mag_min[3] = {32767, 32767, 32767}, mag_temp[3] = {0, 0, 0};

    if(_mMode == M_8Hz) sample_count = 128;  // at 8 Hz ODR, new mag data is available every 125 ms
    if(_mMode == M_100Hz) sample_count = 1500;  // at 100 Hz ODR, new mag data is available every 10 ms

    for(ii = 0; ii < sample_count; ii++) {

        readMagData(mag_temp);  // Read the mag data   

        for (int jj = 0; jj < 3; jj++) {
            if(mag_temp[jj] > mag_max[jj]) mag_max[jj] = mag_temp[jj];
            if(mag_temp[jj] < mag_min[jj]) mag_min[jj] = mag_temp[jj];
        }

        if(_mMode == M_8Hz) delay(135);  // at 8 Hz ODR, new mag data is available every 125 ms
        if(_mMode == M_100Hz) delay(12);  // at 100 Hz ODR, new mag data is available every 10 ms
    }

    // Get hard iron correction
    mag_bias[0]  = (mag_max[0] + mag_min[0])/2;  // get average x mag bias in counts
    mag_bias[1]  = (mag_max[1] + mag_min[1])/2;  // get average y mag bias in counts
    mag_bias[2]  = (mag_max[2] + mag_min[2])/2;  // get average z mag bias in counts

    _magBias[0] = (float) mag_bias[0]*_mRes*_magCalibration[0];  // save mag biases in G for main program
    _magBias[1] = (float) mag_bias[1]*_mRes*_magCalibration[1];   
    _magBias[2] = (float) mag_bias[2]*_mRes*_magCalibration[2];  

    // Get soft iron correction estimate
    mag_scale[0]  = (mag_max[0] - mag_min[0])/2;  // get average x axis max chord length in counts
    mag_scale[1]  = (mag_max[1] - mag_min[1])/2;  // get average y axis max chord length in counts
    mag_scale[2]  = (mag_max[2] - mag_min[2])/2;  // get average z axis max chord length in counts

    float avg_rad = mag_scale[0] + mag_scale[1] + mag_scale[2];
    avg_rad /= 3.0;

    _magScale[0] = avg_rad/((float)mag_scale[0]);
    _magScale[1] = avg_rad/((float)mag_scale[1]);
    _magScale[2] = avg_rad/((float)mag_scale[2]);
}


// Accelerometer and gyroscope self test; check calibration wrt factory settings
// Checks percent deviation from factory trim values, +/- 14 or less deviation is a pass
bool MPU9250::selfTest(void)
{
    uint8_t rawData[6] = {0, 0, 0, 0, 0, 0};
    uint8_t selfTest[6];
    int32_t gAvg[3] = {0}, aAvg[3] = {0}, aSTAvg[3] = {0}, gSTAvg[3] = {0};
    float factoryTrim[6];
    uint8_t FS = 0;

    writeMPURegister(SMPLRT_DIV, 0x00);    // Set gyro sample rate to 1 kHz
    writeMPURegister(CONFIG, 0x02);        // Set gyro sample rate to 1 kHz and DLPF to 92 Hz
    writeMPURegister(GYRO_CONFIG, 1<<FS);  // Set full scale range for the gyro to 250 dps
    writeMPURegister(ACCEL_CONFIG2, 0x02); // Set accelerometer rate to 1 kHz and bandwidth to 92 Hz
    writeMPURegister(ACCEL_CONFIG, 1<<FS); // Set full scale range for the accelerometer to 2 g

    for( int ii = 0; ii < 200; ii++) {  // get average current values of gyro and acclerometer

        readMPURegisters(ACCEL_XOUT_H, 6, &rawData[0]);        // Read the six raw data registers into data array
        aAvg[0] += (int16_t)(((int16_t)rawData[0] << 8) | rawData[1]) ;  // Turn the MSB and LSB into a signed 16-bit value
        aAvg[1] += (int16_t)(((int16_t)rawData[2] << 8) | rawData[3]) ;  
        aAvg[2] += (int16_t)(((int16_t)rawData[4] << 8) | rawData[5]) ; 

        readMPURegisters(GYRO_XOUT_H, 6, &rawData[0]);       // Read the six raw data registers sequentially into data array
        gAvg[0] += (int16_t)(((int16_t)rawData[0] << 8) | rawData[1]) ;  // Turn the MSB and LSB into a signed 16-bit value
        gAvg[1] += (int16_t)(((int16_t)rawData[2] << 8) | rawData[3]) ;  
        gAvg[2] += (int16_t)(((int16_t)rawData[4] << 8) | rawData[5]) ; 
    }

    for (int ii =0; ii < 3; ii++) {  // Get average of 200 values and store as average current readings
        aAvg[ii] /= 200;
        gAvg[ii] /= 200;
    }

    // Configure the accelerometer for self-test
    writeMPURegister(ACCEL_CONFIG, 0xE0); // Enable self test on all three axes and set accelerometer range to +/- 2 g
    writeMPURegister(GYRO_CONFIG,  0xE0); // Enable self test on all three axes and set gyro range to +/- 250 degrees/s
    delay(25);  // Delay a while to let the device stabilize

    for( int ii = 0; ii < 200; ii++) {  // get average self-test values of gyro and acclerometer

        readMPURegisters(ACCEL_XOUT_H, 6, &rawData[0]);  // Read the six raw data registers into data array
        aSTAvg[0] += (int16_t)(((int16_t)rawData[0] << 8) | rawData[1]) ;  // Turn the MSB and LSB into a signed 16-bit value
        aSTAvg[1] += (int16_t)(((int16_t)rawData[2] << 8) | rawData[3]) ;  
        aSTAvg[2] += (int16_t)(((int16_t)rawData[4] << 8) | rawData[5]) ; 

        readMPURegisters(GYRO_XOUT_H, 6, &rawData[0]);  // Read the six raw data registers sequentially into data array
        gSTAvg[0] += (int16_t)(((int16_t)rawData[0] << 8) | rawData[1]) ;  // Turn the MSB and LSB into a signed 16-bit value
        gSTAvg[1] += (int16_t)(((int16_t)rawData[2] << 8) | rawData[3]) ;  
        gSTAvg[2] += (int16_t)(((int16_t)rawData[4] << 8) | rawData[5]) ; 
    }

    for (int ii =0; ii < 3; ii++) {  // Get average of 200 values and store as average self-test readings
        aSTAvg[ii] /= 200;
        gSTAvg[ii] /= 200;
    }   

    // Configure the gyro and accelerometer for normal operation
    writeMPURegister(ACCEL_CONFIG, 0x00);  
    writeMPURegister(GYRO_CONFIG,  0x00);  
    delay(25);  // Delay a while to let the device stabilize

    // Retrieve accelerometer and gyro factory Self-Test Code from USR_Reg
    selfTest[0] = readMPURegister(SELF_TEST_X_ACCEL); // X-axis accel self-test results
    selfTest[1] = readMPURegister(SELF_TEST_Y_ACCEL); // Y-axis accel self-test results
    selfTest[2] = readMPURegister(SELF_TEST_Z_ACCEL); // Z-axis accel self-test results
    selfTest[3] = readMPURegister(MPU6500::SELF_TEST_X_GYRO);  // X-axis gyro self-test results
    selfTest[4] = readMPURegister(MPU6500::SELF_TEST_Y_GYRO);  // Y-axis gyro self-test results
    selfTest[5] = readMPURegister(MPU6500::SELF_TEST_Z_GYRO);  // Z-axis gyro self-test results

    // Retrieve factory self-test value from self-test code reads
    factoryTrim[0] = (float)(2620/1<<FS)*(pow( 1.01 , ((float)selfTest[0] - 1.0) )); // FT[Xa] factory trim calculation
    factoryTrim[1] = (float)(2620/1<<FS)*(pow( 1.01 , ((float)selfTest[1] - 1.0) )); // FT[Ya] factory trim calculation
    factoryTrim[2] = (float)(2620/1<<FS)*(pow( 1.01 , ((float)selfTest[2] - 1.0) )); // FT[Za] factory trim calculation
    factoryTrim[3] = (float)(2620/1<<FS)*(pow( 1.01 , ((float)selfTest[3] - 1.0) )); // FT[Xg] factory trim calculation
    factoryTrim[4] = (float)(2620/1<<FS)*(pow( 1.01 , ((float)selfTest[4] - 1.0) )); // FT[Yg] factory trim calculation
    factoryTrim[5] = (float)(2620/1<<FS)*(pow( 1.01 , ((float)selfTest[5] - 1.0) )); // FT[Zg] factory trim calculation

    // Report results as a ratio of (STR - FT)/FT; the change from Factory Trim of the Self-Test Response
    // To get percent, must multiply by 100
    float tolerances[6];
    for (int i = 0; i < 3; i++) {
        tolerances[i]   = 100.0f*((float)(aSTAvg[i] - aAvg[i]))/factoryTrim[i] - 100.0f;   // Report percent differences
        tolerances[i+3] = 100.0f*((float)(gSTAvg[i] - gAvg[i]))/factoryTrim[i+3] - 100.0f; // Report percent differences
    }

    // Check results
    for (uint8_t k=0; k<6; ++k) {
        if (tolerances[k] >= 14.f) {
            return false;
        }
    }

    // Success!
    return true;
}

void MPU9250::readGyrometer(float & gx, float & gy, float & gz)
{
    MPUIMU::readGyrometer(gx, gy, gz);
}


uint8_t MPU9250::readAK8963Register(uint8_t subAddress)
{
    uint8_t buffer = 0;
    readAK8963Registers(subAddress, 1, &buffer);
    return buffer;
}

uint8_t MPU9250::getAK8963CID()
{
    return readAK8963Register(AK8963_WHO_AM_I);
}

void MPU9250::gyroMagSleep()
{
    uint8_t temp = 0;
    temp = readAK8963Register(AK8963_CNTL);
    writeAK8963Register(AK8963_CNTL, temp & ~(0x0F) ); // Clear bits 0 - 3 to power down magnetometer  
    temp = readMPURegister(PWR_MGMT_1);
    writeMPURegister(PWR_MGMT_1, temp | 0x10);     // Write bit 4 to enable gyro standby
    delay(10); // Wait for all registers to reset 
}

void MPU9250::gyroMagWake(Mmode_t mmode)
{
    uint8_t temp = 0;
    temp = readAK8963Register(AK8963_CNTL);
    writeAK8963Register(AK8963_CNTL, temp | mmode ); // Reset normal mode for  magnetometer  
    temp = readMPURegister(PWR_MGMT_1);
    writeMPURegister(PWR_MGMT_1, 0x01);   // return gyro and accel normal mode
    delay(10); // Wait for all registers to reset 
}

void MPU9250::readMagnetometer(float & mx, float & my, float & mz)
{
    int16_t magCount[3];
    readMagData(magCount);

    // Calculate the magnetometer values in milliGauss
    // Include factory calibration per data sheet and user environmental corrections
    // Get actual magnetometer value, this depends on scale being set
    mx = (float)magCount[0]*_mRes*_magCalibration[0] - _magBias[0];  
    my = (float)magCount[1]*_mRes*_magCalibration[1] - _magBias[1];  
    mz = (float)magCount[2]*_mRes*_magCalibration[2] - _magBias[2];  
    mx *= _magScale[0];
    my *= _magScale[1];
    mz *= _magScale[2]; 
}

void MPU9250::readMagData(int16_t * destination)
{
    uint8_t rawData[7];  // x/y/z gyro register data, ST2 register stored here, must read ST2 at end of data acquisition
    readAK8963Registers(AK8963_XOUT_L, 7, &rawData[0]);  // Read the six raw data and ST2 registers sequentially into data array
    uint8_t c = rawData[6]; // End data read by reading ST2 register
    if(!(c & 0x08)) { // Check if magnetic sensor overflow set, if not then report data
        destination[0] = ((int16_t)rawData[1] << 8) | rawData[0] ;  // Turn the MSB and LSB into a signed 16-bit value
        destination[1] = ((int16_t)rawData[3] << 8) | rawData[2] ;  // Data stored as little Endian
        destination[2] = ((int16_t)rawData[5] << 8) | rawData[4] ; 
    }
}

void MPU9250::initAK8963(Mscale_t mscale, Mmode_t Mmode, float * magCalibration)
{
    // First extract the factory calibration for each magnetometer axis
    uint8_t rawData[3];  // x/y/z gyro calibration data stored here
    writeAK8963Register(AK8963_CNTL, 0x00); // Power down magnetometer  
    delay(10);
    writeAK8963Register(AK8963_CNTL, 0x0F); // Enter Fuse ROM access mode
    delay(10);
    readAK8963Registers(AK8963_ASAX, 3, &rawData[0]);  // Read the x-, y-, and z-axis calibration values
    magCalibration[0] =  (float)(rawData[0] - 128)/256.0f + 1.0f;   // Return x-axis sensitivity adjustment values, etc.
    magCalibration[1] =  (float)(rawData[1] - 128)/256.0f + 1.0f;  
    magCalibration[2] =  (float)(rawData[2] - 128)/256.0f + 1.0f; 
    _magCalibration[0] = magCalibration[0];
    _magCalibration[1] = magCalibration[1];
    _magCalibration[2] = magCalibration[2];
    _mMode = Mmode;
    writeAK8963Register(AK8963_CNTL, 0x00); // Power down magnetometer  
    delay(10);
    // Configure the magnetometer for continuous read and highest resolution
    // set Mscale bit 4 to 1 (0) to enable 16 (14) bit resolution in CNTL register,
    // and enable continuous mode data acquisition Mmode (bits [3:0]), 0010 for 8 Hz and 0110 for 100 Hz sample rates
    writeAK8963Register(AK8963_CNTL, mscale << 4 | Mmode); // Set magnetometer data resolution and sample ODR
    delay(10);
}
