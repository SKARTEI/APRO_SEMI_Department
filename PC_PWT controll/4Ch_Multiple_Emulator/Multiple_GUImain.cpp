#include "ModbusRTU.h"

using namespace std;

//--------------------------------------------------
// 4CH PWT-Daeho Gateway
// - CH1: PWT COM + DAEHO COM, DAEHO ID=1
// - CH2: PWT COM + DAEHO COM, DAEHO ID=2
// - CH3: PWT COM + DAEHO COM, DAEHO ID=3
// - CH4: PWT COM + DAEHO COM, DAEHO ID=4
//--------------------------------------------------

constexpr int CHANNEL_COUNT = 4;
constexpr int GRAPH_MAX_POINTS = 6000;     // 100min x 60sec
constexpr ULONGLONG PWT_TIMEOUT_MS = 5000;
constexpr ULONGLONG DAEHO_TIMEOUT_MS = 5000;
constexpr ULONGLONG PV_LOG_INTERVAL_MS = 60ULL * 1000ULL; // 1분 단위 PV 기록

//--------------------------------------------------
// Control IDs
//--------------------------------------------------

constexpr int ID_CONNECT_BASE = 2000;
constexpr int ID_PWT_COM_BASE = 3000;
constexpr int ID_DAEHO_COM_BASE = 3100;

//--------------------------------------------------
// Forward declarations
//--------------------------------------------------

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK GraphProc(HWND, UINT, WPARAM, LPARAM);
DWORD WINAPI DaehoPollingThread(LPVOID);
DWORD WINAPI PwtRxThread(LPVOID);

//--------------------------------------------------
// Channel state
//--------------------------------------------------

struct Channel
{
    int index;       // 0~3
    int daehoId;     // 1~4

    ModbusRTU modbus;
    HANDLE pwtSerial;
    HANDLE pollingThread;
    HANDLE pwtThread;

    atomic<bool> threadRunning;
    atomic<double> pv;
    atomic<double> sv;
    atomic<bool> daehoSignalOk;
    atomic<int> failCount;
    atomic<ULONGLONG> lastPwtCommandTime;
    atomic<ULONGLONG> lastDaehoSuccessTime;
    atomic<ULONGLONG> connectTime;
    atomic<ULONGLONG> lastPvLogWriteTime;

    char currentLogDate[16];       // YYYYMMDD
    char dailyLogFileName[MAX_PATH];

    HWND hGroup;
    HWND hEditPwtCom;
    HWND hEditDaehoCom;
    HWND hPwtStatus;
    HWND hDaehoStatus;
    HWND hCurrentTemp;
    HWND hTargetTemp;
    HWND hFailCount;
    HWND hUptime;
    HWND hLastPwtRx;
    HWND hConnectBtn;
    HWND hLogEdit;
    HWND hGraphPanel;

    double graphPV[GRAPH_MAX_POINTS];
    double graphSV[GRAPH_MAX_POINTS];
    int graphCount;
    CRITICAL_SECTION graphCS;

    Channel()
        : index(0),
        daehoId(0),
        pwtSerial(INVALID_HANDLE_VALUE),
        pollingThread(NULL),
        pwtThread(NULL),
        threadRunning(false),
        pv(-99.0),
        sv(-99.0),
        daehoSignalOk(false),
        failCount(0),
        lastPwtCommandTime(0),
        lastDaehoSuccessTime(0),
        connectTime(0),
        hGroup(NULL),
        hEditPwtCom(NULL),
        hEditDaehoCom(NULL),
        hPwtStatus(NULL),
        hDaehoStatus(NULL),
        hCurrentTemp(NULL),
        hTargetTemp(NULL),
        hFailCount(NULL),
        hUptime(NULL),
        hLastPwtRx(NULL),
        hConnectBtn(NULL),
        hLogEdit(NULL),
        hGraphPanel(NULL),
        graphCount(0)
    {
        ZeroMemory(&graphCS, sizeof(graphCS));
        ZeroMemory(currentLogDate, sizeof(currentLogDate));
        ZeroMemory(dailyLogFileName, sizeof(dailyLogFileName));

        for (int i = 0; i < GRAPH_MAX_POINTS; i++)
        {
            graphPV[i] = -99.0;
            graphSV[i] = -99.0;
        }
    }
};

Channel gCh[CHANNEL_COUNT];
atomic<bool> gProgramRunning(true);

//--------------------------------------------------
// GDI resources
//--------------------------------------------------

HFONT  hFontTitle = NULL;
HFONT  hFontLabel = NULL;
HFONT  hFontNormal = NULL;
HFONT  hFontSmall = NULL;
HFONT  hFontTemp = NULL;
HFONT  hFontStatus = NULL;
HFONT  hFontFooter = NULL;
HBRUSH hBrushBg = NULL;

//--------------------------------------------------
// Helpers
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

