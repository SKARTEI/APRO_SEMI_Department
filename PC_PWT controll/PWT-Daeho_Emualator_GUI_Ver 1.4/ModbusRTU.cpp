#include "ModbusRTU.h"

ModbusRTU::ModbusRTU()
{
    // 생성자
    hCom = INVALID_HANDLE_VALUE;
    mLastException = 0;
    InitializeCriticalSection(&mCS);
}

ModbusRTU::~ModbusRTU()
{
    // 소멸자 - 포트 폐쇄
    ClosePort();
    DeleteCriticalSection(&mCS);
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

uint8_t ModbusRTU::GetLastExceptionCode() const
{
    return mLastException;
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

//--------------------------------------------------
// ReadExact: len 바이트를 타임아웃 내에 정확히 수신
//--------------------------------------------------

bool ModbusRTU::ReadExact(
    uint8_t* dst,
    int len)
{
    DWORD totalRead = 0;

    while (totalRead < (DWORD)len)
    {
        DWORD currentRead = 0;

        if (!ReadFile(
            hCom,
            dst + totalRead,
            len - totalRead,
            &currentRead,
            NULL))
        {
            return false;
        }

        if (currentRead == 0)
        {
            break;  // 타임아웃
        }

        totalRead += currentRead;
    }

    return (totalRead == (DWORD)len);
}

//--------------------------------------------------
// SendRequest
//   요청-응답 1회를 mCS로 직렬화.
//   두 스레드(대호 폴링 / PWT SV 쓰기)가 같은 포트에
//   동시 트랜잭션을 걸어 응답 바이트를 나눠 갖는
//   충돌을 차단한다. 겹칠 경우 뒤쪽 트랜잭션은
//   최대 수백 ms 대기 후 정상 수행됨.
//--------------------------------------------------

bool ModbusRTU::SendRequest(
    const uint8_t* tx,
    int txLength,
    uint8_t* rx,
    int expectedRxLength)
{
    EnterCriticalSection(&mCS);

    bool ok = SendRequestLocked(
        tx, txLength, rx, expectedRxLength);

    LeaveCriticalSection(&mCS);

    return ok;
}

//--------------------------------------------------
// SendRequestLocked (mCS 획득 상태에서만 호출)
//   1) 송신 전 수신버퍼 비움: 이전 트랜잭션의 지연 응답 등
//      잔류 바이트가 이번 응답으로 오독되는 것을 차단
//   2) 2단 읽기: 헤더 2바이트(ID+FC) 먼저 수신 후,
//      예외 프레임(5바이트)과 정상 프레임을 구분해 처리.
//      기존 방식은 예외 시 길이 불일치로만 실패해
//      예외 코드 확인 불가 + 타임아웃 300ms 전체 소모.
//--------------------------------------------------

bool ModbusRTU::SendRequestLocked(
    const uint8_t* tx,
    int txLength,
    uint8_t* rx,
    int expectedRxLength)
{
    mLastException = 0;

    // 송신 전 수신 잔류 바이트 제거
    PurgeComm(hCom, PURGE_RXCLEAR);

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

    // ── 1단계: 헤더 2바이트 (SlaveID + FunctionCode) ──
    if (!ReadExact(rx, 2))
    {
        return false;
    }

    // ── 예외 응답 (FC | 0x80): 총 5바이트 프레임 ──
    if (rx[1] & 0x80)
    {
        // 나머지 3바이트: 예외코드 1 + CRC 2
        if (!ReadExact(rx + 2, 3))
        {
            return false;
        }

        uint16_t crcRx =
            rx[3] |
            (rx[4] << 8);

        uint16_t crcCalc = CRC16(rx, 3);

        if (crcRx == crcCalc)
        {
            mLastException = rx[2];
        }

        return false;
    }

    // ── 2단계: 정상 프레임 나머지 수신 ──
    if (!ReadExact(rx + 2, expectedRxLength - 2))
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

    // 예외 응답은 SendRequest 내부에서 처리됨

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

    // 예외 응답은 SendRequest 내부에서 처리됨

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

    // 예외 응답은 SendRequest 내부에서 처리됨

    return true;
}
