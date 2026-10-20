#include <windows.h>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>   // malloc, free
#include <cstring>   // strcmp, strcpy_s
#include <ctime>
#include "ModbusRTU.h"

using namespace std;

//--------------------------------------------------
// Forward declarations
//--------------------------------------------------

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
string ProcessCommand(const string& cmd);
DWORD WINAPI DaehoPollingThread(LPVOID);
DWORD WINAPI PwtRxThread(LPVOID);
void AppendLog(const wchar_t* msg);
void AppendLogf(const wchar_t* fmt, ...);
void WriteFailLog(const char* reason, double pv, double sv);
void WritePvLog(double pv, double sv, const char* eventTag);

// Graph.cpp 모듈 (별도 헤더 없이 forward 선언)
void GraphInit();
void GraphShutdown();
void GraphRegisterClass(HINSTANCE hInst);
HWND GraphCreate(HWND parent, int x, int y, int w, int h);
void PushGraphData(double pv, double sv);

//--------------------------------------------------
// Custom messages
//--------------------------------------------------

// 워커 스레드 → UI 스레드 로그 전달 (비동기)
// lParam = malloc된 wchar_t* (수신 측에서 free)
#define WM_APP_LOG (WM_APP + 1)

//--------------------------------------------------
// Constants
//--------------------------------------------------

constexpr ULONGLONG PWT_TIMEOUT_MS = 5000;
constexpr ULONGLONG DAEHO_TIMEOUT_MS = 5000;

// PV 정기 기록 주기: 10초
// (냉각기 1주기 3.5~30초 → 튐/일관성을 보려면 10초 샘플이 적절)
constexpr ULONGLONG PV_LOG_INTERVAL_MS = 10ULL * 1000ULL;

//--------------------------------------------------
// Global state
//--------------------------------------------------

ModbusRTU gModbus;

HANDLE gPwtSerial = INVALID_HANDLE_VALUE;
HANDLE gPollingThread = NULL;
HANDLE gPwtThread = NULL;

atomic<bool>      gProgramRunning = true;
atomic<double>    gPV = -99.0;
atomic<double>    gSV = -99.0;
atomic<int>       gModbusFailCount = 0;
atomic<ULONGLONG> gLastPwtCommandTime(0);
atomic<ULONGLONG> gLastDaehoSuccessTime(0);
atomic<ULONGLONG> gConnectTime(0);

HWND gMainWnd = NULL;   // 로그 PostMessage 대상

//--------------------------------------------------
// Logging state (B안: fail / pv 분리 + 일자 자동 분할)
//   Logs\fail_YYYYMMDD.csv : 이슈 상시 기록
//   Logs\pv_YYYYMMDD.csv   : PV LOG ON 구간에만 기록
//   장기(2~14일) 구동 시 자정마다 새 파일 자동 생성
//--------------------------------------------------

atomic<bool>      gPvLogEnabled = false;
atomic<bool>      gPvLogFailNotified = false;
atomic<ULONGLONG> gLastPvLogWriteTime(0);

CRITICAL_SECTION gFailLogCS;
CRITICAL_SECTION gPvLogCS;

char gFailLogDate[16] = {};   // 현재 열린 fail 파일 날짜 (YYYYMMDD)
char gPvLogDate[16] = {};     // 현재 열린 pv 파일 날짜
char gFailLogFile[MAX_PATH] = {};
char gPvLogFile[MAX_PATH] = {};

//--------------------------------------------------
// UI handles
//--------------------------------------------------

HWND hEditPwtCom = NULL;
HWND hEditDaehoCom = NULL;
HWND hPwtStatus = NULL;
HWND hDaehoStatus = NULL;
HWND hCurrentTemp = NULL;
HWND hTargetTemp = NULL;
HWND hFailCount = NULL;
HWND hUptime = NULL;
HWND hLastPwtRx = NULL;
HWND hConnectBtn = NULL;
HWND hPvLogBtn = NULL;
HWND hLogEdit = NULL;
HWND hFooter = NULL;

//--------------------------------------------------
// GDI resources
//--------------------------------------------------

HFONT  hFontLabel = NULL;
HFONT  hFontNormal = NULL;
HFONT  hFontTemp = NULL;
HFONT  hFontStatus = NULL;
HFONT  hFontFooter = NULL;
HBRUSH hBrushBg = NULL;

//--------------------------------------------------
// Time helpers
//--------------------------------------------------

static void GetLocalTimeStr(char* buf, int size)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_s(&t, &now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &t);
}

static void GetLogDateStr(char* buf, int size)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_s(&t, &now);
    strftime(buf, size, "%Y%m%d", &t);
}

static void EnsureLogDir()
{
    CreateDirectoryA("Logs", NULL);
}

