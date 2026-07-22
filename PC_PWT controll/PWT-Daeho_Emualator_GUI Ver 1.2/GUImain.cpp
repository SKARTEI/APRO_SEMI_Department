#include <windows.h>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <ctime>
#include "ModbusRTU.h"

using namespace std;

//--------------------------------------------------
// Forward declarations
//--------------------------------------------------

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK GraphProc(HWND, UINT, WPARAM, LPARAM);
string ProcessCommand(const string& cmd);
DWORD WINAPI DaehoPollingThread(LPVOID);
DWORD WINAPI PwtRxThread(LPVOID);
void AppendLog(const wchar_t* msg);
void WriteFailLog(const char* reason, double pv, double sv);
void PushGraphData(double pv, double sv);

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
atomic<bool>      gModbusConnected = false;
atomic<int>       gModbusFailCount = 0;
atomic<ULONGLONG> gLastPwtCommandTime(0);
atomic<ULONGLONG> gLastDaehoSuccessTime(0);
atomic<ULONGLONG> gConnectTime(0);

constexpr ULONGLONG PWT_TIMEOUT_MS = 5000;
constexpr ULONGLONG DAEHO_TIMEOUT_MS = 5000;

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
HWND hLogEdit = NULL;
HWND hGraphPanel = NULL;  // 그래프 전용 패널

//--------------------------------------------------
// Graph data
//--------------------------------------------------

// 최대 6000포인트 = 100분 × 60초
constexpr int GRAPH_MAX_POINTS = 6000;

double gGraphPV[GRAPH_MAX_POINTS] = {};
double gGraphSV[GRAPH_MAX_POINTS] = {};
int    gGraphCount = 0;  // 현재 저장된 포인트 수

CRITICAL_SECTION gGraphCS;  // 그래프 버퍼 보호

//--------------------------------------------------
// GDI resources
//--------------------------------------------------

HFONT  hFontLabel = NULL;
HFONT  hFontNormal = NULL;
HFONT  hFontTemp = NULL;
HFONT  hFontStatus = NULL;
HBRUSH hBrushBg = NULL;

//--------------------------------------------------
// Helpers
//--------------------------------------------------

static void GetLocalTimeStr(char* buf, int size)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_s(&t, &now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &t);
}

void WriteFailLog(const char* reason, double pv, double sv)
{
    FILE* f = NULL;
    fopen_s(&f, "fail_log.csv", "a");
    if (!f) return;

    // 파일 크기가 0이면 헤더 추가
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)
        fprintf(f, "TIMESTAMP,REASON,PV,SV\n");

    char ts[32];
    GetLocalTimeStr(ts, sizeof(ts));
    fprintf(f, "%s,%s,%.2f,%.2f\n", ts, reason, pv, sv);
    fclose(f);
}

void AppendLog(const wchar_t* msg)
{
    if (!hLogEdit) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[512];
    swprintf(line, 512,
        L"[%02d:%02d:%02d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, msg);

    int len = GetWindowTextLengthW(hLogEdit);

    if (len > 10000)
    {
        SetWindowTextW(hLogEdit, L"");
        len = 0;
    }

    SendMessageW(hLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)line);
}

//--------------------------------------------------
// Graph: data push (1초마다 WM_TIMER에서 호출)
//--------------------------------------------------

void PushGraphData(double pv, double sv)
{
    EnterCriticalSection(&gGraphCS);

    if (gGraphCount < GRAPH_MAX_POINTS)
    {
        gGraphPV[gGraphCount] = pv;
        gGraphSV[gGraphCount] = sv;
        gGraphCount++;
    }
    else
    {
        // 가장 오래된 1포인트 제거 후 추가 (좌로 밀기)
        memmove(gGraphPV, gGraphPV + 1,
            (GRAPH_MAX_POINTS - 1) * sizeof(double));
        memmove(gGraphSV, gGraphSV + 1,
            (GRAPH_MAX_POINTS - 1) * sizeof(double));
        gGraphPV[GRAPH_MAX_POINTS - 1] = pv;
        gGraphSV[GRAPH_MAX_POINTS - 1] = sv;
    }

    LeaveCriticalSection(&gGraphCS);

    if (hGraphPanel)
        InvalidateRect(hGraphPanel, NULL, FALSE);
}

//--------------------------------------------------
// Graph panel window procedure
//--------------------------------------------------

