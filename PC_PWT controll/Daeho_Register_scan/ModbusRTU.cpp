#include "ModbusRTU.h"

#include <iostream>
#include <iomanip>

using namespace std;

ModbusRTU::ModbusRTU()
{
    hCom = INVALID_HANDLE_VALUE;
}

ModbusRTU::~ModbusRTU()
{
    ClosePort();
}

bool ModbusRTU::OpenPort(const char* portName)
{
    hCom = CreateFileA(
        portName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (hCom == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DCB dcb;
    ZeroMemory(&dcb, sizeof(dcb));

    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(hCom, &dcb))
    {
        return false;
    }

    dcb.BaudRate = CBR_9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;

    if (!SetCommState(hCom, &dcb))
    {
        return false;
    }

    COMMTIMEOUTS timeouts;

    ZeroMemory(&timeouts, sizeof(timeouts));

    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 300;
    timeouts.ReadTotalTimeoutMultiplier = 0;

    timeouts.WriteTotalTimeoutConstant = 300;
    timeouts.WriteTotalTimeoutMultiplier = 0;

    SetCommTimeouts(hCom, &timeouts);

    PurgeComm(
        hCom,
        PURGE_RXCLEAR |
        PURGE_TXCLEAR);

    return true;
}

void ModbusRTU::ClosePort()
{
    if (hCom != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hCom);
        hCom = INVALID_HANDLE_VALUE;
    }
}

unsigned short ModbusRTU::CRC16(
    const unsigned char* data,
    int length)
{
    unsigned short crc = 0xFFFF;

    for (int i = 0; i < length; i++)
    {
        crc ^= data[i];

        for (int j = 0; j < 8; j++)
        {
            if (crc & 0x0001)
            {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}

void ModbusRTU::PrintHex(
    const unsigned char* data,
    int length,
    const char* prefix)
{
    cout << prefix << " ";

    for (int i = 0; i < length; i++)
    {
        cout
            << uppercase
            << hex
            << setw(2)
            << setfill('0')
            << (int)data[i]
            << " ";
    }

    cout << dec << endl;
}

bool ModbusRTU::ReadRegister(
    unsigned char slaveId,
    unsigned char functionCode,
    unsigned short address,
    unsigned short& value)
{
    unsigned char tx[8];

    tx[0] = slaveId;
    tx[1] = functionCode;

    tx[2] = (address >> 8) & 0xFF;
    tx[3] = address & 0xFF;

    tx[4] = 0x00;
    tx[5] = 0x01;

    unsigned short crc = CRC16(tx, 6);

    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;

    PrintHex(tx, 8, "TX:");

    DWORD bytesWritten = 0;

    if (!WriteFile(
        hCom,
        tx,
        8,
        &bytesWritten,
        NULL))
    {
        return false;
    }

    unsigned char rx[256];

    DWORD bytesRead = 0;

    if (!ReadFile(
        hCom,
        rx,
        sizeof(rx),
        &bytesRead,
        NULL))
    {
        return false;
    }

    if (bytesRead == 0)
    {
        return false;
    }

    PrintHex(rx, bytesRead, "RX:");

    if (bytesRead < 5)
    {
        return false;
    }

    if (rx[1] & 0x80)
    {
        cout << endl;
        cout << "MODBUS EXCEPTION : ";

        switch (rx[2])
        {
        case 1:
            cout << "Illegal Function";
            break;

        case 2:
            cout << "Illegal Data Address";
            break;

        case 3:
            cout << "Illegal Data Value";
            break;

        case 4:
            cout << "Slave Device Failure";
            break;

        default:
            cout << (int)rx[2];
            break;
        }

        cout << endl;

        return false;
    }

    if (bytesRead < 7)
    {
        return false;
    }

    value =
        ((unsigned short)rx[3] << 8)
        | rx[4];

    return true;
}

bool ModbusRTU::ReadRange(
    unsigned char slaveId,
    unsigned char functionCode,
    unsigned short startAddr,
    unsigned short endAddr,
    vector<unsigned short>& values)
{
    values.clear();

    for (unsigned short addr = startAddr;
        addr <= endAddr;
        addr++)
    {
        unsigned short value;

        if (ReadRegister(
            slaveId,
            functionCode,
            addr,
            value))
        {
            values.push_back(value);
        }
        else
        {
            values.push_back(0xFFFF);
        }
    }

    return true;
}

bool ModbusRTU::ReadRegisters(
    unsigned char slaveId,
    unsigned char functionCode,
    unsigned short startAddr,
    unsigned short quantity,
    vector<unsigned short>& values)
{
    values.clear();

    unsigned char tx[8];

    tx[0] = slaveId;
    tx[1] = functionCode;

    tx[2] = (startAddr >> 8) & 0xFF;
    tx[3] = startAddr & 0xFF;

    tx[4] = (quantity >> 8) & 0xFF;
    tx[5] = quantity & 0xFF;

    unsigned short crc = CRC16(tx, 6);

    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;

    PrintHex(tx, 8, "TX:");

    DWORD bytesWritten = 0;

    if (!WriteFile(
        hCom,
        tx,
        8,
        &bytesWritten,
        NULL))
    {
        return false;
    }

    unsigned char rx[512];

    DWORD bytesRead = 0;

    if (!ReadFile(
        hCom,
        rx,
        sizeof(rx),
        &bytesRead,
        NULL))
    {
        return false;
    }

    if (bytesRead < 5)
    {
        return false;
    }

    PrintHex(rx, bytesRead, "RX:");

    if (rx[1] & 0x80)
    {
        return false;
    }

    int byteCount = rx[2];

    for (int i = 0; i < byteCount; i += 2)
    {
        unsigned short value =
            ((unsigned short)rx[3 + i] << 8)
            | rx[4 + i];

        values.push_back(value);
    }

    return true;
}