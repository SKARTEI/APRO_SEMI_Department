#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdint.h>

class ModbusRTU
{
public:

    ModbusRTU();

    ~ModbusRTU();

    bool OpenPort(const char* portName);

    void ClosePort();

    bool IsConnected() const;


    // FC03 Read Holding Register
    bool ReadHoldingRegister(
        uint8_t slaveId,
        uint16_t address,
        uint16_t& value);


    // FC04 Read Input Register
    bool ReadInputRegister(
        uint8_t slaveId,
        uint16_t address,
        uint16_t& value);


    // FC06 Write Single Register
    bool WriteSingleRegister(
        uint8_t slaveId,
        uint16_t address,
        uint16_t value);

private:

    HANDLE hCom;

    uint16_t CRC16(
        const uint8_t* data,
        int length);

    bool SendRequest(
        const uint8_t* tx,
        int txLength,
        uint8_t* rx,
        int expectedRxLength);
};