LRESULT CALLBACK GraphProc(
    HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hdcWindow = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        // ── 더블 버퍼링: 메모리 DC에 전부 그린 후 한 번에 화면 복사 ──
        HDC hdc = CreateCompatibleDC(hdcWindow);
        HBITMAP hMemBmp = CreateCompatibleBitmap(
            hdcWindow, rc.right, rc.bottom);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hMemBmp);

        // 전체 배경 흰색
        HBRUSH hWhite = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, hWhite);
        DeleteObject(hWhite);

        // 플롯 영역 여백: 좌35 우10 상15 하30
        const int ML = 35, MR = 10, MT = 15, MB = 30;
        int pw = rc.right - ML - MR;  // 플롯 폭
        int ph = rc.bottom - MT - MB;  // 플롯 높이

        // 온도 범위
        const double T_MIN = 15.0;
        const double T_MAX = 85.0;
        const double T_RANGE = T_MAX - T_MIN;

        // X 좌표: 항상 GRAPH_MAX_POINTS 고정 스케일
        // 인덱스 0 = 좌측 끝, GRAPH_MAX_POINTS-1 = 우측 끝
        // 데이터가 cnt개면 우측 cnt칸만 채워지고 좌측은 빈 공간
        auto TempToY = [&](double t) -> int {
            double ratio = (t - T_MIN) / T_RANGE;
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;
            return MT + ph - (int)(ratio * ph);
            };

        // 고정 스케일: idx는 버퍼 내 실제 인덱스
        // 버퍼가 cnt개면 우측 끝(GRAPH_MAX_POINTS-1)에 맞춰 오프셋
        // offset = GRAPH_MAX_POINTS - cnt
        auto IdxToX = [&](int idx, int offset) -> int {
            int scaledIdx = idx + offset;
            return ML + (int)((double)scaledIdx / (GRAPH_MAX_POINTS - 1) * pw);
            };

        // ── 점선 그리기 ──
        HPEN hDotPen = CreatePen(PS_DOT, 1, RGB(180, 180, 180));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hDotPen);
        SetBkMode(hdc, TRANSPARENT);

        // 가로 점선: 15°C 간격 (15, 30, 45, 60, 75°C)
        double gridTemps[] = { 15.0, 30.0, 45.0, 60.0, 75.0 };
        for (double gt : gridTemps)
        {
            int y = TempToY(gt);
            MoveToEx(hdc, ML, y, NULL);
            LineTo(hdc, ML + pw, y);

            wchar_t lbl[8];
            swprintf(lbl, 8, L"%d", (int)gt);
            SetTextColor(hdc, RGB(120, 120, 120));
            RECT lr = { 0, y - 8, ML - 2, y + 8 };
            HFONT hSmall = CreateFontW(11, 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT hOldF = (HFONT)SelectObject(hdc, hSmall);
            DrawTextW(hdc, lbl, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hOldF);
            DeleteObject(hSmall);
        }

        // 세로 점선: 20분=1200포인트 간격 (우측 끝 기준 역산, 고정 스케일)
        EnterCriticalSection(&gGraphCS);
        int cnt = gGraphCount;
        LeaveCriticalSection(&gGraphCS);

        {
            const int MIN20 = 1200;
            int offset = GRAPH_MAX_POINTS - cnt;  // 우측 정렬 오프셋

            for (int back = MIN20; back < GRAPH_MAX_POINTS; back += MIN20)
            {
                // 우측 끝(GRAPH_MAX_POINTS-1)에서 back만큼 앞
                int scaledIdx = (GRAPH_MAX_POINTS - 1) - back;
                if (scaledIdx < 0) break;
                int x = ML + (int)((double)scaledIdx / (GRAPH_MAX_POINTS - 1) * pw);
                MoveToEx(hdc, x, MT, NULL);
                LineTo(hdc, x, MT + ph);

                int minAgo = back / 60;
                wchar_t tlbl[12];
                swprintf(tlbl, 12, L"-%dm", minAgo);
                RECT tr = { x - 20, MT + ph + 2, x + 20, MT + ph + 18 };
                HFONT hSmall = CreateFontW(11, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT hOldF = (HFONT)SelectObject(hdc, hSmall);
                SetTextColor(hdc, RGB(120, 120, 120));
                DrawTextW(hdc, tlbl, -1, &tr, DT_CENTER | DT_SINGLELINE);
                SelectObject(hdc, hOldF);
                DeleteObject(hSmall);
            }
        }

        SelectObject(hdc, hOldPen);
        DeleteObject(hDotPen);

        // 테두리
        HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
        SelectObject(hdc, hBorderPen);
        MoveToEx(hdc, ML, MT, NULL);
        LineTo(hdc, ML + pw, MT);
        LineTo(hdc, ML + pw, MT + ph);
        LineTo(hdc, ML, MT + ph);
        LineTo(hdc, ML, MT);
        SelectObject(hdc, GetStockObject(NULL_PEN));
        DeleteObject(hBorderPen);

        // ── 데이터 플롯 ──
        EnterCriticalSection(&gGraphCS);

        if (cnt > 1)
        {
            int offset = GRAPH_MAX_POINTS - cnt;  // 우측 정렬 오프셋

            // PV: 흑색
            HPEN hPvPen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
            SelectObject(hdc, hPvPen);

            bool pvStarted = false;
            for (int i = 0; i < cnt; i++)
            {
                double v = gGraphPV[i];
                if (v < -90.0) { pvStarted = false; continue; }
                int x = IdxToX(i, offset);
                int y = TempToY(v);
                if (!pvStarted) { MoveToEx(hdc, x, y, NULL); pvStarted = true; }
                else LineTo(hdc, x, y);
            }
            SelectObject(hdc, GetStockObject(NULL_PEN));
            DeleteObject(hPvPen);

            // SV: 적색
            HPEN hSvPen = CreatePen(PS_SOLID, 2, RGB(210, 30, 30));
            SelectObject(hdc, hSvPen);

            bool svStarted = false;
            for (int i = 0; i < cnt; i++)
            {
                double v = gGraphSV[i];
                if (v < -90.0) { svStarted = false; continue; }
                int x = IdxToX(i, offset);
                int y = TempToY(v);
                if (!svStarted) { MoveToEx(hdc, x, y, NULL); svStarted = true; }
                else LineTo(hdc, x, y);
            }
            SelectObject(hdc, GetStockObject(NULL_PEN));
            DeleteObject(hSvPen);
        }

        LeaveCriticalSection(&gGraphCS);

        // 범례: 우측 상단
        {
            HFONT hLgFont = CreateFontW(12, 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT hOldF = (HFONT)SelectObject(hdc, hLgFont);

            // PV 범례
            HPEN hPvL = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
            SelectObject(hdc, hPvL);
            MoveToEx(hdc, ML + pw - 65, MT + 8, NULL);
            LineTo(hdc, ML + pw - 48, MT + 8);
            SelectObject(hdc, GetStockObject(NULL_PEN));
            DeleteObject(hPvL);
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT rPv = { ML + pw - 46, MT + 2, ML + pw, MT + 16 };
            DrawTextW(hdc, L"PV", -1, &rPv, DT_LEFT | DT_SINGLELINE);

            // SV 범례
            HPEN hSvL = CreatePen(PS_SOLID, 2, RGB(210, 30, 30));
            SelectObject(hdc, hSvL);
            MoveToEx(hdc, ML + pw - 65, MT + 22, NULL);
            LineTo(hdc, ML + pw - 48, MT + 22);
            SelectObject(hdc, GetStockObject(NULL_PEN));
            DeleteObject(hSvL);
            SetTextColor(hdc, RGB(210, 30, 30));
            RECT rSv = { ML + pw - 46, MT + 16, ML + pw, MT + 30 };
            DrawTextW(hdc, L"SV", -1, &rSv, DT_LEFT | DT_SINGLELINE);

            SelectObject(hdc, hOldF);
            DeleteObject(hLgFont);
        }

        // ── 메모리 DC 내용을 화면에 한 번에 복사 ──
        BitBlt(hdcWindow, 0, 0, rc.right, rc.bottom,
            hdc, 0, 0, SRCCOPY);

        SelectObject(hdc, hOldBmp);
        DeleteObject(hMemBmp);
        DeleteDC(hdc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    if (msg == WM_ERASEBKGND)
        return 1; // 깜빡임 방지

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//--------------------------------------------------
// Helper: create font
//--------------------------------------------------

static HFONT MakeFont(int height, int weight, const wchar_t* face)
{
    return CreateFontW(
        height, 0, 0, 0, weight,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        face);
}

//--------------------------------------------------
// Helper: set font on window
//--------------------------------------------------

static void SF(HWND h, HFONT f)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

//--------------------------------------------------
// Helper: CreateWindowW shorthand
//--------------------------------------------------

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
            gModbusConnected = true;
            gModbusFailCount = 0;
            gLastDaehoSuccessTime = GetTickCount64();
        }
        else
        {
            gModbusConnected = false;

            int prev = gModbusFailCount.fetch_add(1);

            if (prev == 0)
            {
                WriteFailLog("DAEHO_MODBUS_FAIL",
                    gPV.load(), gSV.load());
                AppendLog(L"[DAEHO] MODBUS FAIL");
            }
            // PV/SV 마지막 실측값 유지 (리셋 안 함)
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
            wchar_t buf[64];
            swprintf(buf, 64, L"[SV] RANGE ERROR: %.2f", value);
            AppendLog(buf);
            return "";
        }

        uint16_t raw = static_cast<uint16_t>(value * 10.0);

        if (gModbus.WriteSingleRegister(1, 20, raw))
        {
            gSV = value;
            wchar_t buf[64];
            swprintf(buf, 64, L"[SV] SET OK: %.2f", value);
            AppendLog(buf);
        }
        else
        {
            wchar_t buf[64];
            swprintf(buf, 64, L"[SV] WRITE FAIL: %.2f", value);
            AppendLog(buf);
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
        wchar_t log[160];
        swprintf(log, 160, L"[UNKNOWN] %s", wbuf);
        AppendLog(log);
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
                                resp.c_str(),
                                (DWORD)resp.size(),
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
            }
        }
        else
        {
            DWORD err = GetLastError();
            if (err != ERROR_INVALID_HANDLE &&
                err != ERROR_OPERATION_ABORTED)
            {
                WriteFailLog("PWT_PORT_ERROR",
                    gPV.load(), gSV.load());
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
        // 폰트
        hFontLabel = MakeFont(16, FW_BOLD, L"Segoe UI");
        hFontNormal = MakeFont(15, FW_NORMAL, L"Segoe UI");
        hFontTemp = MakeFont(32, FW_BOLD, L"Segoe UI");
        hFontStatus = MakeFont(15, FW_SEMIBOLD, L"Segoe UI");

        // 배경 브러시
        hBrushBg = CreateSolidBrush(RGB(245, 245, 245));

        //----------------------------------
        // GroupBox: PWT  (x=10, y=10, w=240, h=165)
        //----------------------------------
        HWND hGrpPwt = CW(L"BUTTON", L"PWT",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 10, 240, 165, hwnd, NULL);
        SF(hGrpPwt, hFontLabel);

        CW(L"STATIC", L"COM Port",
            WS_CHILD | WS_VISIBLE,
            22, 36, 70, 18, hwnd, NULL);

        hEditPwtCom = CW(L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            100, 33, 135, 24, hwnd, (HMENU)1001);
        SF(hEditPwtCom, hFontNormal);

        CW(L"STATIC", L"Status",
            WS_CHILD | WS_VISIBLE,
            22, 70, 70, 18, hwnd, NULL);

        hPwtStatus = CW(L"STATIC", L"DISCONNECTED",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            100, 67, 135, 26, hwnd, (HMENU)4001);
        SF(hPwtStatus, hFontStatus);

        CW(L"STATIC", L"Last RX",
            WS_CHILD | WS_VISIBLE,
            22, 110, 70, 18, hwnd, NULL);

        hLastPwtRx = CW(L"STATIC", L"--",
            WS_CHILD | WS_VISIBLE,
            100, 110, 135, 20, hwnd, NULL);
        SF(hLastPwtRx, hFontNormal);

        //----------------------------------
        // GroupBox: DAEHO  (x=10, y=185, w=240, h=145)
        //----------------------------------
        HWND hGrpDaeho = CW(L"BUTTON", L"DAEHO",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 185, 240, 145, hwnd, NULL);
        SF(hGrpDaeho, hFontLabel);

        CW(L"STATIC", L"COM Port",
            WS_CHILD | WS_VISIBLE,
            22, 211, 70, 18, hwnd, NULL);

        hEditDaehoCom = CW(L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            100, 208, 135, 24, hwnd, (HMENU)1002);
        SF(hEditDaehoCom, hFontNormal);

        CW(L"STATIC", L"Status",
            WS_CHILD | WS_VISIBLE,
            22, 247, 70, 18, hwnd, NULL);

        hDaehoStatus = CW(L"STATIC", L"DISCONNECTED",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            100, 244, 135, 26, hwnd, (HMENU)4002);
        SF(hDaehoStatus, hFontStatus);

        CW(L"STATIC", L"Fail Count",
            WS_CHILD | WS_VISIBLE,
            22, 285, 70, 18, hwnd, NULL);

        hFailCount = CW(L"STATIC", L"0",
            WS_CHILD | WS_VISIBLE,
            100, 285, 135, 20, hwnd, NULL);
        SF(hFailCount, hFontNormal);

        //----------------------------------
        // GroupBox: TEMPERATURE  (x=265, y=10, w=290, h=185)
        //----------------------------------
        HWND hGrpTemp = CW(L"BUTTON", L"TEMPERATURE",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            265, 10, 550, 185, hwnd, NULL);
        SF(hGrpTemp, hFontLabel);

        CW(L"STATIC", L"CURRENT (PV)",
            WS_CHILD | WS_VISIBLE,
            278, 34, 140, 18, hwnd, NULL);

        hCurrentTemp = CW(L"STATIC", L"---- \u00B0C",
            WS_CHILD | WS_VISIBLE,
            278, 56, 525, 48, hwnd, NULL);
        SF(hCurrentTemp, hFontTemp);

        CW(L"STATIC", L"TARGET (SV)",
            WS_CHILD | WS_VISIBLE,
            278, 116, 140, 18, hwnd, NULL);

        hTargetTemp = CW(L"STATIC", L"---- \u00B0C",
            WS_CHILD | WS_VISIBLE,
            278, 138, 525, 48, hwnd, NULL);
        SF(hTargetTemp, hFontTemp);

        //----------------------------------
        // GroupBox: SYSTEM  (x=265, y=205, w=550, h=125)
        //----------------------------------
        HWND hGrpSys = CW(L"BUTTON", L"SYSTEM",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            265, 205, 550, 125, hwnd, NULL);
        SF(hGrpSys, hFontLabel);

        CW(L"STATIC", L"Uptime",
            WS_CHILD | WS_VISIBLE,
            278, 230, 70, 18, hwnd, NULL);

        hUptime = CW(L"STATIC", L"--:--:--",
            WS_CHILD | WS_VISIBLE,
            358, 230, 440, 20, hwnd, NULL);
        SF(hUptime, hFontNormal);

        //----------------------------------
        // CONNECT button
        //----------------------------------
        hConnectBtn = CW(L"BUTTON", L"CONNECT",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            278, 270, 525, 48, hwnd, (HMENU)2001);
        SF(hConnectBtn, hFontLabel);

        //----------------------------------
        // GroupBox: LOG  (좌측 절반: x=10, y=340, w=400, h=195)
        //----------------------------------
        HWND hGrpLog = CW(L"BUTTON", L"LOG",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 340, 400, 200, hwnd, NULL);
        SF(hGrpLog, hFontLabel);

        hLogEdit = CW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            18, 360, 384, 172, hwnd, (HMENU)3001);
        SF(hLogEdit, hFontNormal);

        //----------------------------------
        // GroupBox: GRAPH  (우측 절반: x=418, y=340, w=395, h=195)
        //----------------------------------
        HWND hGrpGraph = CW(L"BUTTON", L"GRAPH",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            418, 340, 395, 200, hwnd, NULL);
        SF(hGrpGraph, hFontLabel);

        // 그래프 클래스 등록 (GraphProc)
        {
            WNDCLASSW gcw = {};
            gcw.lpfnWndProc = GraphProc;
            gcw.hInstance = GetModuleHandleW(NULL);
            gcw.lpszClassName = L"GRAPH_PANEL";
            gcw.hbrBackground = NULL;  // WM_PAINT가 항상 전체를 다시 그리므로 불필요
            gcw.style = 0;             // CS_VREDRAW/CS_HREDRAW 미사용 - 불필요한 재칠 방지
            RegisterClassW(&gcw);
        }

        hGraphPanel = CreateWindowW(
            L"GRAPH_PANEL", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER,
            426, 360, 379, 172,
            hwnd, NULL, GetModuleHandleW(NULL), NULL);

        //----------------------------------
        // 마킹 텍스트 (최하단)
        //----------------------------------
        CW(L"STATIC",
            L"SW개발 및 디자인: 반도체팀 나윤상 연구원",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            10, 548, 803, 18, hwnd, NULL);

        //----------------------------------
        // CRITICAL_SECTION 초기화
        //----------------------------------
        InitializeCriticalSection(&gGraphCS);

        SetTimer(hwnd, 1, 1000, NULL);

        return 0;
    }

    //--------------------------------------------------
    case WM_COMMAND:
        //--------------------------------------------------
    {
        if (LOWORD(wParam) != 2001) break;

        // 중복 방지
        if (gPwtSerial != INVALID_HANDLE_VALUE)
        {
            MessageBoxW(hwnd, L"Already connected.",
                L"INFO", MB_OK);
            break;
        }

        wchar_t pwtNum[16] = {};
        wchar_t daehoNum[16] = {};
        GetWindowTextW(hEditPwtCom, pwtNum, 16);
        GetWindowTextW(hEditDaehoCom, daehoNum, 16);

        wstring pwtPort = wstring(L"\\\\.\\COM") + pwtNum;
        wstring daehoPort = wstring(L"\\\\.\\COM") + daehoNum;

        // PWT OPEN
        gPwtSerial = CreateFileW(
            pwtPort.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);

        if (gPwtSerial == INVALID_HANDLE_VALUE)
        {
            MessageBoxW(hwnd, L"PWT COM OPEN FAIL",
                L"ERROR", MB_ICONERROR);
            break;
        }

        // PWT DCB
        DCB dcb = { 0 };
        dcb.DCBlength = sizeof(DCB);

        if (!GetCommState(gPwtSerial, &dcb))
        {
            MessageBoxW(hwnd, L"GetCommState FAIL",
                L"ERROR", MB_ICONERROR);
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
            MessageBoxW(hwnd, L"SetCommState FAIL",
                L"ERROR", MB_ICONERROR);
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
            break;
        }

        // PWT Timeout
        COMMTIMEOUTS to = { 0 };
        to.ReadIntervalTimeout = 50;
        to.ReadTotalTimeoutConstant = 50;
        to.ReadTotalTimeoutMultiplier = 10;
        SetCommTimeouts(gPwtSerial, &to);

        // DAEHO OPEN
        string daehoPortA(daehoPort.begin(), daehoPort.end());

        if (!gModbus.OpenPort(daehoPortA.c_str()))
        {
            MessageBoxW(hwnd, L"DAEHO COM OPEN FAIL",
                L"ERROR", MB_ICONERROR);
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
            break;
        }

        // Polling thread
        gPollingThread = CreateThread(
            nullptr, 0, DaehoPollingThread,
            nullptr, 0, nullptr);

        if (!gPollingThread)
        {
            MessageBoxW(hwnd, L"POLLING THREAD FAIL",
                L"ERROR", MB_ICONERROR);
            gModbus.ClosePort();
            CloseHandle(gPwtSerial);
            gPwtSerial = INVALID_HANDLE_VALUE;
            break;
        }

        // PWT thread
        gPwtThread = CreateThread(
            nullptr, 0, PwtRxThread,
            nullptr, 0, nullptr);

        if (!gPwtThread)
        {
            MessageBoxW(hwnd, L"PWT THREAD FAIL",
                L"ERROR", MB_ICONERROR);
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

        // 타임스탬프 리셋
        ULONGLONG now = GetTickCount64();
        gConnectTime = now;
        gLastPwtCommandTime = now;
        gLastDaehoSuccessTime = now;
        gModbusFailCount = 0;

        // 버튼/입력창 잠금
        EnableWindow(hConnectBtn, FALSE);
        EnableWindow(hEditPwtCom, FALSE);
        EnableWindow(hEditDaehoCom, FALSE);

        SetWindowTextW(hPwtStatus, L"CONNECTED");
        SetWindowTextW(hDaehoStatus, L"CONNECTED");

        // 연결 로그
        {
            wchar_t logMsg[64];
            swprintf(logMsg, 64,
                L"[CONNECT] PWT=COM%s  DAEHO=COM%s",
                pwtNum, daehoNum);
            AppendLog(logMsg);
        }

        MessageBoxW(hwnd, L"COM OPEN SUCCESS", L"INFO", MB_OK);
        break;
    }

    //--------------------------------------------------
    case WM_TIMER:
        //--------------------------------------------------
    {
        // PV
        {
            wchar_t buf[32];
            if (gPV.load() < -90.0)
                wcscpy_s(buf, L"---- \u00B0C");
            else
                swprintf(buf, 32, L"%.2f \u00B0C", gPV.load());
            SetWindowTextW(hCurrentTemp, buf);
        }

        // SV
        {
            wchar_t buf[32];
            if (gSV.load() < -90.0)
                wcscpy_s(buf, L"---- \u00B0C");
            else
                swprintf(buf, 32, L"%.2f \u00B0C", gSV.load());
            SetWindowTextW(hTargetTemp, buf);
        }

        // Fail Count
        {
            wchar_t buf[16];
            swprintf(buf, 16, L"%d", gModbusFailCount.load());
            SetWindowTextW(hFailCount, buf);
        }

        // DAEHO STATUS
        {
            ULONGLONG now = GetTickCount64();
            ULONGLONG last = gLastDaehoSuccessTime.load();
            bool open = gModbus.IsConnected();

            if (!open)
                SetWindowTextW(hDaehoStatus, L"DISCONNECTED");
            else if (last == 0 || now - last > DAEHO_TIMEOUT_MS)
                SetWindowTextW(hDaehoStatus, L"NO SIGNAL");
            else
                SetWindowTextW(hDaehoStatus, L"CONNECTED");
        }

        // PWT STATUS
        {
            ULONGLONG now = GetTickCount64();
            ULONGLONG last = gLastPwtCommandTime.load();
            bool open = (gPwtSerial != INVALID_HANDLE_VALUE);

            if (!open)
                SetWindowTextW(hPwtStatus, L"DISCONNECTED");
            else if (last == 0 || now - last > PWT_TIMEOUT_MS)
                SetWindowTextW(hPwtStatus, L"NO SIGNAL");
            else
                SetWindowTextW(hPwtStatus, L"CONNECTED");
        }

        // Uptime
        {
            ULONGLONG ct = gConnectTime.load();
            if (ct == 0)
            {
                SetWindowTextW(hUptime, L"--:--:--");
            }
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
            if (last == 0)
            {
                SetWindowTextW(hLastPwtRx, L"--");
            }
            else
            {
                ULONGLONG s = (GetTickCount64() - last) / 1000;
                wchar_t buf[32];
                swprintf(buf, 32, L"%llu sec ago", s);
                SetWindowTextW(hLastPwtRx, buf);
            }
        }

        // 상태창 색 갱신 (InvalidateRect로 WM_CTLCOLORSTATIC 재발생)
        InvalidateRect(hPwtStatus, NULL, TRUE);
        InvalidateRect(hDaehoStatus, NULL, TRUE);

        // 그래프 데이터 push (연결 후에만)
        if (gConnectTime.load() != 0)
            PushGraphData(gPV.load(), gSV.load());

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

        if (hCtrl == hCurrentTemp)
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 80, 180));
            return (LRESULT)hBrushBg;
        }

        if (hCtrl == hTargetTemp)
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(70, 70, 70));
            return (LRESULT)hBrushBg;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    //--------------------------------------------------
    case WM_DESTROY:
        //--------------------------------------------------
    {
        KillTimer(hwnd, 1);

        gProgramRunning = false;

        // PwtSerial 먼저 닫아서 PwtRxThread ReadFile 즉시 해제
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

        // GDI 해제
        if (hFontLabel)  DeleteObject(hFontLabel);
        if (hFontNormal) DeleteObject(hFontNormal);
        if (hFontTemp)   DeleteObject(hFontTemp);
        if (hFontStatus) DeleteObject(hFontStatus);
        if (hBrushBg)    DeleteObject(hBrushBg);

        DeleteCriticalSection(&gGraphCS);

        PostQuitMessage(0);
        return 0;
    }

    } // switch

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//--------------------------------------------------
// 비밀번호 다이얼로그
//--------------------------------------------------

