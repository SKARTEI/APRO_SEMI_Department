#include <windows.h>
#include <conio.h>
#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>

using namespace std;

struct ChillerState
{
    float currentTemp = 21.34f;
    float targetTemp = 34.57f;
    bool running = false;
};

ChillerState gState;

string ProcessCommand(const string& cmd)
{
    cout << "[RX] " << cmd << endl;

    //----------------------------------------
    // 현재온도 요청
    //----------------------------------------

    if (cmd == "in_pv_00")
    {
        ostringstream ss;

        ss << fixed
            << setprecision(2)
            << gState.currentTemp
            << "\r\n";

        return ss.str();
    }

    //----------------------------------------
    // 목표온도 요청
    //----------------------------------------

    if (cmd == "in_sp_00")
    {
        ostringstream ss;

        ss << fixed
            << setprecision(2)
            << gState.targetTemp
            << "\r\n";

        return ss.str();
    }

    //----------------------------------------
    // 상태 요청
    //----------------------------------------

    if (cmd == "status")
    {
        if (gState.running)
        {
            return "03 REMOTE START\r\n";
        }

        return "02 REMOTE STOP\r\n";
    }

    //----------------------------------------
    // 목표온도 설정
    //----------------------------------------

    if (cmd.find("out_sp_00") == 0)
    {
        string dummy;
        float value;

        stringstream ss(cmd);

        ss >> dummy;
        ss >> value;

        gState.targetTemp = value;

        cout << "[TARGET TEMP] "
            << fixed
            << setprecision(2)
            << gState.targetTemp
            << endl;

        return "";
    }

    //----------------------------------------
    // RUN / STOP
    //----------------------------------------

    if (cmd.find("out_mode_05") == 0)
    {
        string dummy;
        int value;

        stringstream ss(cmd);

        ss >> dummy;
        ss >> value;

        gState.running = (value == 1);

        cout << "[RUNNING] "
            << (gState.running ? "TRUE" : "FALSE")
            << endl;

        return "";
    }

    //----------------------------------------
    // 알 수 없는 명령
    //----------------------------------------

    cout << "[UNKNOWN] "
        << cmd
        << endl;

    return "";
}

int main()
{
    //----------------------------------------
    // COM PORT 입력
    //----------------------------------------

    string comNumber;

    cout << "==================================" << endl;
    cout << " PWT-PC usual response v0.1" << endl;
    cout << "==================================" << endl;
    cout << endl;

    cout << "COM Port Number : ";
    cin >> comNumber;

    string portName =
        "\\\\.\\COM" + comNumber;

    //----------------------------------------
    // COM OPEN
    //----------------------------------------

    HANDLE hSerial =
        CreateFileA(
            portName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

    if (hSerial == INVALID_HANDLE_VALUE)
    {
        cout << endl;
        cout << "COM OPEN FAIL" << endl;
        return -1;
    }

    //----------------------------------------
    // DCB 설정
    //----------------------------------------

    DCB dcb = { 0 };

    dcb.DCBlength = sizeof(DCB);

    if (!GetCommState(hSerial, &dcb))
    {
        cout << "GetCommState FAIL" << endl;
        CloseHandle(hSerial);
        return -1;
    }

    dcb.BaudRate = 19200;
    dcb.ByteSize = 7;
    dcb.Parity = EVENPARITY;
    dcb.StopBits = ONESTOPBIT;

    if (!SetCommState(hSerial, &dcb))
    {
        cout << "SetCommState FAIL" << endl;
        CloseHandle(hSerial);
        return -1;
    }

    //----------------------------------------
    // Timeout
    //----------------------------------------

    COMMTIMEOUTS timeouts = { 0 };

    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;

    SetCommTimeouts(hSerial, &timeouts);

    //----------------------------------------

    cout << endl;
    cout << "COM OPEN SUCCESS" << endl;
    cout << "ESC : PROGRAM EXIT" << endl;
    cout << "==================================" << endl;
    cout << endl;

    string rxBuffer;

    ULONGLONG lastUpdate = GetTickCount64();

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
                cout << endl;
                cout << "EXIT REQUEST" << endl;
                break;
            }
        }

        //------------------------------------
        // 온도 시뮬레이션
        //------------------------------------

        ULONGLONG now = GetTickCount64();

        if (now - lastUpdate >= 1000)
        {
            lastUpdate = now;

            if (gState.running)
            {
                if (gState.currentTemp < gState.targetTemp)
                {
                    gState.currentTemp += 0.10f;
                }
                else if (gState.currentTemp > gState.targetTemp)
                {
                    gState.currentTemp -= 0.10f;
                }
            }
        }

        //------------------------------------
        // 수신 처리
        //------------------------------------

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
                            ProcessCommand(rxBuffer);

                        if (!response.empty())
                        {
                            string display = response;

                            while (!display.empty() &&
                                (display.back() == '\r' ||
                                    display.back() == '\n'))
                            {
                                display.pop_back();
                            }

                            cout << "[TX] "
                                << display
                                << endl;

                            DWORD bytesWritten = 0;

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
                }
            }
        }

        Sleep(10);
    }

    //----------------------------------------
    // 종료 처리
    //----------------------------------------

    CloseHandle(hSerial);

    cout << "COM CLOSED" << endl;

    return 0;
}
