#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace std;
using Bytes = vector<uint8_t>;

inline string U(const char* s) { return s; }
#ifdef __cpp_char8_t
inline string U(const char8_t* s) { return reinterpret_cast<const char*>(s); }
inline filesystem::path P(const char8_t* s) { return filesystem::path(s); }
#else
inline filesystem::path P(const char* s) { return filesystem::u8path(s); }
#endif

namespace {

    void append_utf8(string& s, uint32_t c) {
        static const int lead[4] = { 0, 0xC0, 0xE0, 0xF0 };
        int n = c < 0x80 ? 0 : c < 0x800 ? 1 : c < 0x10000 ? 2 : 3;
        s += char(lead[n] | (c >> (6 * n)));
        for (int i = n - 1; i >= 0; --i) s += char(0x80 | ((c >> (6 * i)) & 0x3F));
    }

    string decode(const Bytes& b) {
        if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return string(b.begin() + 3, b.end());
        if (b.size() < 2 || !((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) return string(b.begin(), b.end());
        bool le = b[0] == 0xFF;
        auto unit = [&](size_t i) -> uint32_t { return le ? (b[i] | (b[i + 1] << 8)) : ((b[i] << 8) | b[i + 1]); };
        string o;
        for (size_t i = 2; i + 1 < b.size(); i += 2) {
            uint32_t u = unit(i);
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size() && unit(i + 2) >= 0xDC00 && unit(i + 2) < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (unit(i + 2) - 0xDC00); i += 2; }
            append_utf8(o, u);
        }
        return o;
    }

    Bytes encode16(const string& s, bool bom) {
        Bytes o;
        if (bom) { o.push_back(0xFF); o.push_back(0xFE); }
        auto put = [&](uint32_t u) { o.push_back(uint8_t(u & 0xFF)); o.push_back(uint8_t(u >> 8)); };
        for (size_t i = 0; i < s.size();) {
            uint8_t c = uint8_t(s[i]);
            int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
            uint32_t u = n == 0 ? c : (c & (0x3F >> n));
            for (int k = 1; k <= n && i + k < s.size(); ++k) u = (u << 6) | (uint8_t(s[i + k]) & 0x3F);
            i += n + 1;
            if (u >= 0x10000) { u -= 0x10000; put(0xD800 + (u >> 10)); put(0xDC00 + (u & 0x3FF)); }
            else put(u);
        }
        return o;
    }

    bool read_bytes(const filesystem::path& p, Bytes& out) {
        ifstream in(p, ios::binary);
        if (!in) return false;
        out.assign(istreambuf_iterator<char>(in), istreambuf_iterator<char>());
        return true;
    }

    vector<string> split_lines(const string& t) {
        vector<string> v;
        for (size_t p = 0; p < t.size();) {
            size_t e = t.find('\n', p);
            if (e == string::npos) e = t.size();
            v.push_back(t.substr(p, e - p));
            if (!v.back().empty() && v.back().back() == '\r') v.back().pop_back();
            p = e + 1;
        }
        return v;
    }

    bool parse_time(const string& s, long long& out) {
        static const int pos[6][2] = { {0, 4}, {5, 2}, {8, 2}, {11, 2}, {14, 2}, {17, 2} };
        if (s.size() != 19 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':') return false;
        long long v[6];
        for (int i = 0; i < 6; ++i) {
            v[i] = 0;
            for (int k = 0; k < pos[i][1]; ++k) {
                char c = s[pos[i][0] + k];
                if (c < '0' || c > '9') return false;
                v[i] = v[i] * 10 + (c - '0');
            }
        }
        if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 60) return false;
        long long y = v[0] - (v[1] <= 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (v[1] + (v[1] > 2 ? -3 : 9)) + 2) / 5 + v[2] - 1;
        long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
        out = days * 86400 + v[3] * 3600 + v[4] * 60 + v[5];
        return true;
    }

    string fmt_dur(long long s) {
        string o;
        long long d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
        if (d) o += to_string(d) + U(u8"일 ");
        if (d || h) o += to_string(h) + U(u8"시간 ");
        if (d || h || m) o += to_string(m) + U(u8"분 ");
        return o + to_string(s % 60) + U(u8"초");
    }