static void SF(HWND h, HFONT f)
{
    if (h && f)
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

static HWND CW(
    const wchar_t* cls,
    const wchar_t* txt,
    DWORD style,
    int x, int y, int w, int h,
    HWND parent,
    HMENU id)
{
    return CreateWindowW(cls, txt, style,
        x, y, w, h, parent, id, NULL, NULL);
}

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

static void EnsureLogDirectories()
{
    CreateDirectoryA("Logs", NULL);
    CreateDirectoryA("Logs\\CH1", NULL);
    CreateDirectoryA("Logs\\CH2", NULL);
    CreateDirectoryA("Logs\\CH3", NULL);
    CreateDirectoryA("Logs\\CH4", NULL);
}

static void MakeComPath(const wchar_t* numText, wchar_t* out, int outCount)
{
    // 사용자는 장치관리자에서 본 COM 번호만 입력하면 됨. 예: 7 -> \\.\COM7
    swprintf(out, outCount, L"\\\\.\\COM%s", numText);
}

static void WideToAnsi(const wchar_t* src, char* dst, int dstCount)
{
    WideCharToMultiByte(CP_ACP, 0, src, -1, dst, dstCount, NULL, NULL);
    dst[dstCount - 1] = 0;
}

static void AppendLog(Channel* ch, const wchar_t* msg)
{
    if (!ch || !ch->hLogEdit) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[1024];
    swprintf(line, 1024,
        L"[%02d:%02d:%02d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, msg);

    int len = GetWindowTextLengthW(ch->hLogEdit);

    if (len > 16000)
    {
        SetWindowTextW(ch->hLogEdit, L"");
        len = 0;
    }

    SendMessageW(ch->hLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(ch->hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)line);
}

static void AppendLogf(Channel* ch, const wchar_t* fmt, ...)
{
    wchar_t buf[768];

    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 768, fmt, ap);
    va_end(ap);

    AppendLog(ch, buf);
}

static bool EnsureDailyLogFile(Channel* ch)
{
    if (!ch) return false;

    EnsureLogDirectories();

    char today[16];
    GetLogDateStr(today, sizeof(today));

    if (strcmp(ch->currentLogDate, today) != 0)
    {
        strcpy_s(ch->currentLogDate, sizeof(ch->currentLogDate), today);

        sprintf_s(ch->dailyLogFileName, sizeof(ch->dailyLogFileName),
            "Logs\\CH%d\\log_CH%d_%s.csv",
            ch->index + 1,
            ch->index + 1,
            today);

        FILE* f = NULL;
        fopen_s(&f, ch->dailyLogFileName, "a");
        if (!f) 
            return false;

        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0)
            fprintf(f, "TIMESTAMP,CH,DAEHO_ID,LOG_TYPE,PV,SV,REASON\n");

        fclose(f);
    }

    return true;
}

static void AppendUnifiedLog(Channel* ch, const char* logType, double pv, double sv, const char* reason)
{
    if (!ch) return;
    if (!EnsureDailyLogFile(ch)) return;

    FILE* f = NULL;
    fopen_s(&f, ch->dailyLogFileName, "a");
    if (!f) return;

    char ts[32];
    GetLocalTimeStr(ts, sizeof(ts));

    fprintf(f, "%s,%d,%d,%s,",
        ts, ch->index + 1, ch->daehoId, logType ? logType : "");

    if (pv > -90.0)
        fprintf(f, "%.2f,", pv);
    else
        fprintf(f, "NA,");

    if (sv > -90.0)
        fprintf(f, "%.2f,", sv);
    else
        fprintf(f, "NA,");

    fprintf(f, "%s\n", reason ? reason : "");
    fclose(f);
}

static void WriteFailLog(Channel* ch, const char* reason, double pv, double sv)
{
    AppendUnifiedLog(ch, "FAILURE", pv, sv, reason);
}

static void WritePvLogIfNeeded(Channel* ch)
{
    if (!ch) 
        return;
    if (ch->connectTime.load() == 0) 
        return;

    ULONGLONG now = GetTickCount64();
    ULONGLONG last = ch->lastPvLogWriteTime.load();

    if (last != 0 && now - last < PV_LOG_INTERVAL_MS)
        return;

    // 날짜가 바뀌면 EnsureDailyLogFile()에서 새 파일을 자동 생성함.
    AppendUnifiedLog(ch, "PV", ch->pv.load(), ch->sv.load(), "");

    ch->lastPvLogWriteTime = now;
}

static void PushGraphData(Channel* ch, double pv, double sv)
{
    if (!ch) 
        return;

    EnterCriticalSection(&ch->graphCS);

    if (ch->graphCount < GRAPH_MAX_POINTS)
    {
        ch->graphPV[ch->graphCount] = pv;
        ch->graphSV[ch->graphCount] = sv;
        ch->graphCount++;
    }
    else
    {
        memmove(ch->graphPV, ch->graphPV + 1,
            (GRAPH_MAX_POINTS - 1) * sizeof(double));
        memmove(ch->graphSV, ch->graphSV + 1,
            (GRAPH_MAX_POINTS - 1) * sizeof(double));

        ch->graphPV[GRAPH_MAX_POINTS - 1] = pv;
        ch->graphSV[GRAPH_MAX_POINTS - 1] = sv;
    }

    LeaveCriticalSection(&ch->graphCS);

    if (ch->hGraphPanel)
        InvalidateRect(ch->hGraphPanel, NULL, FALSE);
}