// 비밀번호 (평문)
static const wchar_t* CORRECT_PASSWORD = L"ssw";

// 다이얼로그 내 컨트롤 ID
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
        // 타이틀 레이블
        CreateWindowW(L"STATIC",
            L"PWT-Daeho Gateway",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 18, 280, 24,
            hwnd, NULL, NULL, NULL);

        CreateWindowW(L"STATIC",
            L"비밀번호를 입력하세요.",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 48, 280, 20,
            hwnd, NULL, NULL, NULL);

        // 비밀번호 입력창 (ES_PASSWORD → **** 표시)
        HWND hEdit = CreateWindowW(
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            ES_PASSWORD | ES_AUTOHSCROLL,
            60, 78, 160, 24,
            hwnd, (HMENU)ID_PWD_EDIT, NULL, NULL);

        // 기본 포커스를 입력창으로
        SetFocus(hEdit);

        // 확인 버튼
        CreateWindowW(L"BUTTON", L"확인",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            55, 118, 75, 28,
            hwnd, (HMENU)ID_PWD_OK, NULL, NULL);

        // 취소 버튼
        CreateWindowW(L"BUTTON", L"취소",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            150, 118, 75, 28,
            hwnd, (HMENU)ID_PWD_CANCEL, NULL, NULL);

        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        if (id == ID_PWD_OK ||
            (LOWORD(wParam) == 1 && HIWORD(wParam) == 0)) // Enter 키
        {
            wchar_t input[64] = {};
            GetWindowTextW(
                GetDlgItem(hwnd, ID_PWD_EDIT),
                input, 64);

            if (wcscmp(input, CORRECT_PASSWORD) == 0)
            {
                gPasswordOk = true;
                DestroyWindow(hwnd);
            }
            else
            {
                MessageBoxW(hwnd,
                    L"비밀번호가 올바르지 않습니다.",
                    L"오류",
                    MB_ICONERROR | MB_OK);

                // 입력창 초기화 후 포커스 재설정
                SetWindowTextW(
                    GetDlgItem(hwnd, ID_PWD_EDIT),
                    L"");
                SetFocus(
                    GetDlgItem(hwnd, ID_PWD_EDIT));
            }
        }
        else if (id == ID_PWD_CANCEL)
        {
            gPasswordOk = false;
            DestroyWindow(hwnd);
        }

        return 0;
    }

    // Enter 키를 확인 버튼으로 처리
    case WM_KEYDOWN:
    {
        if (wParam == VK_RETURN)
        {
            SendMessageW(hwnd, WM_COMMAND,
                ID_PWD_OK, 0);
        }
        else if (wParam == VK_ESCAPE)
        {
            SendMessageW(hwnd, WM_COMMAND,
                ID_PWD_CANCEL, 0);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//--------------------------------------------------
// WinMain
//--------------------------------------------------

int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    LPSTR,
    int nCmdShow)
{
    //------------------------------------------
    // 비밀번호 다이얼로그 먼저 실행
    //------------------------------------------

    const wchar_t PWD_CLASS[] = L"PWD_DIALOG";

    WNDCLASSW pwdWc = {};
    pwdWc.lpfnWndProc = PasswordProc;
    pwdWc.hInstance = hInstance;
    pwdWc.lpszClassName = PWD_CLASS;
    pwdWc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&pwdWc);

    HWND hPwd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        PWD_CLASS,
        L"PWT-Daeho Gateway — 인증",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        296, 192,
        NULL, NULL, hInstance, NULL);

    if (!hPwd) return 0;

    ShowWindow(hPwd, SW_SHOW);
    UpdateWindow(hPwd);

    // MSG를 WinMain 스코프 상단에서 한 번만 선언
    MSG msg = {};

    // 비밀번호 다이얼로그 전용 메시지 루프
    while (GetMessage(&msg, NULL, 0, 0))
    {
        // Edit 컨트롤에서 Enter/Escape 처리
        if (msg.message == WM_KEYDOWN)
        {
            if (msg.wParam == VK_RETURN)
            {
                SendMessageW(hPwd, WM_COMMAND,
                    ID_PWD_OK, 0);
                continue;
            }
            else if (msg.wParam == VK_ESCAPE)
            {
                SendMessageW(hPwd, WM_COMMAND,
                    ID_PWD_CANCEL, 0);
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 비밀번호 틀리거나 취소 시 종료
    if (!gPasswordOk)
        return 0;

    //------------------------------------------
    // 메인 창 실행
    //------------------------------------------

    const wchar_t CLASS_NAME[] = L"PWT_DAEHO_GATEWAY";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"PWT-Daeho Gateway",
        WS_OVERLAPPED | WS_CAPTION |
        WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        840, 615,
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