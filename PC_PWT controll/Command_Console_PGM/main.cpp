#include <windows.h>
#include <conio.h>
#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <atomic>

#include "ModbusRTU.h"

using namespace std;

//--------------------------------------------------
// 전역 변수들
//--------------------------------------------------

ModbusRTU gModbus;


// 스레드 동기화 처리 고려하여 아토믹
atomic<double> gPV = -99.00;
atomic<double> gSV = -99.00;

atomic<bool> gProgramRunning = true;
atomic<bool> gModbusConnected = false;
atomic<int> gModbusFailCount = 0;

//--------------------------------------------------
// ProcessCommand
//--------------------------------------------------

string ProcessCommand(const string& cmd)
{
    cout << "[RX] " << cmd << endl;

    // 현재온도

    if (cmd == "in_pv_00")
    {
        ostringstream ss;

        ss << fixed
            << setprecision(2)
            << gPV
            << "\r\n";

        return ss.str();
    }

    // 목표온도 요청

    if (cmd == "in_sp_00")
    {
        ostringstream ss;

        ss << fixed
            << setprecision(2)
            << gSV
            << "\r\n";

        return ss.str();
    }

    //----------------------------------------
    // 상태 요청 (속임수)
    //----------------------------------------

    if (cmd == "status")
    {
        return "03 REMOTE START\r\n";
    }

    //----------------------------------------
    // 목표온도 설정
    //----------------------------------------

    if (cmd.find("out_sp_00") == 0)
    {
        string dummy;
        double value;

        stringstream ss(cmd);

        ss >> dummy;

        if (!(ss >> value))
        {
            cout
                << "[INVALID SV COMMAND]"
                << endl;

            return "";
        }

        //----------------------------------
        // 온도 범위 제한
        //----------------------------------

        if (value < 5.0 || value > 90.0)
        {
            cout
                << "[SV RANGE ERROR] "
                << value
                << endl;

            return "";
        }

        //----------------------------------
        // FC06 Write
        //----------------------------------

        uint16_t rawValue =
            static_cast<uint16_t>(
                value * 10.0);

        if (gModbus.WriteSingleRegister(
            1,
            20,
            rawValue))
        {
            gSV = value;

            cout
                << "[SV WRITE OK] "
                << fixed
                << setprecision(2)
                << value
                << endl;
        }
        else
        {
            cout
                << "[SV WRITE FAIL]"
                << endl;
        }

        return "";
    }

    //----------------------------------------
    // RUN / STOP
    //----------------------------------------

    if (cmd.find("out_mode_05") == 0)
    {
        cout
            << "[IGNORED] RUN/STOP COMMAND"
            << endl;

        return "";
    }

    //----------------------------------------
    // 알 수 없는 명령
    //----------------------------------------

    cout
        << "[UNKNOWN COMMAND] "
        << cmd
        << endl;

    return "";
}

// DAEHO POLLING THREAD
DWORD WINAPI DaehoPollingThread(LPVOID)
{
    while (gProgramRunning)
    {
        bool pvOk = false;
        bool svOk = false;

        uint16_t value;

        //----------------------------------
        // PV
        // FC04 Addr 20
        // Raw 2513 = 25.13℃
        // 현재값은 소숫점 둘째자리
        //----------------------------------

        if (gModbus.ReadInputRegister(
            1,
            20,
            value))
        {
            gPV =
                static_cast<double>(value)
                / 100.0;

            pvOk = true;
        }

        //----------------------------------
        // SV
        // FC03 Addr 20
        // Raw 250 = 25.0℃
        // 목표값은 소숫점 첫째자리
        //----------------------------------

        if (gModbus.ReadHoldingRegister(
            1,
            20,
            value))
        {
            gSV =
                static_cast<double>(value)
                / 10.0;

            svOk = true;
        }

        //----------------------------------
        // 상태 관리
        //----------------------------------

        if (pvOk && svOk)
        {
            gModbusConnected = true;
        }
        else
        {
            gModbusConnected = false;

            gModbusFailCount++;
        }

        Sleep(1000);
    }

    return 0;
}

//--------------------------------------------------
// MAIN
//--------------------------------------------------