static bool OpenPwtPort(Channel* ch, const wchar_t* pwtPort)
{
    ch->pwtSerial = CreateFileW(
        pwtPort,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (ch->pwtSerial == INVALID_HANDLE_VALUE)
        return false;

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);

    if (!GetCommState(ch->pwtSerial, &dcb))
    {
        CloseHandle(ch->pwtSerial);
        ch->pwtSerial = INVALID_HANDLE_VALUE;
        return false;
    }

    // PWT 통신 조건: 기존 코드 유지
    dcb.BaudRate = 19200;
    dcb.ByteSize = 7;
    dcb.Parity = EVENPARITY;
    dcb.StopBits = ONESTOPBIT;

    if (!SetCommState(ch->pwtSerial, &dcb))
    {
        CloseHandle(ch->pwtSerial);
        ch->pwtSerial = INVALID_HANDLE_VALUE;
        return false;
    }

    COMMTIMEOUTS to = { 0 };
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutConstant = 50;
    to.ReadTotalTimeoutMultiplier = 10;
    to.WriteTotalTimeoutConstant = 300;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(ch->pwtSerial, &to);

    PurgeComm(ch->pwtSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return true;
}

static void CloseChannelHandles(Channel* ch)
{
    if (!ch) 
        return;

    ch->threadRunning = false;

    // PWT ReadFile 해제용으로 먼저 닫음
    if (ch->pwtSerial != INVALID_HANDLE_VALUE)
    {
        CloseHandle(ch->pwtSerial);
        ch->pwtSerial = INVALID_HANDLE_VALUE;
    }

    if (ch->pwtThread)
    {
        WaitForSingleObject(ch->pwtThread, 3000);
        CloseHandle(ch->pwtThread);
        ch->pwtThread = NULL;
    }

    if (ch->pollingThread)
    {
        WaitForSingleObject(ch->pollingThread, 3000);
        CloseHandle(ch->pollingThread);
        ch->pollingThread = NULL;
    }

    ch->modbus.ClosePort();
}

//--------------------------------------------------
// Process PWT command per channel
//--------------------------------------------------

static string ProcessCommand(Channel* ch, const string& cmd)
{
    if (!ch) 
        return "";

    if (cmd == "in_pv_00")
    {
        ostringstream ss;
        ss << fixed << setprecision(2) << ch->pv.load() << "\r\n";
        return ss.str();
    }

    if (cmd == "in_sp_00")
    {
        ostringstream ss;
        ss << fixed << setprecision(2) << ch->sv.load() << "\r\n";
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
            AppendLog(ch, L"[SV] INVALID COMMAND");
            return "";
        }

        if (value < 5.0 || value > 90.0)
        {
            AppendLogf(ch, L"[SV] RANGE ERROR: %.2f", value);
            return "";
        }

        uint16_t raw = static_cast<uint16_t>(value * 10.0 + 0.5);

        if (ch->modbus.WriteSingleRegister(
            static_cast<uint8_t>(ch->daehoId),
            20,
            raw))
        {
            ch->sv = value;
            AppendLogf(ch, L"[SV] SET OK: %.2f", value);
        }
        else
        {
            AppendLogf(ch, L"[SV] WRITE FAIL: %.2f", value);
            WriteFailLog(ch, "SV_WRITE_FAIL", ch->pv.load(), value);
        }

        return "";
    }

    if (cmd.find("out_mode_05") == 0)
    {
        AppendLog(ch, L"[MODE] RUN/STOP IGNORED");
        return "";
    }

    wchar_t wbuf[256] = {};
    MultiByteToWideChar(CP_ACP, 0, cmd.c_str(), -1, wbuf, 256);
    AppendLogf(ch, L"[UNKNOWN] %s", wbuf);

    return "";
}

//--------------------------------------------------
// Thread: Daeho polling
//--------------------------------------------------

DWORD WINAPI DaehoPollingThread(LPVOID param)
{
    Channel* ch = reinterpret_cast<Channel*>(param);
    if (!ch) 
        return 0;

    while (gProgramRunning && ch->threadRunning)
    {
        bool pvOk = false;
        bool svOk = false;
        uint16_t value = 0;

        if (ch->modbus.ReadInputRegister(
            static_cast<uint8_t>(ch->daehoId), 20, value))
        {
            ch->pv = static_cast<double>(value) / 100.0;
            pvOk = true;
        }

        if (ch->modbus.ReadHoldingRegister(
            static_cast<uint8_t>(ch->daehoId), 20, value))
        {
            ch->sv = static_cast<double>(value) / 10.0;
            svOk = true;
        }

        if (pvOk && svOk)
        {
            ch->daehoSignalOk = true;
            ch->failCount = 0;
            ch->lastDaehoSuccessTime = GetTickCount64();
        }
        else
        {
            ch->daehoSignalOk = false;

            int prev = ch->failCount.fetch_add(1);
            if (prev == 0)
            {
                WriteFailLog(ch, "DAEHO_MODBUS_FAIL",
                    ch->pv.load(), ch->sv.load());
                AppendLog(ch, L"[DAEHO] MODBUS FAIL");
            }
        }

        Sleep(1000);
    }

    return 0;
}

//--------------------------------------------------
// Thread: PWT RX
//--------------------------------------------------

