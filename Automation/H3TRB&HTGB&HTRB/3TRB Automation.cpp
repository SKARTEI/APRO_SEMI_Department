#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;
using Bytes = vector<uint8_t>;

inline string U(const char* s) { return string(s); }
#ifdef __cpp_char8_t
inline string U(const char8_t* s) { return string(reinterpret_cast<const char*>(s)); }
#endif

namespace {

    constexpr const char* kDefaultChartTitle = "Trend [nA]";
    constexpr array<const char*, 24> kSeriesColors = 
    {
        "5B9BD5", "ED7D31", "A5A5A5", "FFC000", "4472C4", "70AD47", "255E91", "9E480E",
        "636363", "997300", "264478", "43682B", "7CAFDD", "F1975A", "B7B7B7", "FFCD33",
        "698ED0", "8CC168", "327DC2", "D26012", "848484", "CC9A00", "335AA1", "5A8A39" };

    uint32_t rd16(const Bytes& b, size_t p)
    {
        if (p + 2 > b.size()) throw runtime_error("zip: truncated");
        return static_cast<uint32_t>(b[p]) | (static_cast<uint32_t>(b[p + 1]) << 8);
    }

    uint32_t rd32(const Bytes& b, size_t p)
    {
        if (p + 4 > b.size()) throw runtime_error("zip: truncated");
        return static_cast<uint32_t>(b[p]) | (static_cast<uint32_t>(b[p + 1]) << 8) |
            (static_cast<uint32_t>(b[p + 2]) << 16) | (static_cast<uint32_t>(b[p + 3]) << 24);
    }

    Bytes read_file(const filesystem::path& path)
    {
        ifstream in(path, ios::binary);
        if (!in) throw runtime_error("cannot open input file: " + path.string());
        in.seekg(0, ios::end);
        const streamoff size = in.tellg();
        in.seekg(0, ios::beg);
        Bytes data(static_cast<size_t>(size));
        if (size > 0) in.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }

    void write_file(const filesystem::path& path, const Bytes& data)
    {
        ofstream out(path, ios::binary | ios::trunc);
        if (!out) throw runtime_error("cannot create output file: " + path.string());
        if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), static_cast<streamsize>(data.size()));
        if (!out) throw runtime_error("write failed: " + path.string());
    }

    struct BitReader {
        const uint8_t* data;
        size_t size;
        size_t pos = 0;
        uint32_t buf = 0;
        int cnt = 0;

        int bits(int need)
        {
            uint32_t v = buf;
            while (cnt < need) {
                if (pos >= size) throw runtime_error("inflate: out of data");
                v |= static_cast<uint32_t>(data[pos++]) << cnt;
                cnt += 8;
            }
            buf = v >> need;
            cnt -= need;
            return static_cast<int>(v & ((1u << need) - 1u));
        }
    };

    struct Huff {
        uint16_t count[16];
        uint16_t symbol[320];
    };

    void build_huff(Huff& h, const uint8_t* lengths, int n)
    {
        fill(begin(h.count), end(h.count), static_cast<uint16_t>(0));
        fill(begin(h.symbol), end(h.symbol), static_cast<uint16_t>(0));
        for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + h.count[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i] != 0) h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }

    int decode_sym(BitReader& br, const Huff& h)
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= br.bits(1);
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw runtime_error("inflate: bad code");
    }

    void inflate_codes(BitReader& br, Bytes& out, const Huff& lencode, const Huff& distcode)
    {
        static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const uint16_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static const uint16_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        for (;;) {
            int sym = decode_sym(br, lencode);
            if (sym < 256) {
                out.push_back(static_cast<uint8_t>(sym));
            }
            else if (sym == 256) {
                return;
            }
            else {
                sym -= 257;
                if (sym >= 29) throw runtime_error("inflate: bad length symbol");
                const int len = lbase[sym] + br.bits(lext[sym]);
                const int ds = decode_sym(br, distcode);
                if (ds >= 30) throw runtime_error("inflate: bad distance symbol");
                const size_t dist = static_cast<size_t>(dbase[ds] + br.bits(dext[ds]));
                if (dist > out.size()) throw runtime_error("inflate: distance too far");
                for (int i = 0; i < len; ++i) out.push_back(out[out.size() - dist]);
            }
        }
    }

    Bytes inflate_raw(const uint8_t* src, size_t srcSize, size_t expected)
    {
        BitReader br{ src, srcSize };
        Bytes out;
        out.reserve(expected);
        int last = 0;
        do {
            last = br.bits(1);
            const int type = br.bits(2);
            if (type == 0) {
                br.buf = 0;
                br.cnt = 0;
                if (br.pos + 4 > br.size) throw runtime_error("inflate: stored header");
                const size_t len = static_cast<size_t>(br.data[br.pos]) | (static_cast<size_t>(br.data[br.pos + 1]) << 8);
                br.pos += 4;
                if (br.pos + len > br.size) throw runtime_error("inflate: stored data");
                out.insert(out.end(), br.data + br.pos, br.data + br.pos + len);
                br.pos += len;
            }
            else if (type == 1) {
                uint8_t lengths[288];
                int i = 0;
                for (; i < 144; ++i) lengths[i] = 8;
                for (; i < 256; ++i) lengths[i] = 9;
                for (; i < 280; ++i) lengths[i] = 7;
                for (; i < 288; ++i) lengths[i] = 8;
                Huff lencode;
                build_huff(lencode, lengths, 288);
                uint8_t dl[30];
                fill(begin(dl), end(dl), static_cast<uint8_t>(5));
                Huff distcode;
                build_huff(distcode, dl, 30);
                inflate_codes(br, out, lencode, distcode);
            }
            else if (type == 2) {
                static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                const int nlen = br.bits(5) + 257;
                const int ndist = br.bits(5) + 1;
                const int ncode = br.bits(4) + 4;
                if (nlen > 286 || ndist > 30) throw runtime_error("inflate: bad counts");
                uint8_t cl[19] = {};
                for (int i = 0; i < ncode; ++i) cl[order[i]] = static_cast<uint8_t>(br.bits(3));
                Huff clcode;
                build_huff(clcode, cl, 19);
                uint8_t lens[320] = {};
                int index = 0;
                while (index < nlen + ndist) {
                    const int sym = decode_sym(br, clcode);
                    if (sym < 16) {
                        lens[index++] = static_cast<uint8_t>(sym);
                    }
                    else {
                        int value = 0;
                        int rep = 0;
                        if (sym == 16) {
                            if (index == 0) throw runtime_error("inflate: bad repeat");
                            value = lens[index - 1];
                            rep = 3 + br.bits(2);
                        }
                        else if (sym == 17) {
                            rep = 3 + br.bits(3);
                        }
                        else {
                            rep = 11 + br.bits(7);
                        }
                        if (index + rep > nlen + ndist) throw runtime_error("inflate: repeat overflow");
                        while (rep-- > 0) lens[index++] = static_cast<uint8_t>(value);
                    }
                }
                Huff lencode;
                Huff distcode;
                build_huff(lencode, lens, nlen);
                build_huff(distcode, lens + nlen, ndist);
                inflate_codes(br, out, lencode, distcode);
            }
            else {
                throw runtime_error("inflate: bad block type");
            }
        } while (!last);
        return out;
    }

    struct ZipEntry {
        uint32_t method = 0;
        uint32_t csize = 0;
        uint32_t usize = 0;
        uint32_t offset = 0;
    };

    map<string, ZipEntry> zip_index(const Bytes& z)
    {
        if (z.size() < 22) throw runtime_error("not a zip file");
        size_t eocd = string::npos;
        const size_t minPos = z.size() > 65557 ? z.size() - 65557 : 0;
        for (size_t p = z.size() - 22 + 1; p-- > minPos;) {
            if (rd32(z, p) == 0x06054b50u) {
                eocd = p;
                break;
            }
        }
        if (eocd == string::npos) throw runtime_error("zip: end record not found");
        const size_t total = rd16(z, eocd + 10);
        size_t p = rd32(z, eocd + 16);
        map<string, ZipEntry> index;
        for (size_t i = 0; i < total; ++i) {
            if (rd32(z, p) != 0x02014b50u) throw runtime_error("zip: bad central directory");
            ZipEntry e;
            e.method = rd16(z, p + 10);
            e.csize = rd32(z, p + 20);
            e.usize = rd32(z, p + 24);
            const size_t nlen = rd16(z, p + 28);
            const size_t elen = rd16(z, p + 30);
            const size_t clen = rd16(z, p + 32);
            e.offset = rd32(z, p + 42);
            if (p + 46 + nlen > z.size()) throw runtime_error("zip: truncated");
            index[string(reinterpret_cast<const char*>(z.data() + p + 46), nlen)] = e;
            p += 46 + nlen + elen + clen;
        }
        return index;
    }

    string zip_extract(const Bytes& z, const map<string, ZipEntry>& index, const string& name)
    {
        const auto it = index.find(name);
        if (it == index.end()) throw runtime_error("zip: entry not found: " + name);
        const ZipEntry& e = it->second;
        if (rd32(z, e.offset) != 0x04034b50u) throw runtime_error("zip: bad local header");
        const size_t nlen = rd16(z, e.offset + 26);
        const size_t elen = rd16(z, e.offset + 28);
        const size_t start = e.offset + 30 + nlen + elen;
        if (start + e.csize > z.size()) throw runtime_error("zip: truncated entry");
        if (e.method == 0) return string(reinterpret_cast<const char*>(z.data() + start), e.csize);
        if (e.method != 8) throw runtime_error("zip: unsupported compression method");
        const Bytes out = inflate_raw(z.data() + start, e.csize, e.usize);
        return string(reinterpret_cast<const char*>(out.data()), out.size());
    }

    void append_utf8(string& s, unsigned long cp)
    {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    string xml_unescape(const string& s)
    {
        string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '&') {
                out.push_back(s[i]);
                continue;
            }
            const size_t semi = s.find(';', i);
            if (semi == string::npos || semi - i > 10) {
                out.push_back(s[i]);
                continue;
            }
            const string ent = s.substr(i + 1, semi - i - 1);
            if (ent == "amp") out.push_back('&');
            else if (ent == "lt") out.push_back('<');
            else if (ent == "gt") out.push_back('>');
            else if (ent == "quot") out.push_back('"');
            else if (ent == "apos") out.push_back('\'');
            else if (!ent.empty() && ent[0] == '#') {
                const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                const unsigned long cp = strtoul(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
                append_utf8(out, cp);
            }
            else {
                out.append(s, i, semi - i + 1);
            }
            i = semi;
        }
        return out;
    }

    string xml_escape(const string& s)
    {
        string out;
        out.reserve(s.size());
        for (const char c : s) {
            if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else out.push_back(c);
        }
        return out;
    }

    string attr_value(const string& tag, const string& name)
    {
        const string key = " " + name + "=\"";
        const size_t p = tag.find(key);
        if (p == string::npos) return string();
        const size_t s = p + key.size();
        const size_t e = tag.find('"', s);
        if (e == string::npos) return string();
        return tag.substr(s, e - s);
    }

    string collect_text(const string& block)
    {
        string cleaned;
        size_t pos = 0;
        for (;;) {
            const size_t a = block.find("<rPh", pos);
            if (a == string::npos) {
                cleaned.append(block, pos, string::npos);
                break;
            }
            cleaned.append(block, pos, a - pos);
            const size_t b = block.find("</rPh>", a);
            if (b == string::npos) break;
            pos = b + 6;
        }
        string text;
        pos = 0;
        for (;;) {
            const size_t a = cleaned.find("<t", pos);
            if (a == string::npos) break;
            const char next = a + 2 < cleaned.size() ? cleaned[a + 2] : '\0';
            if (next != '>' && next != ' ' && next != '/') {
                pos = a + 2;
                continue;
            }
            const size_t gt = cleaned.find('>', a);
            if (gt == string::npos) break;
            if (cleaned[gt - 1] == '/') {
                pos = gt + 1;
                continue;
            }
            const size_t end = cleaned.find("</t>", gt);
            if (end == string::npos) break;
            text += xml_unescape(cleaned.substr(gt + 1, end - gt - 1));
            pos = end + 4;
        }
        return text;
    }

    vector<string> parse_shared_strings(const string& xml)
    {
        vector<string> list;
        size_t pos = 0;
        for (;;) {
            const size_t a = xml.find("<si", pos);
            if (a == string::npos) break;
            const char next = a + 3 < xml.size() ? xml[a + 3] : '\0';
            if (next != '>' && next != ' ' && next != '/') {
                pos = a + 3;
                continue;
            }
            const size_t gt = xml.find('>', a);
            if (gt == string::npos) break;
            if (xml[gt - 1] == '/') {
                list.emplace_back();
                pos = gt + 1;
                continue;
            }
            const size_t end = xml.find("</si>", gt);
            if (end == string::npos) break;
            list.push_back(collect_text(xml.substr(gt + 1, end - gt - 1)));
            pos = end + 5;
        }
        return list;
    }

    struct Cell {
        int col = 0;
        bool isText = false;
        string text;
        double num = 0.0;
    };

    using Row = vector<Cell>;

    int column_index(const string& ref)
    {
        int col = 0;
        for (const char c : ref) {
            if (c < 'A' || c > 'Z') break;
            col = col * 26 + (c - 'A' + 1);
        }
        return col - 1;
    }

    string column_name(int col)
    {
        string name;
        int n = col + 1;
        while (n > 0) {
            const int r = (n - 1) % 26;
            name.insert(name.begin(), static_cast<char>('A' + r));
            n = (n - 1) / 26;
        }
        return name;
    }

    string inner_tag(const string& body, const string& tag)
    {
        const string open = "<" + tag;
        const size_t a = body.find(open);
        if (a == string::npos) return string();
        const size_t gt = body.find('>', a);
        if (gt == string::npos || body[gt - 1] == '/') return string();
        const size_t end = body.find("</" + tag + ">", gt);
        if (end == string::npos) return string();
        return body.substr(gt + 1, end - gt - 1);
    }

    vector<Row> parse_sheet(const string& xml, const vector<string>& shared)
    {
        vector<Row> rows;
        size_t pos = xml.find("<sheetData");
        if (pos == string::npos) throw runtime_error("sheet: no sheetData");
        for (;;) {
            const size_t ra = xml.find("<row ", pos);
            if (ra == string::npos) break;
            const size_t rgt = xml.find('>', ra);
            if (rgt == string::npos) break;
            Row row;
            if (xml[rgt - 1] == '/') {
                rows.push_back(row);
                pos = rgt + 1;
                continue;
            }
            const size_t rend = xml.find("</row>", rgt);
            if (rend == string::npos) break;
            size_t cp = rgt + 1;
            while (cp < rend) {
                const size_t ca = xml.find("<c ", cp);
                if (ca == string::npos || ca >= rend) break;
                const size_t cgt = xml.find('>', ca);
                if (cgt == string::npos) break;
                const string tag = xml.substr(ca, cgt - ca + 1);
                string body;
                if (xml[cgt - 1] == '/') {
                    cp = cgt + 1;
                    continue;
                }
                const size_t cend = xml.find("</c>", cgt);
                if (cend == string::npos) break;
                body = xml.substr(cgt + 1, cend - cgt - 1);
                cp = cend + 4;
                Cell cell;
                cell.col = column_index(attr_value(tag, "r"));
                const string type = attr_value(tag, "t");
                if (type == "s") {
                    const string v = inner_tag(body, "v");
                    const size_t idx = static_cast<size_t>(strtoull(v.c_str(), nullptr, 10));
                    if (idx >= shared.size()) throw runtime_error("sheet: bad shared string index");
                    cell.isText = true;
                    cell.text = shared[idx];
                }
                else if (type == "inlineStr") {
                    cell.isText = true;
                    cell.text = collect_text(inner_tag(body, "is"));
                }
                else if (type == "str" || type == "b" || type == "e") {
                    cell.isText = true;
                    cell.text = xml_unescape(inner_tag(body, "v"));
                }
                else {
                    const string v = inner_tag(body, "v");
                    if (v.empty()) continue;
                    cell.text = v;
                    cell.num = strtod(v.c_str(), nullptr);
                }
                row.push_back(cell);
            }
            rows.push_back(row);
            pos = rend + 6;
        }
        return rows;
    }

    array<uint32_t, 256> make_crc_table()
    {
        array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        return table;
    }

    uint32_t crc32_of(const string& data)
    {
        static const array<uint32_t, 256> table = make_crc_table();
        uint32_t c = 0xFFFFFFFFu;
        for (const char ch : data) c = table[(c ^ static_cast<uint8_t>(ch)) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    void put16(Bytes& b, uint32_t v)
    {
        b.push_back(static_cast<uint8_t>(v & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }

    void put32(Bytes& b, uint32_t v)
    {
        put16(b, v & 0xFFFF);
        put16(b, (v >> 16) & 0xFFFF);
    }

    Bytes zip_store(const vector<pair<string, string>>& files)
    {
        Bytes out;
        Bytes central;
        for (const auto& f : files) {
            const uint32_t crc = crc32_of(f.second);
            const uint32_t size = static_cast<uint32_t>(f.second.size());
            const uint32_t offset = static_cast<uint32_t>(out.size());
            put32(out, 0x04034b50u);
            put16(out, 20);
            put16(out, 0);
            put16(out, 0);
            put16(out, 0);
            put16(out, 0x0021);
            put32(out, crc);
            put32(out, size);
            put32(out, size);
            put16(out, static_cast<uint32_t>(f.first.size()));
            put16(out, 0);
            out.insert(out.end(), f.first.begin(), f.first.end());
            out.insert(out.end(), f.second.begin(), f.second.end());
            put32(central, 0x02014b50u);
            put16(central, 20);
            put16(central, 20);
            put16(central, 0);
            put16(central, 0);
            put16(central, 0);
            put16(central, 0x0021);
            put32(central, crc);
            put32(central, size);
            put32(central, size);
            put16(central, static_cast<uint32_t>(f.first.size()));
            put16(central, 0);
            put16(central, 0);
            put16(central, 0);
            put16(central, 0);
            put32(central, 0);
            put32(central, offset);
            central.insert(central.end(), f.first.begin(), f.first.end());
        }
        const uint32_t cdOffset = static_cast<uint32_t>(out.size());
        out.insert(out.end(), central.begin(), central.end());
        put32(out, 0x06054b50u);
        put16(out, 0);
        put16(out, 0);
        put16(out, static_cast<uint32_t>(files.size()));
        put16(out, static_cast<uint32_t>(files.size()));
        put32(out, static_cast<uint32_t>(central.size()));
        put32(out, cdOffset);
        put16(out, 0);
        return out;
    }

    struct OutCell {
        bool present = false;
        string text;
    };

    struct Table {
        vector<string> header;
        vector<vector<OutCell>> rows;
        int scanCol = -1;
        int timeCol = -1;
        vector<int> chartCols;
    };

    string build_sheet_xml(const Table& t, int valueStyle)
    {
        const int ncols = static_cast<int>(t.header.size());
        const size_t nrows = t.rows.size() + 1;
        string x;
        x.reserve(t.rows.size() * static_cast<size_t>(ncols) * 36 + 4096);
        x += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
        x += "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
        x += "<dimension ref=\"A1:" + column_name(ncols - 1) + to_string(nrows) + "\"/>";
        x += "<sheetViews><sheetView tabSelected=\"1\" workbookViewId=\"0\"/></sheetViews>";
        x += "<sheetFormatPr defaultRowHeight=\"16.5\"/>";
        x += "<cols><col min=\"1\" max=\"1\" width=\"9\" style=\"1\"/>";
        x += "<col min=\"2\" max=\"2\" width=\"16.625\" style=\"1\" customWidth=\"1\"/>";
        x += "<col min=\"3\" max=\"16384\" width=\"9\" style=\"1\"/></cols>";
        x += "<sheetData>";
        x += "<row r=\"1\">";
        for (int c = 0; c < ncols; ++c) {
            x += "<c r=\"" + column_name(c) + "1\" s=\"1\" t=\"inlineStr\"><is><t>" + xml_escape(t.header[static_cast<size_t>(c)]) + "</t></is></c>";
        }
        x += "</row>";
        for (size_t r = 0; r < t.rows.size(); ++r) {
            const string rn = to_string(r + 2);
            x += "<row r=\"" + rn + "\">";
            for (int c = 0; c < ncols; ++c) {
                const OutCell& cell = t.rows[r][static_cast<size_t>(c)];
                if (!cell.present) continue;
                int style = valueStyle;
                if (c == t.scanCol) style = 1;
                else if (c == t.timeCol) style = 2;
                x += "<c r=\"" + column_name(c) + rn + "\" s=\"" + to_string(style) + "\"><v>" + cell.text + "</v></c>";
            }
            x += "</row>";
        }
        x += "</sheetData>";
        x += "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>";
        if (!t.chartCols.empty()) x += "<drawing r:id=\"rId1\"/>";
        x += "</worksheet>";
        return x;
    }

    string chart_text_props(int size, const char* color)
    {
        return "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"" + to_string(size) + "\" b=\"0\"><a:solidFill><a:srgbClr val=\"" + color + "\"/></a:solidFill></a:defRPr></a:pPr><a:endParaRPr lang=\"ko-KR\"/></a:p></c:txPr>";
    }

    string chart_axis_xml(const string& id, const string& cross, const char* pos, const char* fmt, const char* labelPos, int labelSize)
    {
        string x;
        x += "<c:valAx><c:axId val=\"" + id + "\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/>";
        x += string("<c:axPos val=\"") + pos + "\"/>";
        x += "<c:majorGridlines><c:spPr><a:ln w=\"9525\"><a:solidFill><a:srgbClr val=\"D9D9D9\"/></a:solidFill></a:ln></c:spPr></c:majorGridlines>";
        x += string("<c:numFmt formatCode=\"") + fmt + "\" sourceLinked=\"1\"/>";
        x += "<c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/>";
        x += string("<c:tickLblPos val=\"") + labelPos + "\"/>";
        x += "<c:spPr><a:noFill/><a:ln w=\"9525\"><a:solidFill><a:srgbClr val=\"BFBFBF\"/></a:solidFill></a:ln></c:spPr>";
        x += chart_text_props(labelSize, "595959");
        x += "<c:crossAx val=\"" + cross + "\"/><c:crosses val=\"autoZero\"/><c:crossBetween val=\"midCat\"/></c:valAx>";
        return x;
    }

    string build_chart_xml(const Table& t, const string& sheetName, size_t lastRow, const string& title)
    {
        const string last = to_string(lastRow);
        const string xRef = sheetName + "!$" + column_name(t.timeCol) + "$2:$" + column_name(t.timeCol) + "$" + last;
        string x;
        x += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
        x += "<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
        x += "<c:date1904 val=\"0\"/><c:lang val=\"ko-KR\"/><c:roundedCorners val=\"0\"/><c:chart>";
        x += "<c:title><c:tx><c:rich><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"2400\" b=\"0\"/></a:pPr><a:r><a:rPr lang=\"en-US\" sz=\"2400\" b=\"0\"><a:solidFill><a:srgbClr val=\"595959\"/></a:solidFill></a:rPr><a:t>";
        x += xml_escape(title);
        x += "</a:t></a:r></a:p></c:rich></c:tx><c:overlay val=\"0\"/></c:title><c:autoTitleDeleted val=\"0\"/>";
        x += "<c:plotArea><c:layout/><c:scatterChart><c:scatterStyle val=\"lineMarker\"/><c:varyColors val=\"0\"/>";
        for (const int col : t.chartCols) {
            const string cn = column_name(col);
            const string idx = to_string(col - 2);
            const string color = kSeriesColors[static_cast<size_t>(col - 2) % kSeriesColors.size()];
            x += "<c:ser><c:idx val=\"" + idx + "\"/><c:order val=\"" + idx + "\"/>";
            x += "<c:tx><c:strRef><c:f>" + sheetName + "!$" + cn + "$1</c:f></c:strRef></c:tx>";
            x += "<c:spPr><a:ln w=\"19050\" cap=\"rnd\"><a:noFill/><a:round/></a:ln></c:spPr>";
            x += "<c:marker><c:symbol val=\"circle\"/><c:size val=\"5\"/><c:spPr><a:solidFill><a:srgbClr val=\"" + color + "\"/></a:solidFill><a:ln w=\"9525\"><a:solidFill><a:srgbClr val=\"" + color + "\"/></a:solidFill></a:ln></c:spPr></c:marker>";
            x += "<c:xVal><c:numRef><c:f>" + xRef + "</c:f></c:numRef></c:xVal>";
            x += "<c:yVal><c:numRef><c:f>" + sheetName + "!$" + cn + "$2:$" + cn + "$" + last + "</c:f></c:numRef></c:yVal>";
            x += "<c:smooth val=\"0\"/></c:ser>";
        }
        x += "<c:dLbls><c:showLegendKey val=\"0\"/><c:showVal val=\"0\"/><c:showCatName val=\"0\"/><c:showSerName val=\"0\"/><c:showPercent val=\"0\"/><c:showBubbleSize val=\"0\"/></c:dLbls>";
        x += "<c:axId val=\"1884610511\"/><c:axId val=\"1884610991\"/></c:scatterChart>";
        x += chart_axis_xml("1884610511", "1884610991", "b", "m/d/yyyy\\ h:mm", "low", 900);
        x += chart_axis_xml("1884610991", "1884610511", "l", "General", "nextTo", 1200);
        x += "<c:spPr><a:noFill/><a:ln><a:noFill/></a:ln></c:spPr></c:plotArea>";
        x += "<c:legend><c:legendPos val=\"b\"/><c:overlay val=\"0\"/>" + chart_text_props(900, "595959") + "</c:legend>";
        x += "<c:plotVisOnly val=\"1\"/><c:dispBlanksAs val=\"gap\"/></c:chart>";
        x += "<c:spPr><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill><a:ln w=\"9525\"><a:solidFill><a:srgbClr val=\"D9D9D9\"/></a:solidFill></a:ln></c:spPr>";
        x += "</c:chartSpace>";
        return x;
    }

    string build_drawing_xml(int fromCol)
    {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<xdr:wsDr xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
            "<xdr:twoCellAnchor><xdr:from><xdr:col>" + to_string(fromCol) + "</xdr:col><xdr:colOff>3710</xdr:colOff><xdr:row>0</xdr:row><xdr:rowOff>191737</xdr:rowOff></xdr:from>"
            "<xdr:to><xdr:col>" + to_string(fromCol + 15) + "</xdr:col><xdr:colOff>325193</xdr:colOff><xdr:row>34</xdr:row><xdr:rowOff>39077</xdr:rowOff></xdr:to>"
            "<xdr:graphicFrame macro=\"\"><xdr:nvGraphicFramePr><xdr:cNvPr id=\"2\" name=\"Chart 1\"/><xdr:cNvGraphicFramePr/></xdr:nvGraphicFramePr>"
            "<xdr:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/></xdr:xfrm>"
            "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">"
            "<c:chart xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" r:id=\"rId1\"/>"
            "</a:graphicData></a:graphic></xdr:graphicFrame><xdr:clientData/></xdr:twoCellAnchor></xdr:wsDr>";
    }

    Bytes build_workbook(const Table& t, const string& sheetName, int valueStyle, const string& chartTitle)
    {
        vector<pair<string, string>> files;
        files.emplace_back("[Content_Types].xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
            "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
            "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>" +
            string(t.chartCols.empty() ? "" :
                "<Override PartName=\"/xl/drawings/drawing1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.drawing+xml\"/>"
                "<Override PartName=\"/xl/charts/chart1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.drawingml.chart+xml\"/>") +
            "</Types>");
        files.emplace_back("_rels/.rels",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
            "</Relationships>");
        files.emplace_back("xl/workbook.xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
            "<sheets><sheet name=\"" + xml_escape(sheetName) + "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");
        files.emplace_back("xl/_rels/workbook.xml.rels",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
            "</Relationships>");
        files.emplace_back("xl/styles.xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
            "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"" + U(u8"맑은 고딕") + "\"/><family val=\"2\"/></font></fonts>"
            "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
            "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
            "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
            "<cellXfs count=\"4\">"
            "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
            "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>"
            "<xf numFmtId=\"22\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>"
            "<xf numFmtId=\"11\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>"
            "</cellXfs>"
            "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
            "</styleSheet>");
        files.emplace_back("xl/worksheets/sheet1.xml", build_sheet_xml(t, valueStyle));
        if (!t.chartCols.empty()) {
            files.emplace_back("xl/worksheets/_rels/sheet1.xml.rels",
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/drawing\" Target=\"../drawings/drawing1.xml\"/>"
                "</Relationships>");
            files.emplace_back("xl/drawings/drawing1.xml", build_drawing_xml(static_cast<int>(t.header.size()) + 3));
            files.emplace_back("xl/drawings/_rels/drawing1.xml.rels",
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart\" Target=\"../charts/chart1.xml\"/>"
                "</Relationships>");
            files.emplace_back("xl/charts/chart1.xml", build_chart_xml(t, sheetName, t.rows.size() + 1, chartTitle));
        }
        return zip_store(files);
    }

    string format_scaled(double v, double factor)
    {
        double r = v * factor;
        if (r == 0.0) r = 0.0;
        char buf[48];
        snprintf(buf, sizeof(buf), "%.15g", r);
        return string(buf);
    }

    bool parse_factor(const string& text, double& value)
    {
        if (text.empty()) return false;
        char* end = nullptr;
        value = strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0') return false;
        return isfinite(value) && value != 0.0;
    }

    bool starts_with(const string& s, const string& prefix)
    {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

}

int main(int argc, char** argv)
{
    try {
        const filesystem::path input = argc > 1 ? filesystem::path(argv[1]) : filesystem::path("RAW.xlsx");
        const filesystem::path outDir = argc > 2 ? filesystem::path(argv[2]) : (input.has_parent_path() ? input.parent_path() : filesystem::path("."));
        string factorText = argc > 3 ? string(argv[3]) : string();
        const string chartTitle = argc > 4 ? string(argv[4]) : string(kDefaultChartTitle);
        double factor = 0.0;
        while (!parse_factor(factorText, factor)) {
            if (argc > 3 && !factorText.empty()) cerr << "invalid factor: " << factorText << "\n";
            cout << "STEP2 factor: ";
            if (!getline(cin, factorText)) throw runtime_error("factor input failed");
            factorText.erase(remove_if(factorText.begin(), factorText.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r'; }), factorText.end());
            if (argc > 3) argc = 3;
        }
        filesystem::create_directories(outDir);

        const Bytes zip = read_file(input);
        const auto index = zip_index(zip);

        vector<string> shared;
        if (index.count("xl/sharedStrings.xml")) shared = parse_shared_strings(zip_extract(zip, index, "xl/sharedStrings.xml"));
        const vector<Row> rows = parse_sheet(zip_extract(zip, index, "xl/worksheets/sheet1.xml"), shared);

        const string scanName = U(u8"스캔");
        const string timeName = U(u8"시간");
        const string alarmPrefix = U(u8"경보");

        size_t headerRow = rows.size();
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!rows[i].empty() && rows[i][0].col == 0 && rows[i][0].isText && rows[i][0].text == scanName) {
                headerRow = i;
                break;
            }
        }
        if (headerRow == rows.size()) throw runtime_error("data header row not found");

        vector<int> keepCols;
        vector<string> names;
        int removed = 0;
        for (const Cell& c : rows[headerRow]) {
            if (!c.isText || c.text.empty()) continue;
            if (starts_with(c.text, alarmPrefix)) {
                ++removed;
                continue;
            }
            keepCols.push_back(c.col);
            names.push_back(c.text);
        }

        Table step1;
        Table step2;
        step1.header = names;
        step2.header = names;
        for (size_t k = 0; k < names.size(); ++k) {
            if (names[k] == scanName) {
                step1.scanCol = step2.scanCol = static_cast<int>(k);
            }
            else if (names[k] == timeName) {
                step1.timeCol = step2.timeCol = static_cast<int>(k);
            }
        }
        if (step1.scanCol < 0 || step1.timeCol < 0) throw runtime_error("scan/time columns not found");

        for (size_t i = headerRow + 1; i < rows.size(); ++i) {
            const Row& row = rows[i];
            if (row.empty() || row[0].col != 0 || row[0].isText) continue;
            map<int, const Cell*> byCol;
            for (const Cell& c : row) byCol[c.col] = &c;
            vector<OutCell> o1(names.size());
            vector<OutCell> o2(names.size());
            for (size_t k = 0; k < keepCols.size(); ++k) {
                const auto it = byCol.find(keepCols[k]);
                if (it == byCol.end() || it->second->isText) continue;
                o1[k].present = true;
                o1[k].text = it->second->text;
                o2[k].present = true;
                if (static_cast<int>(k) == step1.scanCol || static_cast<int>(k) == step1.timeCol) o2[k].text = it->second->text;
                else o2[k].text = format_scaled(it->second->num, factor);
            }
            step1.rows.push_back(move(o1));
            step2.rows.push_back(move(o2));
        }
        if (step1.rows.empty()) throw runtime_error("no data rows found");

        const int valueCols = static_cast<int>(names.size()) - 2;
        for (size_t k = 0; k < names.size(); ++k) {
            if (static_cast<int>(k) == step2.scanCol || static_cast<int>(k) == step2.timeCol) continue;
            step2.chartCols.push_back(static_cast<int>(k));
        }

        write_file(outDir / "STEP1.xlsx", build_workbook(step1, "STEP1", 3, chartTitle));
        write_file(outDir / "STEP2.xlsx", build_workbook(step2, "STEP2", 1, chartTitle));

        string readme;
        readme += "\xEF\xBB\xBF";
        readme += U(u8"RAW 데이터 정리 결과\r\n\r\n");
        readme += U(u8"입력 파일: ") + input.filename().string() + "\r\n";
        readme += U(u8"출력 파일: STEP1.xlsx, STEP2.xlsx, readme.txt\r\n");
        readme += U(u8"스캔 수(데이터 행): ") + to_string(step1.rows.size()) + "\r\n\r\n";
        readme += U(u8"[STEP1]\r\n");
        readme += U(u8"RAW 상단의 계측 설정 정보와 경보 열 ") + to_string(removed) + U(u8"개를 제거하고 스캔, 시간, 채널 값 ") + to_string(valueCols) + U(u8"개 열만 남김\r\n");
        readme += U(u8"수치는 RAW 원본 그대로\r\n\r\n");
        readme += U(u8"[STEP2]\r\n");
        readme += U(u8"STEP1의 전체 채널 수치(") + to_string(valueCols) + U(u8"개 열, 스캔/시간 열 제외)에 ") + factorText + U(u8"을(를) 곱함\r\n");
        readme += U(u8"그래프: 시간-채널 값 분산형 차트(") + to_string(step2.chartCols.size()) + U(u8"개 계열 전체), 제목 ") + chartTitle + "\r\n";

        write_file(outDir / "readme.txt", Bytes(readme.begin(), readme.end()));

        cout << "done: " << outDir.string() << " (rows=" << step1.rows.size() << ", removed alarm cols=" << removed << ")\n";
        return 0;
    }
    catch (const exception& e) {
        cerr << "error: " << e.what() << "\n";
        return 1;
    }
}