//--------------------------------------------------
// 일자별 파일 확보 (날짜 바뀌면 새 파일 + 헤더 생성)
//   호출 측에서 해당 CS를 이미 잡은 상태여야 함
//--------------------------------------------------

static bool EnsureFailLogFile()
{
    EnsureLogDir();

    char today[16];
    GetLogDateStr(today, sizeof(today));

    if (strcmp(gFailLogDate, today) != 0)
    {
        strcpy_s(gFailLogDate, sizeof(gFailLogDate), today);
        sprintf_s(gFailLogFile, sizeof(gFailLogFile),
            "Logs\\fail_%s.csv", today);

        FILE* f = NULL;
        fopen_s(&f, gFailLogFile, "a");
        if (!f) return false;

        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0)
            fprintf(f, "TIMESTAMP,REASON,PV,SV\n");
        fclose(f);
    }
    return true;
}

static bool EnsurePvLogFile()
{
    EnsureLogDir();

    char today[16];
    GetLogDateStr(today, sizeof(today));

    if (strcmp(gPvLogDate, today) != 0)
    {
        strcpy_s(gPvLogDate, sizeof(gPvLogDate), today);
        sprintf_s(gPvLogFile, sizeof(gPvLogFile),
            "Logs\\pv_%s.csv", today);

        FILE* f = NULL;
        fopen_s(&f, gPvLogFile, "a");
        if (!f) return false;

        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0)
            fprintf(f, "TIMESTAMP,PV,SV,EVENT\n");
        fclose(f);
    }
    return true;
}

//--------------------------------------------------
// 이슈 로그 (상시). 무효값은 NA 기록.
//--------------------------------------------------

void WriteFailLog(const char* reason, double pv, double sv)
{
    EnterCriticalSection(&gFailLogCS);

    if (EnsureFailLogFile())
    {
        FILE* f = NULL;
        fopen_s(&f, gFailLogFile, "a");
        if (f)
        {
            char ts[32];
            GetLocalTimeStr(ts, sizeof(ts));

            fprintf(f, "%s,%s,", ts, reason ? reason : "");
            if (pv > -90.0) fprintf(f, "%.2f,", pv); else fprintf(f, "NA,");
            if (sv > -90.0) fprintf(f, "%.2f\n", sv); else fprintf(f, "NA\n");
            fclose(f);
        }
    }

    LeaveCriticalSection(&gFailLogCS);
}

//--------------------------------------------------
// PV 정기 기록 (PV LOG ON 구간). 무효값은 NA.
//   행 단위 open/append/close → 비정상 종료에도 직전 행 보존.
//   기록 실패는 무음 처리하지 않고 세션당 1회 로그 통지.
//--------------------------------------------------

void WritePvLog(double pv, double sv, const char* eventTag)
{
    bool ok = false;

    EnterCriticalSection(&gPvLogCS);

    if (EnsurePvLogFile())
    {
        FILE* f = NULL;
        fopen_s(&f, gPvLogFile, "a");
        if (f)
        {
            char ts[32];
            GetLocalTimeStr(ts, sizeof(ts));

            fprintf(f, "%s,", ts);
            if (pv > -90.0) fprintf(f, "%.2f,", pv); else fprintf(f, "NA,");
            if (sv > -90.0) fprintf(f, "%.2f,", sv); else fprintf(f, "NA,");
            fprintf(f, "%s\n", eventTag ? eventTag : "");
            fclose(f);
            ok = true;
        }
    }

    LeaveCriticalSection(&gPvLogCS);

    if (!ok && !gPvLogFailNotified.exchange(true))
        AppendLog(L"[PVLOG] WRITE FAIL - 경로/권한/용량 확인");
}

//--------------------------------------------------
// AppendLog: 어느 스레드에서든 호출 가능 (PostMessage 큐)
//   종료 시 WaitForSingleObject와 상호 대기(교착)를 피하려
//   SendMessage가 아닌 비동기 Post 사용.
//--------------------------------------------------

void AppendLog(const wchar_t* msg)
{
    if (!gMainWnd) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t* line = (wchar_t*)malloc(512 * sizeof(wchar_t));
    if (!line) return;

    swprintf(line, 512,
        L"[%02d:%02d:%02d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, msg);

    if (!PostMessageW(gMainWnd, WM_APP_LOG, 0, (LPARAM)line))
        free(line);
}

//--------------------------------------------------
// AppendLogf: printf 형식 로그 (가변인자)
//   반복되던 swprintf+AppendLog 3줄을 1줄로 축약.
//--------------------------------------------------

void AppendLogf(const wchar_t* fmt, ...)
{
    wchar_t buf[480];

    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 480, fmt, ap);
    va_end(ap);

    AppendLog(buf);
}

//--------------------------------------------------
// AppendLogUi: UI 스레드 전용 (WM_APP_LOG 핸들러)
//--------------------------------------------------

