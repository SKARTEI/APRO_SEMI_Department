#include "ModbusRTU.h"

ModbusRTU::ModbusRTU()
{
    // 생성자
    hCom = INVALID_HANDLE_VALUE;
}

ModbusRTU::~ModbusRTU()
{
    // 소멸자 - 포트 폐쇄
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

    //----------------------------------------
    // 대호냉각기 통신 타입 정의
    // PWT는 main에 있음
    //----------------------------------------    
    DCB dcb;

    ZeroMemory(&dcb, sizeof(dcb));

    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(hCom, &dcb))
    {
        ClosePort();
        return false;
    }

    dcb.BaudRate = CBR_9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;

    if (!SetCommState(hCom, &dcb))
    {
        ClosePort();
        return false;
    }

    COMMTIMEOUTS timeouts;

    ZeroMemory(&timeouts, sizeof(timeouts));

    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 300;
    timeouts.ReadTotalTimeoutMultiplier = 0;

    timeouts.WriteTotalTimeoutConstant = 300;
    timeouts.WriteTotalTimeoutMultiplier = 0;

    if (!SetCommTimeouts(hCom, &timeouts))
    {
        ClosePort();
        return false;
    }

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

bool ModbusRTU::IsConnected() const
{
    return (hCom != INVALID_HANDLE_VALUE);
}

uint16_t ModbusRTU::CRC16(
    const uint8_t* data,
    int length)
{
    uint16_t crc = 0xFFFF;

    for (int i = 0; i < length; i++)
    {
        crc ^= data[i];

        for (int j = 0; j < 8; j++)
        {
            if (crc & 1)
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

bool ModbusRTU::SendRequest(
    const uint8_t* tx,
    int txLength,
    uint8_t* rx,
    int expectedRxLength)
{
    DWORD written = 0;

    if (!WriteFile(
        hCom,
        tx,
        txLength,
        &written,
        NULL))
    {
        return false;
    }

    DWORD totalRead = 0;

    while (totalRead < (DWORD)expectedRxLength)
    {
        DWORD currentRead = 0;

        if (!ReadFile(
            hCom,
            rx + totalRead,
            expectedRxLength - totalRead,
            &currentRead,
            NULL))
        {
            return false;
        }

        if (currentRead == 0)
        {
            break;
        }

        totalRead += currentRead;
    }

    if (totalRead != (DWORD)expectedRxLength)
    {
        return false;
    }

    uint16_t crcRx =
        rx[expectedRxLength - 2] |
        (rx[expectedRxLength - 1] << 8);

    uint16_t crcCalc =
        CRC16(
            rx,
            expectedRxLength - 2);

    if (crcRx != crcCalc)
    {
        return false;
    }

    return true;
}

bool ModbusRTU::ReadHoldingRegister(
    uint8_t slaveId,
    uint16_t address,
    uint16_t& value)
{
    uint8_t tx[8];

    tx[0] = slaveId;
    tx[1] = 0x03;

    tx[2] = (address >> 8) & 0xFF;
    tx[3] = address & 0xFF;

    tx[4] = 0x00;
    tx[5] = 0x01;

    uint16_t crc = CRC16(tx, 6);

    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;

    uint8_t rx[7];

    if (!SendRequest(
        tx,
        8,
        rx,
        7))
    {
        return false;
    }

    if (rx[1] & 0x80)
    {
        return false;
    }

    value =
        ((uint16_t)rx[3] << 8) |
        rx[4];

    return true;
}

bool ModbusRTU::ReadInputRegister(
    uint8_t slaveId,
    uint16_t address,
    uint16_t& value)
{
    uint8_t tx[8];

    tx[0] = slaveId;
    tx[1] = 0x04;

    tx[2] = (address >> 8) & 0xFF;
    tx[3] = address & 0xFF;

    tx[4] = 0x00;
    tx[5] = 0x01;

    uint16_t crc = CRC16(tx, 6);

    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;

    uint8_t rx[7];

    if (!SendRequest(
        tx,
        8,
        rx,
        7))
    {
        return false;
    }

    if (rx[1] & 0x80)
    {
        return false;
    }

    value =
        ((uint16_t)rx[3] << 8) |
        rx[4];

    return true;
}

bool ModbusRTU::WriteSingleRegister(
    uint8_t slaveId,
    uint16_t address,
    uint16_t value)
{
    uint8_t tx[8];

    tx[0] = slaveId;
    tx[1] = 0x06;

    tx[2] = (address >> 8) & 0xFF;
    tx[3] = address & 0xFF;

    tx[4] = (value >> 8) & 0xFF;
    tx[5] = value & 0xFF;

    uint16_t crc = CRC16(tx, 6);

    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;

    uint8_t rx[8];

    if (!SendRequest(
        tx,
        8,
        rx,
        8))
    {
        return false;
    }

    if (rx[1] & 0x80)
    {
        return false;
    }

    return true;
}