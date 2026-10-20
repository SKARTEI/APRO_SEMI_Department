//==================================================
// Win_graph.cpp
//   PV/SV 실시간 그래프 패널 전담 모듈.
//   버퍼·임계영역·상수를 전부 내부에 숨기고,
//   외부에는 아래 5개 함수만 노출한다.
//     GraphInit / GraphShutdown / GraphRegisterClass
//     GraphCreate / PushGraphData
//   (별도 헤더 없이 GUImain.cpp가 forward 선언으로 사용)
//==================================================

#include <windows.h>
#include <cstring>   // memmove
#include <cstdio>    // swprintf

//--------------------------------------------------
// 내부 상태 (파일 지역)
//--------------------------------------------------

// 최대 10800포인트 = 180분(3시간) × 60초 (1초 1포인트)
static const int GRAPH_MAX_POINTS = 10800;

static double gGraphPV[GRAPH_MAX_POINTS];
static double gGraphSV[GRAPH_MAX_POINTS];
static int    gGraphCount = 0;

static CRITICAL_SECTION gGraphCS;
static bool gGraphReady = false;
static HWND gGraphPanel = NULL;

//--------------------------------------------------
// GraphInit: CS 초기화 + 버퍼 리셋
//--------------------------------------------------

void GraphInit()
{
    InitializeCriticalSection(&gGraphCS);
    gGraphCount = 0;

    for (int i = 0; i < GRAPH_MAX_POINTS; i++)
    {
        gGraphPV[i] = -99.0;
        gGraphSV[i] = -99.0;
    }

    gGraphReady = true;
}

//--------------------------------------------------
// GraphShutdown: CS 해제
//--------------------------------------------------

void GraphShutdown()
{
    if (gGraphReady)
    {
        DeleteCriticalSection(&gGraphCS);
        gGraphReady = false;
    }
}

//--------------------------------------------------
// PushGraphData: 1포인트 추가 (WM_TIMER에서 매초 호출)
//--------------------------------------------------

void PushGraphData(double pv, double sv)
{
    if (!gGraphReady)
        return;

    EnterCriticalSection(&gGraphCS);

    if (gGraphCount < GRAPH_MAX_POINTS)
    {
        gGraphPV[gGraphCount] = pv;
        gGraphSV[gGraphCount] = sv;
        gGraphCount++;
    }
    else
    {
        // 가장 오래된 1포인트 제거 후 좌로 밀기
        memmove(gGraphPV, gGraphPV + 1,
            (GRAPH_MAX_POINTS - 1) * sizeof(double));
        memmove(gGraphSV, gGraphSV + 1,
            (GRAPH_MAX_POINTS - 1) * sizeof(double));

        gGraphPV[GRAPH_MAX_POINTS - 1] = pv;
        gGraphSV[GRAPH_MAX_POINTS - 1] = sv;
    }

    LeaveCriticalSection(&gGraphCS);

    if (gGraphPanel)
        InvalidateRect(gGraphPanel, NULL, FALSE);
}

//--------------------------------------------------
// GraphProc: 패널 윈도우 프로시저 (내부 전용)
//   더블 버퍼링으로 깜빡임 제거.
//--------------------------------------------------