DWORD WINAPI PwtRxThread(LPVOID param)
{
    Channel* ch = reinterpret_cast<Channel*>(param);
    if (!ch) 
        return 0;

    string rxBuffer;

    while (gProgramRunning && ch->threadRunning)
    {
        if (ch->pwtSerial == INVALID_HANDLE_VALUE)
            break;

        char c = 0;
        DWORD bytesRead = 0;

        if (ReadFile(ch->pwtSerial, &c, 1, &bytesRead, nullptr))
        {
            if (bytesRead > 0)
            {
                ch->lastPwtCommandTime = GetTickCount64();

                if (c == '\n')
                    continue;

                if (c == '\r')
                {
                    if (!rxBuffer.empty())
                    {
                        string resp = ProcessCommand(ch, rxBuffer);

                        if (!resp.empty() && ch->pwtSerial != INVALID_HANDLE_VALUE)
                        {
                            DWORD written = 0;
                            WriteFile(ch->pwtSerial,
                                resp.c_str(),
                                (DWORD)resp.size(),
                                &written,
                                nullptr);
                        }
                    }

                    rxBuffer.clear();
                }
                else
                {
                    rxBuffer += c;

                    if (rxBuffer.size() > 256)
                    {
                        rxBuffer.clear();
                        if (ch->pwtSerial != INVALID_HANDLE_VALUE)
                            PurgeComm(ch->pwtSerial, PURGE_RXCLEAR);
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
                WriteFailLog(ch, "PWT_PORT_ERROR",
                    ch->pv.load(), ch->sv.load());
            }
            break;
        }

        Sleep(10); // 10의 논리적 근거 없음. 그냥 씀
    }

    return 0;
}

//--------------------------------------------------
// Graph panel window procedure
//--------------------------------------------------

LRESULT CALLBACK GraphProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }

    Channel* ch = reinterpret_cast<Channel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_ERASEBKGND)
        return 1;

    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hdcWindow = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC hdc = CreateCompatibleDC(hdcWindow);
        HBITMAP hMemBmp = CreateCompatibleBitmap(hdcWindow, rc.right, rc.bottom);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hMemBmp);

        HBRUSH hWhite = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, hWhite);
        DeleteObject(hWhite);

        const int ML = 32;
        const int MR = 8;
        const int MT = 12;
        const int MB = 24;

        int pw = rc.right - ML - MR;
        int ph = rc.bottom - MT - MB;
        if (pw < 10) pw = 10;
        if (ph < 10) ph = 10;

        const double T_MIN = 15.0;
        const double T_MAX = 85.0;
        const double T_RANGE = T_MAX - T_MIN;

        auto TempToY = [&](double t) -> int {
            double ratio = (t - T_MIN) / T_RANGE;
            if (ratio < 0.0) 
                ratio = 0.0;
            if (ratio > 1.0) 
                ratio = 1.0;
            
            return MT + ph - (int)(ratio * ph);
            };

        auto IdxToX = [&](int idx, int offset) -> int {
            int scaledIdx = idx + offset;
            
            return ML + (int)((double)scaledIdx / (GRAPH_MAX_POINTS - 1) * pw);
            };

        SetBkMode(hdc, TRANSPARENT);

        // Grid
        HPEN hDotPen = CreatePen(PS_DOT, 1, RGB(185, 185, 185));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hDotPen);

        double gridTemps[] = { 15.0, 30.0, 45.0, 60.0, 75.0 };
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, RGB(120, 120, 120));

        for (double gt : gridTemps)
        {
            int y = TempToY(gt);
            MoveToEx(hdc, ML, y, NULL);
            LineTo(hdc, ML + pw, y);

            wchar_t lbl[12];
            swprintf(lbl, 12, L"%d", (int)gt);
            RECT lr = { 0, y - 8, ML - 3, y + 8 };
            DrawTextW(hdc, lbl, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        int cnt = 0;
        if (ch)
        {
            EnterCriticalSection(&ch->graphCS);
            cnt = ch->graphCount;
            LeaveCriticalSection(&ch->graphCS);
        }

        const int MIN20 = 1200;
        for (int back = MIN20; back < GRAPH_MAX_POINTS; back += MIN20)
        {
            int scaledIdx = (GRAPH_MAX_POINTS - 1) - back;
            if (scaledIdx < 0) 
                break;

            int x = ML + (int)((double)scaledIdx / (GRAPH_MAX_POINTS - 1) * pw);
            MoveToEx(hdc, x, MT, NULL);
            LineTo(hdc, x, MT + ph);

            int minAgo = back / 60;
            wchar_t tlbl[16];
            swprintf(tlbl, 16, L"-%dm", minAgo);
            RECT tr = { x - 20, MT + ph + 2, x + 20, MT + ph + 18 };
            DrawTextW(hdc, tlbl, -1, &tr, DT_CENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, hOldPen);
        DeleteObject(hDotPen);

        // Border
        HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
        hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
        MoveToEx(hdc, ML, MT, NULL);
        LineTo(hdc, ML + pw, MT);
        LineTo(hdc, ML + pw, MT + ph);
        LineTo(hdc, ML, MT + ph);
        LineTo(hdc, ML, MT);
        SelectObject(hdc, hOldPen);
        DeleteObject(hBorderPen);

        // Data
        if (ch)
        {
            EnterCriticalSection(&ch->graphCS);

            cnt = ch->graphCount;
            if (cnt > 1)
            {
                int offset = GRAPH_MAX_POINTS - cnt;

                // PV: black
                HPEN hPvPen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
                hOldPen = (HPEN)SelectObject(hdc, hPvPen);

                bool started = false;
                for (int i = 0; i < cnt; i++)
                {
                    double v = ch->graphPV[i];
                    if (v < -90.0)
                    {
                        started = false;
                        continue;
                    }

                    int x = IdxToX(i, offset);
                    int y = TempToY(v);

                    if (!started)
                    {
                        MoveToEx(hdc, x, y, NULL);
                        started = true;
                    }
                    else
                    {
                        LineTo(hdc, x, y);
                    }
                }

                SelectObject(hdc, hOldPen);
                DeleteObject(hPvPen);

                // SV: red
                HPEN hSvPen = CreatePen(PS_SOLID, 2, RGB(210, 30, 30));
                hOldPen = (HPEN)SelectObject(hdc, hSvPen);

                started = false;
                for (int i = 0; i < cnt; i++)
                {
                    double v = ch->graphSV[i];
                    if (v < -90.0)  
                    {
                        started = false;
                        continue;
                    }

                    int x = IdxToX(i, offset);
                    int y = TempToY(v);

                    if (!started)
                    {
                        MoveToEx(hdc, x, y, NULL);
                        started = true;
                    }
                    else
                    {
                        LineTo(hdc, x, y);
                    }
                }

                SelectObject(hdc, hOldPen);
                DeleteObject(hSvPen);
            }

            LeaveCriticalSection(&ch->graphCS);
        }

        // Legend
        SelectObject(hdc, hFontSmall);

        HPEN hPvL = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
        hOldPen = (HPEN)SelectObject(hdc, hPvL);
        MoveToEx(hdc, ML + pw - 58, MT + 8, NULL);
        LineTo(hdc, ML + pw - 42, MT + 8);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPvL);
        SetTextColor(hdc, RGB(0, 0, 0));
        RECT rPv = { ML + pw - 40, MT + 1, ML + pw, MT + 16 };
        DrawTextW(hdc, L"PV", -1, &rPv, DT_LEFT | DT_SINGLELINE);

        HPEN hSvL = CreatePen(PS_SOLID, 2, RGB(210, 30, 30));
        hOldPen = (HPEN)SelectObject(hdc, hSvL);
        MoveToEx(hdc, ML + pw - 58, MT + 22, NULL);
        LineTo(hdc, ML + pw - 42, MT + 22);
        SelectObject(hdc, hOldPen);
        DeleteObject(hSvL);
        SetTextColor(hdc, RGB(210, 30, 30));
        RECT rSv = { ML + pw - 40, MT + 15, ML + pw, MT + 30 };
        DrawTextW(hdc, L"SV", -1, &rSv, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, hOldFont);

        BitBlt(hdcWindow, 0, 0, rc.right, rc.bottom,
            hdc, 0, 0, SRCCOPY);

        SelectObject(hdc, hOldBmp);
        DeleteObject(hMemBmp);
        DeleteDC(hdc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//--------------------------------------------------
// UI creation per channel
//--------------------------------------------------

static void CreateChannelUI(HWND hwnd, Channel* ch, int x, int y, int w, int h)
{
    wchar_t title[64];

    const wchar_t* channelName[4] =
    {
        L"PWT-1800",
        L"PWT-2400.1",
        L"PWT-2400.2",
        L"PWT-2400.3"
    };

    swprintf(title, 64, L"CH%d   DAEHO ID=%d   %s",
        ch->index + 1,
        ch->daehoId,
        channelName[ch->index]);

    ch->hGroup = CW(L"BUTTON", title,
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, w, h, hwnd, NULL);
    SF(ch->hGroup, hFontTitle);

    int lx = x + 14;
    int vx = x + 96;
    int sx = x + 190;
    int yy = y + 30;

    // PWT COM
    HWND lbl = CW(L"STATIC", L"PWT COM",
        WS_CHILD | WS_VISIBLE,
        lx, yy + 4, 70, 18, hwnd, NULL);
    SF(lbl, hFontNormal);

    ch->hEditPwtCom = CW(L"EDIT", L"0",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        vx, yy, 82, 24, hwnd, (HMENU)(ID_PWT_COM_BASE + ch->index));
    SF(ch->hEditPwtCom, hFontNormal);

    ch->hPwtStatus = CW(L"STATIC", L"DISCONNECTED",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        sx, yy, 128, 24, hwnd, NULL);
    SF(ch->hPwtStatus, hFontStatus);

    yy += 34;

    // DAEHO COM
    lbl = CW(L"STATIC", L"대호 COM",
        WS_CHILD | WS_VISIBLE,
        lx, yy + 4, 70, 18, hwnd, NULL);
    SF(lbl, hFontNormal);

    ch->hEditDaehoCom = CW(L"EDIT", L"0",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        vx, yy, 82, 24, hwnd, (HMENU)(ID_DAEHO_COM_BASE + ch->index));
    SF(ch->hEditDaehoCom, hFontNormal);

    ch->hDaehoStatus = CW(L"STATIC", L"DISCONNECTED",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        sx, yy, 128, 24, hwnd, NULL);
    SF(ch->hDaehoStatus, hFontStatus);

    yy += 36;

    // Connect
    ch->hConnectBtn = CW(L"BUTTON", L"CONNECT",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        lx, yy, w - 28, 34, hwnd, (HMENU)(ID_CONNECT_BASE + ch->index));
    SF(ch->hConnectBtn, hFontLabel);

    yy += 48;

    // Last RX / fail / uptime small block
    lbl = CW(L"STATIC", L"Last PWT RX",
        WS_CHILD | WS_VISIBLE,
        lx, yy, 92, 18, hwnd, NULL);
    SF(lbl, hFontSmall);

    ch->hLastPwtRx = CW(L"STATIC", L"--",
        WS_CHILD | WS_VISIBLE,
        lx + 94, yy, 92, 18, hwnd, NULL);
    SF(ch->hLastPwtRx, hFontSmall);

    lbl = CW(L"STATIC", L"Fail",
        WS_CHILD | WS_VISIBLE,
        lx + 190, yy, 32, 18, hwnd, NULL);
    SF(lbl, hFontSmall);

    ch->hFailCount = CW(L"STATIC", L"0",
        WS_CHILD | WS_VISIBLE,
        lx + 224, yy, 50, 18, hwnd, NULL);
    SF(ch->hFailCount, hFontSmall);

    yy += 22;

    lbl = CW(L"STATIC", L"Uptime",
        WS_CHILD | WS_VISIBLE,
        lx, yy, 60, 18, hwnd, NULL);
    SF(lbl, hFontSmall);

    ch->hUptime = CW(L"STATIC", L"--:--:--",
        WS_CHILD | WS_VISIBLE,
        lx + 94, yy, 160, 18, hwnd, NULL);
    SF(ch->hUptime, hFontSmall);

    yy += 30;

    // PV / SV 온도 블록 (대충 나란히 놨음)
    const int tempGap = 10;
    const int tempCurrentW = ((w - 28) - tempGap) / 2;
    const int tempTargetW = (w - 28) - tempCurrentW - tempGap;
    const int tempLeftX = lx;
    const int tempRightX = lx + tempCurrentW + tempGap;

    lbl = CW(L"STATIC", L"현재온도  PV",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        tempLeftX, yy, tempCurrentW, 20, hwnd, NULL);
    SF(lbl, hFontLabel);

    lbl = CW(L"STATIC", L"목표온도  SV",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        tempRightX, yy, tempTargetW, 20, hwnd, NULL);
    SF(lbl, hFontLabel);

    yy += 24;

    ch->hCurrentTemp = CW(L"STATIC", L"---- °C",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        tempLeftX, yy, tempCurrentW, 46, hwnd, NULL);
    SF(ch->hCurrentTemp, hFontTemp);

    ch->hTargetTemp = CW(L"STATIC", L"---- °C",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        tempRightX, yy, tempTargetW, 46, hwnd, NULL);
    SF(ch->hTargetTemp, hFontTemp);

    yy += 60;

    // Graph
    lbl = CW(L"STATIC", L"GRAPH",
        WS_CHILD | WS_VISIBLE,
        lx, yy, 90, 18, hwnd, NULL);
    SF(lbl, hFontLabel);

    yy += 22;

    ch->hGraphPanel = CreateWindowW(
        L"GRAPH_PANEL", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER,
        lx, yy, w - 28, 225,
        hwnd, NULL, GetModuleHandleW(NULL), ch);

    yy += 238;

    // Log
    lbl = CW(L"STATIC", L"LOG",
        WS_CHILD | WS_VISIBLE,
        lx, yy, 90, 18, hwnd, NULL);
    SF(lbl, hFontLabel);

    yy += 22;

    ch->hLogEdit = CW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        lx, yy, w - 28, y + h - yy - 16,
        hwnd, NULL);
    SF(ch->hLogEdit, hFontSmall);
}

//--------------------------------------------------
// Connect one channel
//--------------------------------------------------

static void ConnectChannel(HWND hwnd, Channel* ch)
{
    if (!ch) 
        return;

    if (ch->pwtSerial != INVALID_HANDLE_VALUE || ch->modbus.IsConnected())
    {
        MessageBoxW(hwnd, L"This channel is already connected.", L"INFO", MB_OK);
        return;
    }

    wchar_t pwtNum[32] = {};
    wchar_t daehoNum[32] = {};

    GetWindowTextW(ch->hEditPwtCom, pwtNum, 32);
    GetWindowTextW(ch->hEditDaehoCom, daehoNum, 32);

    if (wcslen(pwtNum) == 0 || wcslen(daehoNum) == 0 ||
        wcscmp(pwtNum, L"0") == 0 || wcscmp(daehoNum, L"0") == 0)
    {
        MessageBoxW(hwnd,
            L"PWT COM과 대호 COM 번호를 정확히 입력하세요.\n예: 7, 12",
            L"COM INPUT REQUIRED",
            MB_ICONWARNING | MB_OK);
        return;
    }

    wchar_t pwtPortW[64];
    wchar_t daehoPortW[64];
    MakeComPath(pwtNum, pwtPortW, 64);
    MakeComPath(daehoNum, daehoPortW, 64);

    // PWT open
    if (!OpenPwtPort(ch, pwtPortW))
    {
        AppendLogf(ch, L"[CONNECT FAIL] PWT COM%s OPEN FAIL", pwtNum);
        MessageBoxW(hwnd, L"PWT COM OPEN FAIL", L"ERROR", MB_ICONERROR);
        return;
    }

    // DAEHO open
    char daehoPortA[64];
    WideToAnsi(daehoPortW, daehoPortA, 64);

    if (!ch->modbus.OpenPort(daehoPortA))
    {
        AppendLogf(ch, L"[CONNECT FAIL] DAEHO COM%s OPEN FAIL", daehoNum);
        MessageBoxW(hwnd, L"DAEHO COM OPEN FAIL", L"ERROR", MB_ICONERROR);

        CloseHandle(ch->pwtSerial);
        ch->pwtSerial = INVALID_HANDLE_VALUE;
        return;
    }

    // Runtime reset
    ch->threadRunning = true;

    ch->pv = -99.0;
    ch->sv = -99.0;
    ch->daehoSignalOk = false;
    ch->failCount = 0;
    ch->lastPvLogWriteTime = 0;
    
    ZeroMemory(ch->currentLogDate, sizeof(ch->currentLogDate));
    ZeroMemory(ch->dailyLogFileName, sizeof(ch->dailyLogFileName));

    EnterCriticalSection(&ch->graphCS);
    ch->graphCount = 0;

    for (int i = 0; i < GRAPH_MAX_POINTS; i++)
    {
        ch->graphPV[i] = -99.0;
        ch->graphSV[i] = -99.0;
    }
    LeaveCriticalSection(&ch->graphCS);

    ULONGLONG now = GetTickCount64();
    ch->connectTime = now;
    ch->lastPwtCommandTime = now;
    ch->lastDaehoSuccessTime = now;

    // Threads
    ch->pollingThread = CreateThread(
        nullptr, 0, DaehoPollingThread,
        ch, 0, nullptr);

    if (!ch->pollingThread)
    {
        MessageBoxW(hwnd, L"POLLING THREAD FAIL", L"ERROR", MB_ICONERROR);
        CloseChannelHandles(ch);
        return;
    }

    ch->pwtThread = CreateThread(
        nullptr, 0, PwtRxThread,
        ch, 0, nullptr);

    if (!ch->pwtThread)
    {
        MessageBoxW(hwnd, L"PWT THREAD FAIL", L"ERROR", MB_ICONERROR);
        CloseChannelHandles(ch);
        return;
    }

    EnableWindow(ch->hConnectBtn, FALSE);
    EnableWindow(ch->hEditPwtCom, FALSE);
    EnableWindow(ch->hEditDaehoCom, FALSE);

    SetWindowTextW(ch->hPwtStatus, L"CONNECTED");
    SetWindowTextW(ch->hDaehoStatus, L"CONNECTED");

    AppendLogf(ch,
        L"[CONNECT] CH%d  PWT=COM%s  DAEHO=COM%s  DAEHO_ID=%d",
        ch->index + 1, pwtNum, daehoNum, ch->daehoId);

    char today[16];
    GetLogDateStr(today, sizeof(today));

    wchar_t todayW[16];
    MultiByteToWideChar(CP_ACP, 0, today, -1, todayW, 16);

    AppendLogf(ch,
        L"[LOG] PV 1분 간격으로 일 단위 기록 + FAILURE 통합 저장: Logs\\CH%d\\log_CH%d_%s.csv",
        ch->index + 1,
        ch->index + 1,
        todayW);

    AppendLog(ch,
        L"[LOG] PC 날짜가 바뀌면 다음 기록부터 새 날짜 CSV로 자동 전환되어 저장됩니다.");
}

//--------------------------------------------------
// UI update
//--------------------------------------------------

static void UpdateChannelUI(Channel* ch)
{
    if (!ch) 
        return;

    // PV 표현방식
    {
        wchar_t buf[64];
        double pv = ch->pv.load();

        if (pv < -90.0) // 초기값이 -99도라 대충 이렇게 해놓음
            wcscpy_s(buf, L"---- °C");
        else
            swprintf(buf, 64, L"%.2f °C", pv);

        SetWindowTextW(ch->hCurrentTemp, buf);
    }

    // SV 표현방식
    {
        wchar_t buf[64];
        double sv = ch->sv.load();

        if (sv < -90.0) // 초기값이 -99도라 대충 이렇게 해놓음
            wcscpy_s(buf, L"---- °C");
        else
            swprintf(buf, 64, L"%.2f °C", sv);

        SetWindowTextW(ch->hTargetTemp, buf);
    }

    // Fail count
    {
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", ch->failCount.load());
        SetWindowTextW(ch->hFailCount, buf);
    }

    // DAEHO status 연결상태 표시
    {
        ULONGLONG now = GetTickCount64();
        ULONGLONG last = ch->lastDaehoSuccessTime.load();
        bool open = ch->modbus.IsConnected();

        if (!open)
            SetWindowTextW(ch->hDaehoStatus, L"DISCONNECTED");
        else if (last == 0 || now - last > DAEHO_TIMEOUT_MS)
            SetWindowTextW(ch->hDaehoStatus, L"NO SIGNAL");
        else
            SetWindowTextW(ch->hDaehoStatus, L"CONNECTED");
    }

    // PWT status 연결상태 표시
    {
        ULONGLONG now = GetTickCount64();
        ULONGLONG last = ch->lastPwtCommandTime.load();
        bool open = (ch->pwtSerial != INVALID_HANDLE_VALUE);

        if (!open)
            SetWindowTextW(ch->hPwtStatus, L"DISCONNECTED");
        else if (last == 0 || now - last > PWT_TIMEOUT_MS)
            SetWindowTextW(ch->hPwtStatus, L"NO SIGNAL");
        else
            SetWindowTextW(ch->hPwtStatus, L"CONNECTED");
    }

    // Uptime
    {
        ULONGLONG ct = ch->connectTime.load();

        if (ct == 0)
        {
            SetWindowTextW(ch->hUptime, L"--:--:--");
        }
        else
        {
            ULONGLONG s = (GetTickCount64() - ct) / 1000;
            wchar_t buf[32];
            swprintf(buf, 32, L"%02llu:%02llu:%02llu",
                s / 3600, (s % 3600) / 60, s % 60);
            SetWindowTextW(ch->hUptime, buf);
        }
    }

    // Last PWT RX
    {
        ULONGLONG last = ch->lastPwtCommandTime.load(); 

        if (last == 0)
        {
            SetWindowTextW(ch->hLastPwtRx, L"--");
        }
        else
        {
            ULONGLONG s = (GetTickCount64() - last) / 1000;
            wchar_t buf[32];
            swprintf(buf, 32, L"%llu sec ago", s);
            SetWindowTextW(ch->hLastPwtRx, buf);
        }
    }

    InvalidateRect(ch->hPwtStatus, NULL, TRUE);
    InvalidateRect(ch->hDaehoStatus, NULL, TRUE);
    InvalidateRect(ch->hCurrentTemp, NULL, TRUE);
    InvalidateRect(ch->hTargetTemp, NULL, TRUE);

    WritePvLogIfNeeded(ch);

    if (ch->connectTime.load() != 0)
        PushGraphData(ch, ch->pv.load(), ch->sv.load());
}

//--------------------------------------------------
// WndProc
//--------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        EnsureLogDirectories();

        hFontTitle = MakeFont(17, FW_BOLD, L"Segoe UI");
        hFontLabel = MakeFont(15, FW_BOLD, L"Segoe UI");
        hFontNormal = MakeFont(14, FW_NORMAL, L"Segoe UI");
        hFontSmall = MakeFont(12, FW_NORMAL, L"Segoe UI");
        hFontTemp = MakeFont(31, FW_BOLD, L"Segoe UI");
        hFontStatus = MakeFont(13, FW_SEMIBOLD, L"Segoe UI");
        hFontFooter = MakeFont(16, FW_SEMIBOLD, L"Segoe UI");
        hBrushBg = CreateSolidBrush(RGB(245, 245, 245));

        // Graph class registration
        {
            WNDCLASSW gcw = {};
            gcw.lpfnWndProc = GraphProc;
            gcw.hInstance = GetModuleHandleW(NULL);
            gcw.lpszClassName = L"GRAPH_PANEL";
            gcw.hbrBackground = NULL;
            gcw.style = 0;
            RegisterClassW(&gcw);
        }

        // Channel init + UI
        const int startX = 10;
        const int startY = 10;
        const int chW = 350;
        const int chH = 700;
        const int gap = 10;

        for (int i = 0; i < CHANNEL_COUNT; i++)
        {
            Channel* ch = &gCh[i];

            ch->index = i;
            ch->daehoId = i + 1;
            ch->pwtSerial = INVALID_HANDLE_VALUE;
            ch->pollingThread = NULL;
            ch->pwtThread = NULL;
            ch->threadRunning = false;
            ch->pv = -99.0;
            ch->sv = -99.0;
            ch->daehoSignalOk = false;
            ch->failCount = 0;
            ch->lastPwtCommandTime = 0;
            ch->lastDaehoSuccessTime = 0;
            ch->connectTime = 0;
            ch->lastPvLogWriteTime = 0;
            ZeroMemory(ch->currentLogDate, sizeof(ch->currentLogDate));
            ZeroMemory(ch->dailyLogFileName, sizeof(ch->dailyLogFileName));
            ch->graphCount = 0;

            InitializeCriticalSection(&ch->graphCS);

            for (int j = 0; j < GRAPH_MAX_POINTS; j++)
            {
                ch->graphPV[j] = -99.0;
                ch->graphSV[j] = -99.0;
            }

            CreateChannelUI(hwnd, ch,
                startX + i * (chW + gap),
                startY,
                chW,
                chH);
        }

        HWND credit = CW(L"STATIC",
            L"SW 전체 개발 및 UI디자인: 반도체팀 나윤상 연구원  |  Version 1.3  260630 Release",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            10, 724, 1430, 28, hwnd, NULL);
        SF(credit, hFontFooter);

        SetTimer(hwnd, 1, 1000, NULL);
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        if (id >= ID_CONNECT_BASE && id < ID_CONNECT_BASE + CHANNEL_COUNT)
        {
            int idx = id - ID_CONNECT_BASE;
            ConnectChannel(hwnd, &gCh[idx]);
            return 0;
        }

        break;
    }

    case WM_TIMER:
    {
        for (int i = 0; i < CHANNEL_COUNT; i++)
            UpdateChannelUI(&gCh[i]);

        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HWND hCtrl = (HWND)lParam;
        HDC hdc = (HDC)wParam;

        for (int i = 0; i < CHANNEL_COUNT; i++)
        {
            Channel* ch = &gCh[i];

            if (hCtrl == ch->hPwtStatus || hCtrl == ch->hDaehoStatus)
            {
                wchar_t txt[32] = { };
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

            if (hCtrl == ch->hCurrentTemp)
            {
                SetBkMode(hdc, TRANSPARENT);

                double pv = ch->pv.load();
                double sv = ch->sv.load();

                if (pv < -90.0 || sv < -90.0)
                    SetTextColor(hdc, RGB(0, 80, 180));
                else if (pv > sv + 0.2)
                    SetTextColor(hdc, RGB(0, 80, 180));   // 냉각중
                else if (pv < sv - 0.2)
                    SetTextColor(hdc, RGB(210, 30, 30));  // 가열중
                else
                    SetTextColor(hdc, RGB(34, 139, 34));  // 목표온도 ±0.2°C 이내

                return (LRESULT)hBrushBg;
            }

            if (hCtrl == ch->hTargetTemp)
            {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(70, 70, 70));

                return (LRESULT)hBrushBg;
            }
        }

        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)hBrushBg;
    }

    case WM_DESTROY: // 소멸 시퀀스
    {
        KillTimer(hwnd, 1);
        gProgramRunning = false;

        for (int i = 0; i < CHANNEL_COUNT; i++)
            CloseChannelHandles(&gCh[i]);

        for (int i = 0; i < CHANNEL_COUNT; i++)
            DeleteCriticalSection(&gCh[i].graphCS);

        if (hFontTitle)  DeleteObject(hFontTitle);
        if (hFontLabel)  DeleteObject(hFontLabel);
        if (hFontNormal) DeleteObject(hFontNormal);
        if (hFontSmall)  DeleteObject(hFontSmall);
        if (hFontTemp)   DeleteObject(hFontTemp);
        if (hFontStatus) DeleteObject(hFontStatus);
        if (hFontFooter) DeleteObject(hFontFooter);
        if (hBrushBg)    DeleteObject(hBrushBg);

        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//--------------------------------------------------
// Password window
//--------------------------------------------------

static const wchar_t* CORRECT_PASSWORD = L"768902";

#define ID_PWD_EDIT   5001
#define ID_PWD_OK     5002
#define ID_PWD_CANCEL 5003

static bool gPasswordOk = false;
static bool gPasswordDone = false;

LRESULT CALLBACK PasswordProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HWND title = CreateWindowW(L"STATIC",
            L"4CH PWT-Daeho Gateway",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 18, 300, 24,
            hwnd, NULL, NULL, NULL);
        SF(title, hFontNormal);

        HWND label = CreateWindowW(L"STATIC",
            L"비밀번호를 입력하세요.",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 48, 300, 20,
            hwnd, NULL, NULL, NULL);
        SF(label, hFontNormal);

        HWND hEdit = CreateWindowW(
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            ES_PASSWORD | ES_AUTOHSCROLL,
            70, 78, 160, 24,
            hwnd, (HMENU)ID_PWD_EDIT, NULL, NULL);
        SF(hEdit, hFontNormal);

        CreateWindowW(L"BUTTON", L"확인",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            65, 118, 75, 28,
            hwnd, (HMENU)ID_PWD_OK, NULL, NULL);

        CreateWindowW(L"BUTTON", L"취소",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            160, 118, 75, 28,
            hwnd, (HMENU)ID_PWD_CANCEL, NULL, NULL);

        SetFocus(hEdit);
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
                gPasswordDone = true;
                DestroyWindow(hwnd);
            }
            else
            {
                MessageBoxW(hwnd,
                    L"비밀번호가 올바르지 않습니다.",
                    L"오류",
                    MB_ICONERROR | MB_OK);

                SetWindowTextW(GetDlgItem(hwnd, ID_PWD_EDIT), L"");
                SetFocus(GetDlgItem(hwnd, ID_PWD_EDIT));
            }
        }
        else if (id == ID_PWD_CANCEL)
        {
            gPasswordOk = false;
            gPasswordDone = true;
            DestroyWindow(hwnd);
        }

        return 0;
    }

    case WM_CLOSE:
        gPasswordOk = false;
        gPasswordDone = true;
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//--------------------------------------------------
// WinMain
//--------------------------------------------------