    string stamp() {
        time_t t = time(nullptr);
        tm v{};
#ifdef _WIN32
        localtime_s(&v, &t);
#else
        localtime_r(&t, &v);
#endif
        char b[32];
        strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &v);
        return b;
    }

}

class Merger {
public:
    explicit Merger(filesystem::path dir) : dir_(move(dir)) {}

    int run() {
        filesystem::path lp = dir_ / "readme_merge.txt";
        bool fresh = !filesystem::exists(lp) || filesystem::file_size(lp) == 0;
        log_.open(lp, ios::binary | ios::app);
        if (!log_) { cerr << "cannot open readme_merge.txt\n"; return 2; }
        if (fresh) log_ << "\xEF\xBB\xBF";
        note("");
        note("===== RUN " + stamp() + " =====");
        try {
            return process();
        }
        catch (const exception& e) {
            cerr << "fatal: " << e.what() << "\n";
            ++errors_;
            note(U(u8"[FATAL] 예외 발생, 실행 중단"));
            return 2;
        }
    }

private:
    struct Row { long long t = 0; size_t src = 0; string rest; };
    struct Src {
        filesystem::path path;
        string name, header, t0, t1, why;
        vector<string> head, sig;
        vector<Row> rows;
        long long first = 0, last = 0;
        size_t read = 0;
        bool use = true;
    };
    struct Dup { size_t same = 0, diff = 0; long long lo = LLONG_MAX, hi = LLONG_MIN; string s0, s1; };

    filesystem::path dir_, csv_;
    ofstream log_;
    size_t errors_ = 0, ref_ = 0, gapCount_ = 0;
    int cpNo_ = 0;
    vector<Src> srcs_;
    vector<size_t> order_;
    vector<Row> kept_;
    map<pair<size_t, size_t>, Dup> dups_;
    vector<string> conflicts_, issues_;

    static string tstr(const Row& r) { return r.rest.substr(0, r.rest.find('\t')); }

    void note(const string& s) { log_ << s << "\r\n"; log_.flush(); }

    void fail(const string& s) {
        ++errors_;
        issues_.push_back(s);
        note("[ERROR] " + s);
    }

    bool cp(const string& what, bool ok, const string& detail) {
        note("[CP" + to_string(++cpNo_) + (ok ? " PASS] " : " FAIL] ") + what + " - " + detail);
        if (!ok) ++errors_;
        return ok;
    }

    void exclude(Src& s, const string& why) {
        s.use = false;
        s.why = why;
        fail(s.name + U(u8" 제외: ") + why);
    }