int main()
{
    // COM 입력
    string pwtCom;
    string daehoCom;

    cout << "==================================" << endl;
    cout << " PWT-DAEHO JULABO EMULATOR" << endl;
    cout << "==================================" << endl;
    cout << endl;

    cout << "PWT COM Port Number : ";
    cin >> pwtCom;

    cout << "DAEHO COM Port Number : ";
    cin >> daehoCom;

    string pwtPort =
        "\\\\.\\COM" + pwtCom;

    string daehoPort =
        "\\\\.\\COM" + daehoCom;

    // PWT COM OPEN
    HANDLE hSerial =
        CreateFileA(
            pwtPort.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

    if (hSerial == INVALID_HANDLE_VALUE)
    {
        cout << endl;
        cout << "PWT COM OPEN FAIL" << endl;

        return -1;
    }

    //----------------------------------------
    // PWT 통신 타입 정의
    // 대호냉각기는 곁다리 cpp에 들어가있음
    //----------------------------------------

    DCB dcb = { 0 };

    dcb.DCBlength = sizeof(DCB);

    if (!GetCommState(
        hSerial,
        &dcb))
    {
        cout
            << "GetCommState FAIL"
            << endl;

        CloseHandle(hSerial);

        return -1;
    }

    dcb.BaudRate = 19200;
    dcb.ByteSize = 7;
    dcb.Parity = EVENPARITY;
    dcb.StopBits = ONESTOPBIT;

    if (!SetCommState(
        hSerial,
        &dcb))
    {
        cout
            << "SetCommState FAIL"
            << endl;

        CloseHandle(hSerial);

        return -1;
    }

    // TIMEOUT
    COMMTIMEOUTS timeouts = { 0 };

    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;

    SetCommTimeouts(
        hSerial,
        &timeouts);

    //----------------------------------------
    // DAEHO OPEN
    //----------------------------------------

    if (!gModbus.OpenPort(
        daehoPort.c_str()))
    {
        cout << endl;
        cout << "DAEHO COM OPEN FAIL"
            << endl;

        CloseHandle(hSerial);

        return -1;
    }

    //----------------------------------------
    // THREAD START
    //----------------------------------------

    HANDLE hPollingThread =
        CreateThread(
            nullptr,
            0,
            DaehoPollingThread,
            nullptr,
            0,
            nullptr);

    if (hPollingThread == nullptr)
    {
        cout
            << "THREAD CREATE FAIL"
            << endl;

        gModbus.ClosePort();

        CloseHandle(hSerial);

        return -1;
    }

    //----------------------------------------
    // START
    //----------------------------------------

    cout << endl;

    cout
        << "PWT COM OPEN SUCCESS"
        << endl;

    cout
        << "DAEHO COM OPEN SUCCESS"
        << endl;

    cout << endl;

    cout
        << "ESC : PROGRAM EXIT"
        << endl;

    cout
        << "=================================="
        << endl;

    string rxBuffer;

    ULONGLONG lastDisplay =
        GetTickCount64();

    //----------------------------------------
    // MAIN LOOP
    //----------------------------------------

    while (true)
    {
        //------------------------------------
        // ESC 종료
        //------------------------------------

        if (_kbhit())
        {
            int key = _getch();

            if (key == 27)
            {
                break;
            }
        }

        //------------------------------------
        // 상태 출력
        //------------------------------------

        ULONGLONG now =
            GetTickCount64();

        if (now - lastDisplay >= 1000)
        {
            lastDisplay = now;

            cout << endl;

            if (gPV < -100)
            {
                cout
                    << "PV=---- ";
            }
            else
            {
                cout
                    << "PV="
                    << fixed
                    << setprecision(2)
                    << gPV
                    << " ";
            }

            if (gSV < -100)
            {
                cout
                    << "SV=---- ";
            }
            else
            {
                cout
                    << "SV="
                    << fixed
                    << setprecision(2)
                    << gSV
                    << " ";
            }

            cout
                << "MODBUS="
                << (gModbusConnected ?
                    "OK" :
                    "FAIL")
                << " ";

            cout
                << "FAIL="
                << gModbusFailCount
                << endl;
        }


        // PWT 수신
        char ch;

        DWORD bytesRead = 0;

        if (ReadFile(
            hSerial,
            &ch,
            1,
            &bytesRead,
            nullptr))
        {
            if (bytesRead > 0)
            {
                if (ch == '\n')
                {
                    continue;
                }

                if (ch == '\r')
                {
                    if (!rxBuffer.empty())
                    {
                        string response =
                            ProcessCommand(
                                rxBuffer);

                        if (!response.empty())
                        {
                            DWORD bytesWritten =
                                0;

                            WriteFile(
                                hSerial,
                                response.c_str(),
                                (DWORD)response.size(),
                                &bytesWritten,
                                nullptr);
                        }
                    }

                    rxBuffer.clear();
                }
                else
                {
                    rxBuffer += ch;

                    // RX Buffer Overflow Protection
                    if (rxBuffer.size() > 256)
                    {
                        cout
                            << "[RX BUFFER OVERFLOW]"
                            << endl;

                        rxBuffer.clear();

                        PurgeComm(
                            hSerial,
                            PURGE_RXCLEAR);
                    }
                }
            }
        }

        Sleep(10);
    }

    //----------------------------------------
    // 종료 액션
    //----------------------------------------

    cout << endl;
    cout << "PROGRAM EXIT" << endl;

    gProgramRunning = false;

    WaitForSingleObject(
        hPollingThread,
        3000);

    CloseHandle(
        hPollingThread);

    gModbus.ClosePort();

    CloseHandle(
        hSerial);

    return 0;
}