static LRESULT CALLBACK GraphProc(
    HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hdcWindow = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        // -- 더블 버퍼링: 메모리 DC에 전부 그린 뒤 한 번에 복사 --
        HDC hdc = CreateCompatibleDC(hdcWindow);
        HBITMAP hMemBmp = CreateCompatibleBitmap(
            hdcWindow, rc.right, rc.bottom);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hMemBmp);

        HBRUSH hWhite = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, hWhite);
        DeleteObject(hWhite);

        // 축 라벨 폰트: WM_PAINT당 1회 생성 (14px로 키워 가독성 개선)
        HFONT hAxisFont = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        // 플롯 영역 여백 (세로축은 숫자만이라 좌 여백 표준)
        const int ML = 44, MR = 14, MT = 16, MB = 36;
        int pw = rc.right - ML - MR;
        int ph = rc.bottom - MT - MB;

        const double T_MIN = 10.0;
        const double T_MAX = 85.0;
        const double T_RANGE = T_MAX - T_MIN;

        auto TempToY = [&](double t) -> int {
            double ratio = (t - T_MIN) / T_RANGE;
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;
            return MT + ph - (int)(ratio * ph);
            };

        auto IdxToX = [&](int idx, int offset) -> int {
            int scaledIdx = idx + offset;
            return ML + (int)((double)scaledIdx / (GRAPH_MAX_POINTS - 1) * pw);
            };

        // -- 점선 그리드 --
        HPEN hDotPen = CreatePen(PS_DOT, 1, RGB(180, 180, 180));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hDotPen);
        SetBkMode(hdc, TRANSPARENT);

        HFONT hOldAxisF = (HFONT)SelectObject(hdc, hAxisFont);

        // 가로 그리드: 15도 간격 (10~85도)
        double gridTemps[] = { 10.0, 25.0, 40.0, 55.0, 70.0, 85.0 };
        for (double gt : gridTemps)
        {
            int y = TempToY(gt);
            MoveToEx(hdc, ML, y, NULL);
            LineTo(hdc, ML + pw, y);

            wchar_t lbl[8];
            swprintf(lbl, 8, L"%d", (int)gt);
            SetTextColor(hdc, RGB(80, 80, 80));
            RECT lr = { 0, y - 9, ML - 6, y + 9 };
            DrawTextW(hdc, lbl, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        // 세로 그리드: 30분(1800포인트) 간격, 우측 끝 기준 역산
        EnterCriticalSection(&gGraphCS);
        int cnt = gGraphCount;
        LeaveCriticalSection(&gGraphCS);

        {
            const int MIN30 = 1800;
            for (int back = MIN30; back < GRAPH_MAX_POINTS; back += MIN30)
            {
                int scaledIdx = (GRAPH_MAX_POINTS - 1) - back;
                if (scaledIdx < 0) break;
                int x = ML + (int)((double)scaledIdx / (GRAPH_MAX_POINTS - 1) * pw);
                MoveToEx(hdc, x, MT, NULL);
                LineTo(hdc, x, MT + ph);

                int minAgo = back / 60;
                wchar_t tlbl[12];
                swprintf(tlbl, 12, L"-%d", minAgo);
                RECT tr = { x - 24, MT + ph + 4, x + 24, MT + ph + 22 };
                SetTextColor(hdc, RGB(80, 80, 80));
                DrawTextW(hdc, tlbl, -1, &tr, DT_CENTER | DT_SINGLELINE);
            }
        }

        SelectObject(hdc, hOldAxisF);
        DeleteObject(hAxisFont);
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

        // -- 데이터 플롯 --
        EnterCriticalSection(&gGraphCS);

        if (cnt > 1)
        {
            int offset = GRAPH_MAX_POINTS - cnt;

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

        // 범례: 우측 상단 (14px Bold + 굵은 선으로 강조)
        {
            HFONT hLgFont = CreateFontW(14, 0, 0, 0, FW_BOLD,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT hOldF = (HFONT)SelectObject(hdc, hLgFont);

            HPEN hPvL = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
            SelectObject(hdc, hPvL);
            MoveToEx(hdc, ML + pw - 72, MT + 10, NULL);
            LineTo(hdc, ML + pw - 52, MT + 10);
            SelectObject(hdc, GetStockObject(NULL_PEN));
            DeleteObject(hPvL);
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT rPv = { ML + pw - 48, MT + 3, ML + pw, MT + 19 };
            DrawTextW(hdc, L"PV", -1, &rPv, DT_LEFT | DT_SINGLELINE);

            HPEN hSvL = CreatePen(PS_SOLID, 3, RGB(210, 30, 30));
            SelectObject(hdc, hSvL);
            MoveToEx(hdc, ML + pw - 72, MT + 28, NULL);
            LineTo(hdc, ML + pw - 52, MT + 28);
            SelectObject(hdc, GetStockObject(NULL_PEN));
            DeleteObject(hSvL);
            SetTextColor(hdc, RGB(210, 30, 30));
            RECT rSv = { ML + pw - 48, MT + 21, ML + pw, MT + 37 };
            DrawTextW(hdc, L"SV", -1, &rSv, DT_LEFT | DT_SINGLELINE);

            // 축 단위 안내 (좌상단)
            SetTextColor(hdc, RGB(110, 110, 110));
            RECT rUnit = { ML + 6, MT + 4, ML + 240, MT + 20 };
            DrawTextW(hdc, L"X: minutes ago   Y: \u00B0C", -1, &rUnit,
                DT_LEFT | DT_SINGLELINE);

            SelectObject(hdc, hOldF);
            DeleteObject(hLgFont);
        }

        BitBlt(hdcWindow, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);

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
// GraphRegisterClass: 패널 윈도우 클래스 등록
//--------------------------------------------------

void GraphRegisterClass(HINSTANCE hInst)
{
    WNDCLASSW gcw = {};
    gcw.lpfnWndProc = GraphProc;
    gcw.hInstance = hInst;
    gcw.lpszClassName = L"GRAPH_PANEL";
    gcw.hbrBackground = NULL;   // WM_PAINT가 전체를 다시 그림
    gcw.style = 0;
    RegisterClassW(&gcw);
}

//--------------------------------------------------
// GraphCreate: 패널 자식 윈도우 생성 후 내부 보관
//--------------------------------------------------

HWND GraphCreate(HWND parent, int x, int y, int w, int h)
{
    gGraphPanel = CreateWindowW(
        L"GRAPH_PANEL", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER,
        x, y, w, h,
        parent, NULL, GetModuleHandleW(NULL), NULL);

    return gGraphPanel;
}