    bool scan() {
        string a = U(u8"데이터_계측기"), b = U(u8"데이터 계측기");
        for (const auto& e : filesystem::directory_iterator(dir_)) {
            if (!e.is_regular_file()) continue;
            auto u = e.path().filename().u8string();
            auto x = e.path().extension().u8string();
            string n(u.begin(), u.end()), ext(x.begin(), x.end());
            transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(tolower(c)); });
            if (ext != ".csv" || (n.rfind(a, 0) != 0 && n.rfind(b, 0) != 0)) continue;
            Src s;
            s.path = e.path();
            s.name = n;
            srcs_.push_back(move(s));
        }
        return !srcs_.empty();
    }

    static bool parse_row(const string& line, size_t tabs, Row& r) {
        size_t p = line.find('\t');
        if (p == string::npos || p == 0 || line.find_first_not_of("0123456789") != p) return false;
        if (size_t(count(line.begin(), line.end(), '\t')) != tabs) return false;
        size_t q = line.find('\t', p + 1);
        r.rest = line.substr(p + 1);
        return parse_time(line.substr(p + 1, q - p - 1), r.t);
    }

    void load(Src& s, size_t id) {
        Bytes raw;
        if (!read_bytes(s.path, raw) || raw.empty()) return exclude(s, U(u8"파일을 읽을 수 없거나 비어 있음"));
        vector<string> lines = split_lines(decode(raw));
        string hdr = U(u8"스캔\t시간");
        size_t hi = 0;
        while (hi < lines.size() && lines[hi].rfind(hdr, 0) != 0) ++hi;
        if (hi == lines.size()) return exclude(s, U(u8"헤더(스캔/시간) 행 없음"));
        s.head.assign(lines.begin(), lines.begin() + hi);
        s.header = lines[hi];
        for (const auto& l : s.head)
            if (!l.empty() && (isdigit(uint8_t(l[0])) || l.rfind(U(u8"총 채널"), 0) == 0)) s.sig.push_back(l);
        size_t tabs = size_t(count(s.header.begin(), s.header.end(), '\t')), bad = 0, order = 0;
        for (size_t i = hi + 1; i < lines.size(); ++i) {
            if (lines[i].empty()) continue;
            Row r;
            r.src = id;
            if (!parse_row(lines[i], tabs, r)) {
                if (++bad <= 5) fail(s.name + U(u8" 행 해석 실패(열 수/스캔/시간 형식): 줄 ") + to_string(i + 1));
                continue;
            }
            if (!s.rows.empty() && r.t < s.rows.back().t) ++order;
            s.rows.push_back(move(r));
        }
        if (bad > 5) fail(s.name + U(u8" 행 해석 실패 추가 ") + to_string(bad - 5) + U(u8"행 (목록 생략)"));
        if (order) fail(s.name + U(u8" 시간 역행 ") + to_string(order) + U(u8"행 (정렬로 보정)"));
        if (s.rows.empty()) return exclude(s, U(u8"유효한 데이터 행 없음"));
        auto cmp = [](const Row& a, const Row& b) { return a.t < b.t; };
        const Row& lo = *min_element(s.rows.begin(), s.rows.end(), cmp), & hi2 = *max_element(s.rows.begin(), s.rows.end(), cmp);
        s.first = lo.t;
        s.last = hi2.t;
        s.t0 = tstr(lo);
        s.t1 = tstr(hi2);
        s.read = s.rows.size();
    }

    void merge() {
        vector<Row> all;
        for (size_t i : order_) for (auto& r : srcs_[i].rows) all.push_back(move(r));
        stable_sort(all.begin(), all.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
        for (auto& r : all) {
            if (kept_.empty() || kept_.back().t != r.t) { kept_.push_back(move(r)); continue; }
            Dup& d = dups_[{kept_.back().src, r.src}];
            if (r.t < d.lo) { d.lo = r.t; d.s0 = tstr(r); }
            if (r.t > d.hi) { d.hi = r.t; d.s1 = tstr(r); }
            if (kept_.back().rest == r.rest) { ++d.same; continue; }
            ++d.diff;
            if (conflicts_.size() < 20) conflicts_.push_back(tstr(r) + " | " + srcs_[kept_.back().src].name + " vs " + srcs_[r.src].name);
        }
    }

    bool write_csv() {
        const Src& ref = srcs_[ref_];
        ofstream o(csv_, ios::binary | ios::trunc);
        if (!o) return false;
        auto emit = [&](const string& l, bool bom) { Bytes b = encode16(l + "\r\n", bom); o.write(reinterpret_cast<const char*>(b.data()), streamsize(b.size())); };
        for (size_t i = 0; i < ref.head.size(); ++i) emit(ref.head[i], i == 0);
        emit(ref.header, false);
        for (size_t i = 0; i < kept_.size(); ++i) emit(to_string(i + 1) + "\t" + kept_[i].rest, false);
        o.flush();
        return bool(o);
    }

    bool verify_csv() {
        const Src& ref = srcs_[ref_];
        Bytes raw;
        if (!read_bytes(csv_, raw)) return false;
        vector<string> lines = split_lines(decode(raw));
        size_t hi = ref.head.size(), n = 0;
        if (lines.size() < hi + 1 || !equal(ref.head.begin(), ref.head.end(), lines.begin()) || lines[hi] != ref.header) return false;
        for (size_t i = hi + 1; i < lines.size(); ++i) {
            if (lines[i].empty()) continue;
            if (n >= kept_.size() || lines[i] != to_string(n + 1) + "\t" + kept_[n].rest) return false;
            ++n;
        }
        return n == kept_.size();
    }

    bool report() {
        vector<long long> d;
        for (size_t i = 1; i < kept_.size(); ++i) d.push_back(kept_[i].t - kept_[i - 1].t);
        long long iv = 0;
        if (!d.empty()) { nth_element(d.begin(), d.begin() + long(d.size() / 2), d.end()); iv = d[d.size() / 2]; }
        vector<string> gaps, shorts, files, dups, confl;
        for (size_t i = 1; i < kept_.size() && iv > 0; ++i) {
            const Row& a = kept_[i - 1], & b = kept_[i];
            long long dt = b.t - a.t;
            bool gap = dt * 2 > iv * 3;
            if (!gap && dt * 2 >= iv) continue;
            vector<string>& v = gap ? gaps : shorts;
            string s = to_string(v.size() + 1) + ") " + tstr(a) + " -> " + tstr(b) + " | " + fmt_dur(dt);
            if (gap) s += U(u8" | 누락 추정 ") + to_string((dt + iv / 2) / iv - 1) + U(u8"스캔");
            s += U(u8" | 병합 스캔 #") + to_string(i) + "->#" + to_string(i + 1) + " | " + srcs_[a.src].name;
            s += a.src == b.src ? U(u8" (같은 파일 내부)") : " -> " + srcs_[b.src].name;
            v.push_back(s);
        }
        for (size_t k = 0; k < order_.size(); ++k) {
            const Src& s = srcs_[order_[k]];
            files.push_back(to_string(k + 1) + ") " + s.name + " | " + to_string(s.read) + U(u8"행 | ") + s.t0 + " ~ " + s.t1);
        }
        for (const auto& [k, v] : dups_) {
            string s = srcs_[k.second].name + U(u8" 의 ") + to_string(v.same) + U(u8"행이 ") + srcs_[k.first].name + U(u8" 와 동일하여 제거 (구간 ") + v.s0 + " ~ " + v.s1 + ")";
            dups.push_back(s);
        }
        confl = conflicts_;
        string o = U(u8"[특이사항] 생성: ") + stamp() + "\r\n";
        o += U(u8"병합 파일 ") + to_string(order_.size()) + U(u8"개 / 병합 데이터 ") + to_string(kept_.size()) + U(u8"행 / 기준 스캔 간격(중앙값) ") + to_string(iv) + U(u8"초\r\n");
        auto sec = [&](const string& title, const vector<string>& v) {
            o += "\r\n" + title + " - " + to_string(v.size()) + U(u8"건\r\n");
            if (v.empty()) o += U(u8"   없음\r\n");
            for (const auto& s : v) o += "   " + s + "\r\n";
            };
        sec(U(u8"0. 병합 순서"), files);
        sec(U(u8"1. 시간 공백 (기준 간격의 1.5배 초과)"), gaps);
        sec(U(u8"2. 간격 이상 (기준 간격의 0.5배 미만)"), shorts);
        sec(U(u8"3. 중복 구간 (첫 번째 값 유지, 나머지 제거)"), dups);
        sec(U(u8"4. 값이 다른 중복"), confl);
        sec(U(u8"5. 제외된 파일/행 및 오류"), issues_);
        ofstream f(dir_ / P(u8"특이사항.txt"), ios::binary | ios::trunc);
        f << "\xEF\xBB\xBF" << o;
        f.flush();
        note(U(u8"[RESULT] 병합 ") + to_string(kept_.size()) + U(u8"행, 기준 간격 ") + to_string(iv) + U(u8"초, 시간 공백 ") + to_string(gaps.size()) + U(u8"건, 간격 이상 ") + to_string(shorts.size()) + U(u8"건"));
        gapCount_ = gaps.size();
        return bool(f);
    }

    int process() {
        bool found = scan();
        if (!cp(U(u8"입력 파일 탐색"), found, to_string(srcs_.size()) + U(u8"개 발견"))) { cerr << "no input files\n"; return 1; }
        sort(srcs_.begin(), srcs_.end(), [](const Src& a, const Src& b) { return a.name < b.name; });
        for (size_t i = 0; i < srcs_.size(); ++i) load(srcs_[i], i);
        for (size_t i = 0; i < srcs_.size(); ++i) if (srcs_[i].use) order_.push_back(i);
        if (order_.empty()) { cp(U(u8"유효 데이터 파일 확인"), false, U(u8"사용 가능한 파일 없음")); return 1; }
        sort(order_.begin(), order_.end(), [&](size_t a, size_t b) { return tie(srcs_[a].first, srcs_[a].name) < tie(srcs_[b].first, srcs_[b].name); });
        ref_ = order_.front();
        for (size_t i : order_) {
            if (srcs_[i].header != srcs_[ref_].header) exclude(srcs_[i], U(u8"채널 구성(헤더) 불일치"));
            else if (srcs_[i].sig != srcs_[ref_].sig) exclude(srcs_[i], U(u8"채널 설정 불일치"));
        }
        order_.erase(remove_if(order_.begin(), order_.end(), [&](size_t i) { return !srcs_[i].use; }), order_.end());
        cp(U(u8"채널 구성 일치 검사"), order_.size() == srcs_.size(), to_string(order_.size()) + U(u8"개 사용, ") + to_string(srcs_.size() - order_.size()) + U(u8"개 제외"));
        size_t total = 0;
        for (size_t k = 0; k < order_.size(); ++k) {
            const Src& s = srcs_[order_[k]];
            total += s.read;
            note("[FILE] " + s.name + " | " + to_string(s.read) + U(u8"행 | ") + s.t0 + " ~ " + s.t1);
            if (k == 0) continue;
            long long g = s.first - srcs_[order_[k - 1]].last;
            note(U(u8"       이전 파일 종료 -> 시작 간격: ") + (g < 0 ? U(u8"겹침 ") + fmt_dur(-g) : fmt_dur(g)));
        }
        merge();
        size_t drop = 0, diff = 0;
        for (const auto& kv : dups_) { drop += kv.second.same + kv.second.diff; diff += kv.second.diff; }
        bool asc = true;
        for (size_t i = 1; i < kept_.size(); ++i) asc = asc && kept_[i - 1].t < kept_[i].t;
        cp(U(u8"병합·정렬·중복 제거"), asc && kept_.size() + drop == total, to_string(total) + U(u8"행 -> ") + to_string(kept_.size()) + U(u8"행 (중복 제거 ") + to_string(drop) + U(u8"행)"));
        for (const auto& c : conflicts_) fail(U(u8"값이 다른 중복: ") + c);
        if (diff > conflicts_.size()) fail(U(u8"값이 다른 중복 추가 ") + to_string(diff - conflicts_.size()) + U(u8"행 (목록 생략)"));
        csv_ = dir_ / "RAW.csv";
        bool w = write_csv();
        cp(U(u8"RAW.csv 작성"), w, to_string(kept_.size()) + U(u8"행 기록"));
        cp(U(u8"RAW.csv 재읽기 검증"), w && verify_csv(), U(u8"헤더·행 수·스캔 번호·값 일치"));
        cp(U(u8"특이사항.txt 작성"), report(), U(u8"시간 공백·간격 이상·중복·오류 목록"));
        note(U(u8"[END] 오류 ") + to_string(errors_) + U(u8"건"));
        cout << "merge done: files=" << order_.size() << "/" << srcs_.size() << " rows=" << kept_.size() << " dup_removed=" << drop << " gaps=" << gapCount_ << " errors=" << errors_ << "\n";
        return 0;
    }
};

int main(int argc, char** argv) {
    filesystem::path dir = argc > 1 ? filesystem::path(argv[1]) : filesystem::current_path();
    error_code ec;
    if (!filesystem::is_directory(dir, ec)) { cerr << "folder not found\n"; return 2; }
    return Merger(dir).run();
}