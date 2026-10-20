#pragma once

#include <windows.h>
#include <stdint.h>

class ModbusRTU
{
public:

    ModbusRTU();

    ~ModbusRTU();

    bool OpenPort(const char* portName);

    void ClosePort();

    bool IsConnected() const;

    // 직전 트랜잭션에서 슬레이브가 반환한 Modbus 예외 코드
    // (예외 없었으면 0. 진단·fail_log 사유 병기용)
    uint8_t GetLastExceptionCode() const;


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

    uint8_t mLastException;

    // 트랜잭션 직렬화: DaehoPollingThread(읽기)와
    // PwtRxThread(SV 쓰기)가 같은 포트를 공유하므로
    // 요청-응답 1회가 원자적으로 수행되어야 함
    CRITICAL_SECTION mCS;

    uint16_t CRC16(
        const uint8_t* data,
        int length);

    // len 바이트를 타임아웃 내에 정확히 수신 (부족 시 false)
    bool ReadExact(
        uint8_t* dst,
        int len);

    // 잠금 획득 후 실제 트랜잭션 수행 (SendRequest 내부 전용)
    bool SendRequestLocked(
        const uint8_t* tx,
        int txLength,
        uint8_t* rx,
        int expectedRxLength);

    bool SendRequest(
        const uint8_t* tx,
        int txLength,
        uint8_t* rx,
        int expectedRxLength);
};