static void AppendLogUi(const wchar_t* line)
{
    if (!hLogEdit) return;

    int len = GetWindowTextLengthW(hLogEdit);
    if (len > 16000)
    {
        SetWindowTextW(hLogEdit, L"");
        len = 0;
    }

    SendMessageW(hLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)line);
}

//--------------------------------------------------
// SetTextIfChanged: 값이 실제로 바뀐 경우에만 SetWindowText
//--------------------------------------------------

static void SetTextIfChanged(HWND h, const wchar_t* s)
{
    wchar_t cur[64] = {};
    GetWindowTextW(h, cur, 64);
    if (wcscmp(cur, s) != 0)
        SetWindowTextW(h, s);
}

//--------------------------------------------------
// Helpers: font / set font / create window
//--------------------------------------------------

static HFONT MakeFont(int height, int weight, const wchar_t* face)
{
    return CreateFontW(
        height, 0, 0, 0, weight,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

static void SF(HWND h, HFONT f)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

static HWND CW(
    const wchar_t* cls, const wchar_t* txt,
    DWORD style, int x, int y, int w, int h,
    HWND parent, HMENU id)
{
    return CreateWindowW(cls, txt, style,
        x, y, w, h, parent, id, NULL, NULL);
}

//--------------------------------------------------
// Thread: DaehoPolling
//--------------------------------------------------

DWORD WINAPI DaehoPollingThread(LPVOID)
{
    while (gProgramRunning)
    {
        bool pvOk = false;
        bool svOk = false;
        uint16_t value;

        if (gModbus.ReadInputRegister(1, 20, value))
        {
            gPV = static_cast<double>(value) / 100.0;
            pvOk = true;
        }

        if (gModbus.ReadHoldingRegister(1, 20, value))
        {
            gSV = static_cast<double>(value) / 10.0;
            svOk = true;
        }

        if (pvOk && svOk)
        {
            gModbusFailCount = 0;
            gLastDaehoSuccessTime = GetTickCount64();
        }
        else
        {
            int prev = gModbusFailCount.fetch_add(1);

            if (prev == 0)
            {
                uint8_t ex = gModbus.GetLastExceptionCode();
                char reason[40];
                if (ex)
                    sprintf_s(reason, "DAEHO_MODBUS_FAIL_EX%02X", ex);
                else
                    strcpy_s(reason, "DAEHO_MODBUS_FAIL");

                WriteFailLog(reason, gPV.load(), gSV.load());
                AppendLog(L"[DAEHO] MODBUS FAIL");

                if (gPvLogEnabled)
                    WritePvLog(gPV.load(), gSV.load(), "COMM_FAIL");
            }
        }

        Sleep(1000);
    }

    return 0;
}

//--------------------------------------------------
// ProcessCommand
//--------------------------------------------------

string ProcessCommand(const string& cmd)
{
    if (cmd == "in_pv_00")
    {
        ostringstream ss;
        ss << fixed << setprecision(2) << gPV.load() << "\r\n";
        return ss.str();
    }

    if (cmd == "in_sp_00")
    {
        ostringstream ss;
        ss << fixed << setprecision(2) << gSV.load() << "\r\n";
        return ss.str();
    }

    if (cmd == "status")
        return "03 REMOTE START\r\n";

    if (cmd.find("out_sp_00") == 0)
    {
        string dummy;
        double value;
        stringstream ss(cmd);
        ss >> dummy;

        if (!(ss >> value))
        {
            AppendLog(L"[SV] INVALID COMMAND");
            return "";
        }

        if (value < 5.0 || value > 90.0)
        {
            AppendLogf(L"[SV] RANGE ERROR: %.2f", value);
            return "";
        }

        // 반올림 (+0.5) — 버림 방지
        uint16_t raw = static_cast<uint16_t>(value * 10.0 + 0.5);

        if (gModbus.WriteSingleRegister(1, 20, raw))
        {
            gSV = value;
            AppendLogf(L"[SV] SET OK: %.2f", value);

            if (gPvLogEnabled)
                WritePvLog(gPV.load(), value, "SV_SET");
        }
        else
        {
            AppendLogf(L"[SV] WRITE FAIL: %.2f", value);
            WriteFailLog("SV_WRITE_FAIL", gPV.load(), value);
        }

        return "";
    }

    if (cmd.find("out_mode_05") == 0)
    {
        AppendLog(L"[MODE] RUN/STOP IGNORED");
        return "";
    }

    {
        wchar_t wbuf[128] = {};
        MultiByteToWideChar(CP_ACP, 0, cmd.c_str(), -1, wbuf, 128);
        AppendLogf(L"[UNKNOWN] %s", wbuf);
    }

    return "";
}

//--------------------------------------------------
// Thread: PwtRx
//--------------------------------------------------

DWORD WINAPI PwtRxThread(LPVOID)
{
    string rxBuffer;

    while (gProgramRunning)
    {
        char ch = 0;
        DWORD bytesRead = 0;

        if (ReadFile(gPwtSerial, &ch, 1, &bytesRead, nullptr))
        {
            if (bytesRead > 0)
            {
                gLastPwtCommandTime = GetTickCount64();

                if (ch == '\n') continue;

                if (ch == '\r')
                {
                    if (!rxBuffer.empty())
                    {
                        string resp = ProcessCommand(rxBuffer);
                        if (!resp.empty())
                        {
                            DWORD written = 0;
                            WriteFile(gPwtSerial,
                                resp.c_str(), (DWORD)resp.size(),
                                &written, nullptr);
                        }
                    }
                    rxBuffer.clear();
                }
                else
                {
                    rxBuffer += ch;
                    if (rxBuffer.size() > 256)
                    {
                        rxBuffer.clear();
                        PurgeComm(gPwtSerial, PURGE_RXCLEAR);
                    }
                }

                // 수신 중에는 Sleep 생략 → 연속 바이트 즉시 처리
                continue;
            }
        }
        else
        {
            DWORD err = GetLastError();
            if (err != ERROR_INVALID_HANDLE &&
                err != ERROR_OPERATION_ABORTED)
            {
                WriteFailLog("PWT_PORT_ERROR", gPV.load(), gSV.load());
            }
            break;
        }

        Sleep(10);
    }

    return 0;
}

//--------------------------------------------------
// WndProc
//--------------------------------------------------

LRESULT CALLBACK WndProc(
    HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {

        //--------------------------------------------------
    case WM_CREATE:
        //--------------------------------------------------
    {
        gMainWnd = hwnd;

        hFontLabel = MakeFont(16, FW_BOLD, L"Segoe UI");
        hFontNormal = MakeFont(15, FW_NORMAL, L"Segoe UI");
        hFontTemp = MakeFont(34, FW_BOLD, L"Segoe UI");
        hFontStatus = MakeFont(15, FW_SEMIBOLD, L"Segoe UI");
        hFontFooter = MakeFont(15, FW_BOLD, L"Segoe UI");

        hBrushBg = CreateSolidBrush(RGB(245, 245, 245));

        //======= LEFT COLUMN (x=10, w=245) =======

        // -- PWT --
        HWND hGrpPwt = CW(L"BUTTON", L"PWT",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 8, 245, 150, hwnd, NULL);
        SF(hGrpPwt, hFontLabel);

        CW(L"STATIC", L"COM Port", WS_CHILD | WS_VISIBLE,
            24, 36, 70, 18, hwnd, NULL);
        hEditPwtCom = CW(L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            104, 33, 131, 24, hwnd, (HMENU)1001);
        SF(hEditPwtCom, hFontNormal);

        CW(L"STATIC", L"Status", WS_CHILD | WS_VISIBLE,
            24, 72, 70, 18, hwnd, NULL);
        hPwtStatus = CW(L"STATIC", L"DISCONNECTED",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            104, 69, 131, 26, hwnd, (HMENU)4001);
        SF(hPwtStatus, hFontStatus);

        CW(L"STATIC", L"Last RX", WS_CHILD | WS_VISIBLE,
            24, 110, 70, 18, hwnd, NULL);
        hLastPwtRx = CW(L"STATIC", L"--", WS_CHILD | WS_VISIBLE,
            104, 110, 131, 20, hwnd, NULL);
        SF(hLastPwtRx, hFontNormal);

        // -- DAEHO --
        HWND hGrpDaeho = CW(L"BUTTON", L"DAEHO",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 166, 245, 140, hwnd, NULL);
        SF(hGrpDaeho, hFontLabel);

        CW(L"STATIC", L"COM Port", WS_CHILD | WS_VISIBLE,
            24, 192, 70, 18, hwnd, NULL);
        hEditDaehoCom = CW(L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            104, 189, 131, 24, hwnd, (HMENU)1002);
        SF(hEditDaehoCom, hFontNormal);

        CW(L"STATIC", L"Status", WS_CHILD | WS_VISIBLE,
            24, 226, 70, 18, hwnd, NULL);
        hDaehoStatus = CW(L"STATIC", L"DISCONNECTED",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            104, 223, 131, 26, hwnd, (HMENU)4002);
        SF(hDaehoStatus, hFontStatus);

        CW(L"STATIC", L"Fail Count", WS_CHILD | WS_VISIBLE,
            24, 264, 70, 18, hwnd, NULL);
        hFailCount = CW(L"STATIC", L"0", WS_CHILD | WS_VISIBLE,
            104, 264, 131, 20, hwnd, NULL);
        SF(hFailCount, hFontNormal);

        // -- LOG (좌하단) --
        HWND hGrpLog = CW(L"BUTTON", L"LOG",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 314, 245, 258, hwnd, NULL);
        SF(hGrpLog, hFontLabel);

        hLogEdit = CW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            18, 334, 229, 230, hwnd, (HMENU)3001);
        SF(hLogEdit, hFontNormal);

        //======= RIGHT COLUMN (x=265, w=550) =======

        // -- TEMPERATURE (PV | SV 가로 배치) --
        HWND hGrpTemp = CW(L"BUTTON", L"TEMPERATURE",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            265, 8, 550, 115, hwnd, NULL);
        SF(hGrpTemp, hFontLabel);

        CW(L"STATIC", L"CURRENT (PV)", WS_CHILD | WS_VISIBLE,
            280, 32, 180, 18, hwnd, NULL);
        hCurrentTemp = CW(L"STATIC", L"---- \u00B0C",
            WS_CHILD | WS_VISIBLE,
            280, 54, 258, 50, hwnd, NULL);
        SF(hCurrentTemp, hFontTemp);

        CW(L"STATIC", L"TARGET (SV)", WS_CHILD | WS_VISIBLE,
            546, 32, 180, 18, hwnd, NULL);
        hTargetTemp = CW(L"STATIC", L"---- \u00B0C",
            WS_CHILD | WS_VISIBLE,
            546, 54, 258, 50, hwnd, NULL);
        SF(hTargetTemp, hFontTemp);

        // -- SYSTEM (위로 이동) --
        HWND hGrpSys = CW(L"BUTTON", L"SYSTEM",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            265, 131, 550, 105, hwnd, NULL);
        SF(hGrpSys, hFontLabel);

        CW(L"STATIC", L"Uptime", WS_CHILD | WS_VISIBLE,
            280, 158, 60, 18, hwnd, NULL);
        hUptime = CW(L"STATIC", L"--:--:--", WS_CHILD | WS_VISIBLE,
            346, 158, 200, 20, hwnd, NULL);
        SF(hUptime, hFontNormal);

        hConnectBtn = CW(L"BUTTON", L"CONNECT",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            280, 184, 330, 44, hwnd, (HMENU)2001);
        SF(hConnectBtn, hFontLabel);

        hPvLogBtn = CW(L"BUTTON", L"PV LOG: OFF",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            620, 184, 187, 44, hwnd, (HMENU)2002);
        SF(hPvLogBtn, hFontLabel);

        // -- GRAPH (빈 공간만큼 크게) --
        HWND hGrpGraph = CW(L"BUTTON", L"GRAPH",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            265, 244, 550, 328, hwnd, NULL);
        SF(hGrpGraph, hFontLabel);

        GraphInit();
        GraphRegisterClass(GetModuleHandleW(NULL));
        GraphCreate(hwnd, 273, 264, 534, 300);

        // -- 마킹 (최하단) --
        hFooter = CW(L"STATIC",
            L"SW 전반 개발 및 UI디자인: 반도체팀 나윤상 연구원  ||  Version 1.4 260721 Release",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 580, 825, 20, hwnd, NULL);
        SF(hFooter, hFontFooter);

        InitializeCriticalSection(&gFailLogCS);
        InitializeCriticalSection(&gPvLogCS);

        SetTimer(hwnd, 1, 1000, NULL);
        return 0;
    }

    //--------------------------------------------------
    case WM_APP_LOG:
        //--------------------------------------------------
    {
        wchar_t* line = (wchar_t*)lParam;
        if (line)
        {
            AppendLogUi(line);
            free(line);
        }
        return 0;
    }

    //--------------------------------------------------
    case WM_COMMAND:
        //--------------------------------------------------
    {
        int id = LOWORD(wParam);

        // -- PV LOG 토글 --
        if (id == 2002)
        {
            if (!gPvLogEnabled)
            {
                // 날짜 강제 초기화 → EnsurePvLogFile이 실제 파일을
                // 다시 열어 쓰기 가능 여부를 검증하게 함
                bool ok;
                EnterCriticalSection(&gPvLogCS);
                gPvLogDate[0] = 0;
                ok = EnsurePvLogFile();
                LeaveCriticalSection(&gPvLogCS);

                if (!ok)
                {
                    AppendLog(L"[PVLOG] FILE CREATE FAIL - 시작 불가");
                    MessageBoxW(hwnd,
                        L"PV 로그 파일을 생성할 수 없습니다.\n"
                        L"실행 폴더의 쓰기 권한을 확인하세요.",
                        L"PV LOG", MB_ICONERROR | MB_OK);
                    break;
                }

                gPvLogFailNotified = false;
                gLastPvLogWriteTime = 0;   // 즉시 첫 기록
                gPvLogEnabled = true;
                SetWindowTextW(hPvLogBtn, L"PV LOG: ON");
                AppendLogf(L"[PVLOG] START: %S", gPvLogFile);
            }
            else
            {
                gPvLogEnabled = false;
                SetWindowTextW(hPvLogBtn, L"PV LOG: OFF");
                AppendLog(L"[PVLOG] STOP");
            }
            break;
        }

        if (id != 2001) break;

        if (gPwtSerial != INVALID_HANDLE_VALUE)
        {
            MessageBoxW(hwnd, L"Already connected.", L"INFO", MB_OK);
            break;
        }

        wchar_t pwtNum[16] = {};
        wchar_t daehoNum[16] = {};
        GetWindowTextW(hEditPwtCom, pwtNum, 16);
        GetWindowTextW(hEditDaehoCom, daehoNum, 16);

        wstring pwtPort = wstring(L"\\\\.\\COM") + pwtNum;
        wstring daehoPort = wstring(L"\\\\.\\COM") + daehoNum;

        // PWT OPEN
        gPwtSerial = CreateFileW(pwtPort.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);

        if (gPwtSerial == INVALID_HANDLE_VALUE)
        {
            MessageBoxW(hwnd, L"PWT COM OPEN FAIL", L"ERROR", MB_ICONERROR);
            break;
        }

        DCB dcb = { 0 };
        dcb.DCBlength = sizeof(DCB);

        if (!GetCommState(gPwtSerial, &dcb))
        {
            MessageBoxW(hwnd, L"GetCommState FAIL", L"ERROR", MB_ICONERROR);
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
            break;
        }

        dcb.BaudRate = 19200;
        dcb.ByteSize = 7;
        dcb.Parity = EVENPARITY;
        dcb.StopBits = ONESTOPBIT;

        if (!SetCommState(gPwtSerial, &dcb))
        {
            MessageBoxW(hwnd, L"SetCommState FAIL", L"ERROR", MB_ICONERROR);
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
            break;
        }

        COMMTIMEOUTS to = { 0 };
        to.ReadIntervalTimeout = 50;
        to.ReadTotalTimeoutConstant = 50;
        to.ReadTotalTimeoutMultiplier = 10;

        if (!SetCommTimeouts(gPwtSerial, &to))
        {
            MessageBoxW(hwnd, L"SetCommTimeouts FAIL", L"ERROR", MB_ICONERROR);
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
            break;
        }

        // DAEHO OPEN
        {
            string daehoPortA(daehoPort.begin(), daehoPort.end());
            if (!gModbus.OpenPort(daehoPortA.c_str()))
            {
                MessageBoxW(hwnd, L"DAEHO COM OPEN FAIL", L"ERROR", MB_ICONERROR);
                CloseHandle(gPwtSerial);
                gPwtSerial = INVALID_HANDLE_VALUE;
                break;
            }
        }

        gPollingThread = CreateThread(nullptr, 0,
            DaehoPollingThread, nullptr, 0, nullptr);

        if (!gPollingThread)
        {
            MessageBoxW(hwnd, L"POLLING THREAD FAIL", L"ERROR", MB_ICONERROR);
            gModbus.ClosePort();
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
            break;
        }

        gPwtThread = CreateThread(nullptr, 0,
            PwtRxThread, nullptr, 0, nullptr);

        if (!gPwtThread)
        {
            MessageBoxW(hwnd, L"PWT THREAD FAIL", L"ERROR", MB_ICONERROR);
            gProgramRunning = false;
            WaitForSingleObject(gPollingThread, 3000);
            CloseHandle(gPollingThread);
            gPollingThread = NULL;
            gProgramRunning = true;
            gModbus.ClosePort();
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
            break;
        }

        {
            ULONGLONG now = GetTickCount64();
            gConnectTime = now;
            gLastPwtCommandTime = now;
            gLastDaehoSuccessTime = now;
            gModbusFailCount = 0;
        }

        EnableWindow(hConnectBtn, FALSE);
        EnableWindow(hEditPwtCom, FALSE);
        EnableWindow(hEditDaehoCom, FALSE);

        SetWindowTextW(hPwtStatus, L"CONNECTED");
        SetWindowTextW(hDaehoStatus, L"CONNECTED");

        AppendLogf(L"[CONNECT] PWT=COM%s  DAEHO=COM%s", pwtNum, daehoNum);
        MessageBoxW(hwnd, L"COM OPEN SUCCESS", L"INFO", MB_OK);
        break;
    }

    //--------------------------------------------------
    case WM_TIMER:
        //--------------------------------------------------
    {
        double pv = gPV.load();
        double sv = gSV.load();

        // PV: 텍스트 캐시 + SV 변화 감지 → 색상 갱신 위해 재설정
        {
            static wchar_t pvCache[32] = {};
            static double  svForColor = -999.0;

            wchar_t buf[32];
            if (pv < -90.0) wcscpy_s(buf, L"---- \u00B0C");
            else swprintf(buf, 32, L"%.2f \u00B0C", pv);

            if (wcscmp(pvCache, buf) != 0 || sv != svForColor)
            {
                SetWindowTextW(hCurrentTemp, buf);
                wcscpy_s(pvCache, buf);
                svForColor = sv;
            }
        }

        // SV
        {
            wchar_t buf[32];
            if (sv < -90.0) wcscpy_s(buf, L"---- \u00B0C");
            else swprintf(buf, 32, L"%.2f \u00B0C", sv);
            SetTextIfChanged(hTargetTemp, buf);
        }

        // Fail Count
        {
            wchar_t buf[16];
            swprintf(buf, 16, L"%d", gModbusFailCount.load());
            SetTextIfChanged(hFailCount, buf);
        }

        // DAEHO STATUS
        {
            ULONGLONG now = GetTickCount64();
            ULONGLONG last = gLastDaehoSuccessTime.load();
            bool open = gModbus.IsConnected();

            if (!open) SetTextIfChanged(hDaehoStatus, L"DISCONNECTED");
            else if (last == 0 || now - last > DAEHO_TIMEOUT_MS)
                SetTextIfChanged(hDaehoStatus, L"NO SIGNAL");
            else SetTextIfChanged(hDaehoStatus, L"CONNECTED");
        }

        // PWT STATUS
        {
            ULONGLONG now = GetTickCount64();
            ULONGLONG last = gLastPwtCommandTime.load();
            bool open = (gPwtSerial != INVALID_HANDLE_VALUE);

            if (!open) SetTextIfChanged(hPwtStatus, L"DISCONNECTED");
            else if (last == 0 || now - last > PWT_TIMEOUT_MS)
                SetTextIfChanged(hPwtStatus, L"NO SIGNAL");
            else SetTextIfChanged(hPwtStatus, L"CONNECTED");
        }

        // Uptime
        {
            ULONGLONG ct = gConnectTime.load();
            if (ct == 0) SetTextIfChanged(hUptime, L"--:--:--");
            else
            {
                ULONGLONG s = (GetTickCount64() - ct) / 1000;
                wchar_t buf[32];
                swprintf(buf, 32, L"%02llu:%02llu:%02llu",
                    s / 3600, (s % 3600) / 60, s % 60);
                SetWindowTextW(hUptime, buf);
            }
        }

        // Last PWT RX
        {
            ULONGLONG last = gLastPwtCommandTime.load();
            if (last == 0) SetTextIfChanged(hLastPwtRx, L"--");
            else
            {
                ULONGLONG s = (GetTickCount64() - last) / 1000;
                wchar_t buf[32];
                swprintf(buf, 32, L"%llu sec ago", s);
                SetTextIfChanged(hLastPwtRx, buf);
            }
        }

        // 그래프 + PV 정기 기록 (연결 후)
        if (gConnectTime.load() != 0)
        {
            PushGraphData(pv, sv);

            if (gPvLogEnabled && pv > -90.0)
            {
                ULONGLONG now = GetTickCount64();
                ULONGLONG last = gLastPvLogWriteTime.load();
                if (last == 0 || now - last >= PV_LOG_INTERVAL_MS)
                {
                    WritePvLog(pv, sv, "");
                    gLastPvLogWriteTime = now;
                }
            }
        }

        return 0;
    }

    //--------------------------------------------------
    case WM_CTLCOLORSTATIC:
        //--------------------------------------------------
    {
        HWND hCtrl = (HWND)lParam;
        HDC  hdc = (HDC)wParam;

        if (hCtrl == hPwtStatus || hCtrl == hDaehoStatus)
        {
            wchar_t txt[32] = {};
            GetWindowTextW(hCtrl, txt, 32);
            SetBkMode(hdc, TRANSPARENT);

            if (wcscmp(txt, L"CONNECTED") == 0)
                SetTextColor(hdc, RGB(34, 139, 34));
            else if (wcscmp(txt, L"NO SIGNAL") == 0)
                SetTextColor(hdc, RGB(210, 105, 0));
            else
                SetTextColor(hdc, RGB(200, 30, 30));

            return (LRESULT)hBrushBg;
        }

        // PV: SV와 비교해 색상 결정
        //   PV > SV → 적색 / PV < SV → 청색 / 근접·무효 → 중립
        if (hCtrl == hCurrentTemp)
        {
            double pv = gPV.load();
            double sv = gSV.load();
            SetBkMode(hdc, TRANSPARENT);

            if (pv < -90.0 || sv < -90.0)
                SetTextColor(hdc, RGB(70, 70, 70));
            else if (pv > sv + 0.05)
                SetTextColor(hdc, RGB(200, 30, 30));
            else if (pv < sv - 0.05)
                SetTextColor(hdc, RGB(0, 90, 200));
            else
                SetTextColor(hdc, RGB(30, 140, 60));

            return (LRESULT)hBrushBg;
        }

        if (hCtrl == hTargetTemp)
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(70, 70, 70));
            return (LRESULT)hBrushBg;
        }

        if (hCtrl == hFooter)
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(40, 70, 130));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    //--------------------------------------------------
    case WM_DESTROY:
        //--------------------------------------------------
    {
        KillTimer(hwnd, 1);
        gMainWnd = NULL;
        gProgramRunning = false;

        if (gPwtSerial != INVALID_HANDLE_VALUE)
        {
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
        }

        if (gPwtThread)
        {
            WaitForSingleObject(gPwtThread, 3000);
            CloseHandle(gPwtThread);
        }

        if (gPollingThread)
        {
            WaitForSingleObject(gPollingThread, 3000);
            CloseHandle(gPollingThread);
        }

        gModbus.ClosePort();
        GraphShutdown();

        if (hFontLabel)  DeleteObject(hFontLabel);
        if (hFontNormal) DeleteObject(hFontNormal);
        if (hFontTemp)   DeleteObject(hFontTemp);
        if (hFontStatus) DeleteObject(hFontStatus);
        if (hFontFooter) DeleteObject(hFontFooter);
        if (hBrushBg)    DeleteObject(hBrushBg);

        DeleteCriticalSection(&gFailLogCS);
        DeleteCriticalSection(&gPvLogCS);

        PostQuitMessage(0);
        return 0;
    }

    } // switch

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//==================================================
// 비밀번호 다이얼로그 (오용 방지 목적)
//==================================================

