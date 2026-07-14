#pragma once

#include <windows.h>
#include <vector>

class ModbusRTU
{
private:
    HANDLE hCom;

public:
    ModbusRTU();
    ~ModbusRTU();

    bool OpenPort(const char* portName);
    void ClosePort();

    bool ReadRegister(
        unsigned char slaveId,
        unsigned char functionCode,
        unsigned short address,
        unsigned short& value);

    bool ReadRange(
        unsigned char slaveId,
        unsigned char functionCode,
        unsigned short startAddr,
        unsigned short endAddr,
        std::vector<unsigned short>& values);

    bool ReadRegisters(
        unsigned char slaveId,
        unsigned char functionCode,
        unsigned short startAddr,
        unsigned short quantity,
        std::vector<unsigned short>& values);

private:
    unsigned short CRC16(
        const unsigned char* data,
        int length);

    void PrintHex(
        const unsigned char* data,
        int length,
        const char* prefix);
};