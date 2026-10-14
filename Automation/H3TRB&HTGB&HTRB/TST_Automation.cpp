#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

wstring Now()
{
    time_t t = time(nullptr);
    tm l;
    localtime_s(&l, &t);
    wchar_t b[32];
    wcsftime(b, 32, L"%Y-%m-%d %H:%M:%S", &l);
    return b;
}

string ToUtf8(const wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void Log(const wstring& m)
{
    wstring l = L"[" + Now() + L"] " + m;
    wcout << l << endl;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, L"readme.txt", L"ab") != 0 || !fp) return;
    fseek(fp, 0, SEEK_END);
    if (ftell(fp) == 0) fputs("\xEF\xBB\xBF", fp);
    string u = ToUtf8(l) + "\r\n";
    fwrite(u.data(), 1, u.size(), fp);
    fclose(fp);
}

bool ReadAll(const wstring& p, string& s)
{
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, p.c_str(), L"rb") != 0 || !fp) return false;
    char b[65536];
    size_t r;
    while ((r = fread(b, 1, sizeof b, fp)) > 0) s.append(b, r);
    fclose(fp);
    return true;
}

bool WriteAll(const wstring& p, const string& s)
{
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, p.c_str(), L"wb") != 0 || !fp) return false;
    bool ok = fwrite(s.data(), 1, s.size(), fp) == s.size();
    return fclose(fp) == 0 && ok;
}

bool Match(const string& s, const char* p)
{
    if (s.size() != strlen(p)) return false;
    for (size_t i = 0; i < s.size(); i++)
        if (p[i] == 'd' ? !isdigit((unsigned char)s[i]) : s[i] != p[i]) return false;
    return true;
}

bool Convert(const wstring& in, const wstring& out)
{
    string raw;
    if (!ReadAll(in, raw))
    {
        Log(L"[오류] 입력 파일 열기 실패: " + in);
        return false;
    }
    Log(L"[통과] 입력 파일 읽기: " + in + L" (" + to_wstring(raw.size()) + L" bytes)");

    istringstream s(raw);
    string line, body, fd, ft, ld, lt;
    int n = 0, bad = 0, gap = 0;
    __time64_t prev = 0;
    getline(s, line);
    while (getline(s, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty())
        {
            if (n > 0) break;
            continue;
        }
        vector<string> c;
        istringstream r(line);
        string x;
        while (getline(r, x, ',')) c.push_back(x);
        if (c.size() < 3 || !Match(c[0], "dddd/dd/dd") || !Match(c[1], "dd:dd:dd"))
        {
            bad++;
            continue;
        }

        string d = c[0];
        replace(d.begin(), d.end(), '/', '-');
        string t = c[1];
        if (t[0] == '0') t.erase(0, 1);

        string v = c[2];
        if (v.empty() || v.find_first_not_of("-.0123456789") != string::npos) bad++;
        else
        {
            if (v.find('.') != string::npos)
            {
                v.erase(v.find_last_not_of('0') + 1);
                if (v.back() == '.') v.pop_back();
            }
            if (v == "-0") v = "0";
        }

        tm k = {};
        k.tm_year = stoi(c[0].substr(0, 4)) - 1900;
        k.tm_mon = stoi(c[0].substr(5, 2)) - 1;
        k.tm_mday = stoi(c[0].substr(8, 2));
        k.tm_hour = stoi(c[1].substr(0, 2));
        k.tm_min = stoi(c[1].substr(3, 2));
        k.tm_sec = stoi(c[1].substr(6, 2));
        __time64_t cur = _mkgmtime64(&k);
        if (n > 0 && cur - prev != 60) gap++;
        prev = cur;

        if (n == 0)
        {
            fd = d;
            ft = t;
        }
        ld = d;
        lt = t;
        body += to_string(n + 1) + "," + d + " " + t.substr(0, t.size() - 3) + ",0," + v + ",LLLLLLLLLL,LLLL,,\r\n";
        n++;
    }

    if (n == 0)
    {
        Log(L"[오류] 데이터 행을 찾지 못함: " + in);
        return false;
    }
    Log(wstring(bad ? L"[오류] " : L"[통과] ") + L"데이터 해석: " + to_wstring(n) + L"행, 형식 오류 " + to_wstring(bad) + L"건");
    Log(wstring(gap ? L"[경고] " : L"[통과] ") + L"시간 간격 검증(1min): 불일치 " + to_wstring(gap) + L"건 (" + wstring(fd.begin(), fd.end()) + L" " + wstring(ft.begin(), ft.end()) + L" ~ " + wstring(ld.begin(), ld.end()) + L" " + wstring(lt.begin(), lt.end()) + L")");

    string head =
        "Vendor,GRAPHTEC Corporation,,,,,,\r\n"
        "Model,GL220,,,,,,\r\n"
        "Version,Ver1.08,,,,,,\r\n"
        "Sampling,1min,,,,,,\r\n"
        "Total data points," + to_string(n) + ",,,,,,\r\n"
        "Start time," + fd + "," + ft + ",,,,,\r\n"
        "End time," + ld + "," + lt + ",,,,,\r\n"
        "Trigger time," + fd + "," + ft + ",,,,,\r\n"
        "AMP Settings,,,,,,,\r\n"
        "CH,Signal name,Input,Range,Filter,Span,,\r\n"
        R"(CH1," "" CH 1""",TEMP,TC_T,Off,200,0,degC)" "\r\n"
        "Logic/Pulse,Off,,,,,,\r\n"
        "Data,,,,,,,\r\n"
        "Number,Date&Time,ms,CH1,Alarm1-10,AlarmOut,,\r\n"
        "NO.,Time,ms,degC,A1234567890,A1234,,\r\n";

    if (!WriteAll(out, head + body))
    {
        Log(L"[오류] 파일 작성 실패: " + out);
        return false;
    }
    Log(L"[통과] 파일 작성 완료: " + out + L" (" + to_wstring(n) + L"행)");
    return true;
}

int main()
{


    _setmode(_fileno(stdout), _O_U16TEXT);
    Log(L"===== 실행 시작 =====");

    vector<wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"RAW*.csv", &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do names.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (names.empty()) Log(L"[오류] RAW*.csv 파일이 현재 폴더에 없음");

    size_t ok = 0;
    for (const wstring& nm : names)
        if (Convert(nm, L"RESULT" + nm.substr(3))) ok++;

    Log(L"===== 실행 종료: 성공 " + to_wstring(ok) + L" / 전체 " + to_wstring(names.size()) + L" =====");
    // cout << _MSVC_LANG << endl;
    
    return 0;
}