static int RunApp(HINSTANCE hInstance, int nCmdShow)
{
    // Password window also uses font
    hFontNormal = MakeFont(15, FW_NORMAL, L"Segoe UI");

    const wchar_t PWD_CLASS[] = L"PWD_DIALOG_4CH";

    WNDCLASSW pwdWc = {};
    pwdWc.lpfnWndProc = PasswordProc;
    pwdWc.hInstance = hInstance;
    pwdWc.lpszClassName = PWD_CLASS;
    pwdWc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&pwdWc);

    HWND hPwd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        PWD_CLASS,
        L"4CH PWT-Daeho Gateway — 인증",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        316, 192,
        NULL, NULL, hInstance, NULL);

    if (!hPwd) return 0;

    ShowWindow(hPwd, SW_SHOW);
    UpdateWindow(hPwd);

    MSG msg = {};

    while (!gPasswordDone && GetMessage(&msg, NULL, 0, 0))
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

    if (hFontNormal)
    {
        DeleteObject(hFontNormal);
        hFontNormal = NULL;
    }

    if (!gPasswordOk)
        return 0;

    const wchar_t CLASS_NAME[] = L"PWT_DAEHO_GATEWAY_4CH";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"4CH PWT-Daeho Gateway",
        WS_OVERLAPPED | WS_CAPTION |
        WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1465, 790,
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    return RunApp(hInstance, nCmdShow);
}