static const wchar_t* CORRECT_PASSWORD = L"768902";

#define ID_PWD_EDIT   5001
#define ID_PWD_OK     5002
#define ID_PWD_CANCEL 5003

static bool gPasswordOk = false;

LRESULT CALLBACK PasswordProc(
    HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        CreateWindowW(L"STATIC", L"PWT-Daeho Gateway",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 18, 280, 24, hwnd, NULL, NULL, NULL);

        CreateWindowW(L"STATIC", L"비밀번호를 입력하세요.",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 48, 280, 20, hwnd, NULL, NULL, NULL);

        HWND hEdit = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
            60, 78, 160, 24, hwnd, (HMENU)ID_PWD_EDIT, NULL, NULL);
        SetFocus(hEdit);

        CreateWindowW(L"BUTTON", L"확인",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            55, 118, 75, 28, hwnd, (HMENU)ID_PWD_OK, NULL, NULL);

        CreateWindowW(L"BUTTON", L"취소",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            150, 118, 75, 28, hwnd, (HMENU)ID_PWD_CANCEL, NULL, NULL);
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        if (id == ID_PWD_OK)
        {
            wchar_t input[64] = {};
            GetWindowTextW(GetDlgItem(hwnd, ID_PWD_EDIT), input, 64);

            if (wcscmp(input, CORRECT_PASSWORD) == 0)
            {
                gPasswordOk = true;
                DestroyWindow(hwnd);
            }
            else
            {
                MessageBoxW(hwnd, L"비밀번호가 올바르지 않습니다.",
                    L"오류", MB_ICONERROR | MB_OK);
                SetWindowTextW(GetDlgItem(hwnd, ID_PWD_EDIT), L"");
                SetFocus(GetDlgItem(hwnd, ID_PWD_EDIT));
            }
        }
        else if (id == ID_PWD_CANCEL)
        {
            gPasswordOk = false;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//==================================================
// WinMain
//==================================================

int WINAPI WinMain(
    HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    //-------- 비밀번호 다이얼로그 --------
    const wchar_t PWD_CLASS[] = L"PWD_DIALOG";

    WNDCLASSW pwdWc = {};
    pwdWc.lpfnWndProc = PasswordProc;
    pwdWc.hInstance = hInstance;
    pwdWc.lpszClassName = PWD_CLASS;
    pwdWc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&pwdWc);

    HWND hPwd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, PWD_CLASS,
        L"PWT-Daeho Gateway — 인증",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 296, 192,
        NULL, NULL, hInstance, NULL);

    if (!hPwd) return 0;

    ShowWindow(hPwd, SW_SHOW);
    UpdateWindow(hPwd);

    MSG msg = {};

    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (msg.message == WM_KEYDOWN)
        {
            if (msg.wParam == VK_RETURN)
            {
                SendMessageW(hPwd, WM_COMMAND, ID_PWD_OK, 0);
                continue;
            }
            else if (msg.wParam == VK_ESCAPE)
            {
                SendMessageW(hPwd, WM_COMMAND, ID_PWD_CANCEL, 0);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (!gPasswordOk) return 0;

    //-------- 메인 창 --------
    const wchar_t CLASS_NAME[] = L"PWT_DAEHO_GATEWAY";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"PWT-Daeho Gateway v1.4",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 843, 648,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}