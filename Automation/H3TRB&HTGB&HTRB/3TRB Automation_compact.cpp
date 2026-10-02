#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;
using Bytes = vector<uint8_t>;
using Fld = pair<uint32_t, int>;

inline string U(const char* s) { return s; }
#ifdef __cpp_char8_t
inline string U(const char8_t* s) { return reinterpret_cast<const char*>(s); }
#endif

namespace {

    const char* kDefaultTitle = "Trend [nA]";
    const char* kColors[24] = { "5B9BD5", "ED7D31", "A5A5A5", "FFC000", "4472C4", "70AD47", "255E91", "9E480E", "636363", "997300", "264478", "43682B",
                               "7CAFDD", "F1975A", "B7B7B7", "FFCD33", "698ED0", "8CC168", "327DC2", "D26012", "848484", "CC9A00", "335AA1", "5A8A39" };
    const string kHdr = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
    const string kOx = "http://schemas.openxmlformats.org/";
    const string kNsMain = kOx + "spreadsheetml/2006/main", kNsR = kOx + "officeDocument/2006/relationships";
    const string kNsA = kOx + "drawingml/2006/main", kNsC = kOx + "drawingml/2006/chart";

    uint32_t rd(const Bytes& b, size_t p, int n) {
        if (p + n > b.size()) throw runtime_error("zip: truncated");
        uint32_t v = 0;
        for (int i = n - 1; i >= 0; --i) v = (v << 8) | b[p + i];
        return v;
    }

    void put(Bytes& b, initializer_list<Fld> fs) {
        for (auto [v, n] : fs) for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
    }

    Bytes read_file(const filesystem::path& path) {
        ifstream in(path, ios::binary);
        if (!in) throw runtime_error("cannot open input file: " + path.string());
        return Bytes((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
    }

    void write_file(const filesystem::path& path, const Bytes& d) {
        ofstream out(path, ios::binary | ios::trunc);
        if (!out) throw runtime_error("cannot create output file: " + path.string());
        out.write(reinterpret_cast<const char*>(d.data()), static_cast<streamsize>(d.size()));
        if (!out) throw runtime_error("write failed: " + path.string());
    }

    struct BitReader {
        const uint8_t* d;
        size_t n, pos = 0;
        uint32_t buf = 0;
        int cnt = 0;
        int bits(int need) {
            uint32_t v = buf;
            while (cnt < need) {
                if (pos >= n) throw runtime_error("inflate: out of data");
                v |= uint32_t(d[pos++]) << cnt;
                cnt += 8;
            }
            buf = v >> need;
            cnt -= need;
            return int(v & ((1u << need) - 1u));
        }
    };

    struct Huff { uint16_t count[16], symbol[320]; };

    void build_huff(Huff& h, const uint8_t* len, int n) {
        fill(begin(h.count), end(h.count), uint16_t(0));
        fill(begin(h.symbol), end(h.symbol), uint16_t(0));
        for (int i = 0; i < n; ++i) ++h.count[len[i]];
        uint16_t offs[16] = { 0, 0 };
        for (int l = 1; l < 15; ++l) offs[l + 1] = uint16_t(offs[l] + h.count[l]);
        for (int i = 0; i < n; ++i) if (len[i]) h.symbol[offs[len[i]]++] = uint16_t(i);
    }

    int decode_sym(BitReader& br, const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l <= 15; ++l) {
            code |= br.bits(1);
            int count = h.count[l];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw runtime_error("inflate: bad code");
    }

    void inflate_codes(BitReader& br, Bytes& out, const Huff& lc, const Huff& dc) {
        static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const uint16_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static const uint16_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        for (;;) {
            int sym = decode_sym(br, lc);
            if (sym < 256) { out.push_back(uint8_t(sym)); continue; }
            if (sym == 256) return;
            if ((sym -= 257) >= 29) throw runtime_error("inflate: bad length symbol");
            int len = lbase[sym] + br.bits(lext[sym]), ds = decode_sym(br, dc);
            if (ds >= 30) throw runtime_error("inflate: bad distance symbol");
            size_t dist = size_t(dbase[ds] + br.bits(dext[ds]));
            if (dist > out.size()) throw runtime_error("inflate: distance too far");
            for (int i = 0; i < len; ++i) out.push_back(out[out.size() - dist]);
        }
    }

    Bytes inflate_raw(const uint8_t* src, size_t n, size_t expected) {
        static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        BitReader br{ src, n };
        Bytes out;
        out.reserve(expected);
        for (int last = 0; !last;) {
            last = br.bits(1);
            int type = br.bits(2);
            Huff lc, dc;
            if (type == 0) {
                br.buf = 0;
                br.cnt = 0;
                if (br.pos + 4 > n) throw runtime_error("inflate: stored header");
                size_t len = size_t(src[br.pos]) | (size_t(src[br.pos + 1]) << 8);
                br.pos += 4;
                if (br.pos + len > n) throw runtime_error("inflate: stored data");
                out.insert(out.end(), src + br.pos, src + br.pos + len);
                br.pos += len;
                continue;
            }
            if (type == 1) {
                uint8_t l[288], dl[30];
                for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                fill(begin(dl), end(dl), uint8_t(5));
                build_huff(lc, l, 288);
                build_huff(dc, dl, 30);
            }
            else if (type == 2) {
                int nlen = br.bits(5) + 257, ndist = br.bits(5) + 1, ncode = br.bits(4) + 4;
                if (nlen > 286 || ndist > 30) throw runtime_error("inflate: bad counts");
                uint8_t cl[19] = {}, lens[320] = {};
                for (int i = 0; i < ncode; ++i) cl[order[i]] = uint8_t(br.bits(3));
                Huff cc;
                build_huff(cc, cl, 19);
                for (int i = 0; i < nlen + ndist;) {
                    int sym = decode_sym(br, cc), value = 0, rep = 1;
                    if (sym >= 16) {
                        if (sym == 16) {
                            if (i == 0) throw runtime_error("inflate: bad repeat");
                            value = lens[i - 1];
                            rep = 3 + br.bits(2);
                        }
                        else rep = sym == 17 ? 3 + br.bits(3) : 11 + br.bits(7);
                        if (i + rep > nlen + ndist) throw runtime_error("inflate: repeat overflow");
                    }
                    else value = sym;
                    while (rep-- > 0) lens[i++] = uint8_t(value);
                }
                build_huff(lc, lens, nlen);
                build_huff(dc, lens + nlen, ndist);
            }
            else throw runtime_error("inflate: bad block type");
            inflate_codes(br, out, lc, dc);
        }
        return out;
    }

    struct ZipEntry { uint32_t method, csize, usize, offset; };

    map<string, ZipEntry> zip_index(const Bytes& z) {
        if (z.size() < 22) throw runtime_error("not a zip file");
        size_t eocd = string::npos, lo = z.size() > 65557 ? z.size() - 65557 : 0;
        for (size_t p = z.size() - 21; p-- > lo;) if (rd(z, p, 4) == 0x06054b50u) { eocd = p; break; }
        if (eocd == string::npos) throw runtime_error("zip: end record not found");
        map<string, ZipEntry> idx;
        size_t p = rd(z, eocd + 16, 4), total = rd(z, eocd + 10, 2);
        for (size_t i = 0; i < total; ++i) {
            if (rd(z, p, 4) != 0x02014b50u) throw runtime_error("zip: bad central directory");
            size_t nl = rd(z, p + 28, 2), el = rd(z, p + 30, 2), cl = rd(z, p + 32, 2);
            if (p + 46 + nl > z.size()) throw runtime_error("zip: truncated");
            idx[string(reinterpret_cast<const char*>(z.data() + p + 46), nl)] = { rd(z, p + 10, 2), rd(z, p + 20, 4), rd(z, p + 24, 4), rd(z, p + 42, 4) };
            p += 46 + nl + el + cl;
        }
        return idx;
    }

    string zip_extract(const Bytes& z, const map<string, ZipEntry>& idx, const string& name) {
        auto it = idx.find(name);
        if (it == idx.end()) throw runtime_error("zip: entry not found: " + name);
        const ZipEntry& e = it->second;
        if (rd(z, e.offset, 4) != 0x04034b50u) throw runtime_error("zip: bad local header");
        size_t s = e.offset + 30 + rd(z, e.offset + 26, 2) + rd(z, e.offset + 28, 2);
        if (s + e.csize > z.size()) throw runtime_error("zip: truncated entry");
        if (e.method == 0) return string(reinterpret_cast<const char*>(z.data() + s), e.csize);
        if (e.method != 8) throw runtime_error("zip: unsupported compression method");
        Bytes o = inflate_raw(z.data() + s, e.csize, e.usize);
        return string(o.begin(), o.end());
    }

    uint32_t crc32_of(const string& d) {
        static const auto T = [] {
            array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            return t;
            }();
        uint32_t c = 0xFFFFFFFFu;
        for (char ch : d) c = T[(c ^ uint8_t(ch)) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    Bytes zip_store(const vector<pair<string, string>>& files) {
        Bytes out, central;
        for (auto& [name, data] : files) {
            uint32_t crc = crc32_of(data), size = uint32_t(data.size()), nl = uint32_t(name.size()), off = uint32_t(out.size());
            put(out, { {0x04034b50u, 4}, {20, 2}, {0, 2}, {0, 2}, {0, 2}, {0x21, 2}, {crc, 4}, {size, 4}, {size, 4}, {nl, 2}, {0, 2} });
            out.insert(out.end(), name.begin(), name.end());
            out.insert(out.end(), data.begin(), data.end());
            put(central, { {0x02014b50u, 4}, {20, 2}, {20, 2}, {0, 2}, {0, 2}, {0, 2}, {0x21, 2}, {crc, 4}, {size, 4}, {size, 4}, {nl, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 4}, {off, 4} });
            central.insert(central.end(), name.begin(), name.end());
        }
        uint32_t cdOff = uint32_t(out.size()), cnt = uint32_t(files.size());
        out.insert(out.end(), central.begin(), central.end());
        put(out, { {0x06054b50u, 4}, {0, 2}, {0, 2}, {cnt, 2}, {cnt, 2}, {uint32_t(central.size()), 4}, {cdOff, 4}, {0, 2} });
        return out;
    }

    void append_utf8(string& s, unsigned long c) {
        static const int lead[4] = { 0, 0xC0, 0xE0, 0xF0 };
        int n = c < 0x80 ? 0 : c < 0x800 ? 1 : c < 0x10000 ? 2 : 3;
        s += char(lead[n] | (c >> (6 * n)));
        for (int i = n - 1; i >= 0; --i) s += char(0x80 | ((c >> (6 * i)) & 0x3F));
    }

    string xml_unescape(const string& s) {
        static const map<string, char> named = { {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''} };
        string o;
        for (size_t i = 0; i < s.size(); ++i) {
            size_t sc = s[i] == '&' ? s.find(';', i) : string::npos;
            if (sc == string::npos || sc - i > 10) { o += s[i]; continue; }
            string e = s.substr(i + 1, sc - i - 1);
            if (named.count(e)) o += named.at(e);
            else if (e[0] == '#') {
                bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
                append_utf8(o, strtoul(e.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10));
            }
            else o.append(s, i, sc - i + 1);
            i = sc;
        }
        return o;
    }

    string xml_escape(const string& s) {
        string o;
        for (char c : s) o += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : string(1, c);
        return o;
    }

    string attr_value(const string& tag, const string& name) {
        size_t p = tag.find(" " + name + "=\"");
        if (p == string::npos) return "";
        p += name.size() + 3;
        return tag.substr(p, tag.find('"', p) - p);
    }

    template <class F>
    void each_tag(const string& x, const string& tag, size_t from, F f) {
        string open = "<" + tag;
        for (size_t p = from;;) {
            size_t a = x.find(open, p);
            if (a == string::npos) break;
            char nx = a + open.size() < x.size() ? x[a + open.size()] : '\0';
            if (nx != '>' && nx != ' ' && nx != '/') { p = a + open.size(); continue; }
            size_t g = x.find('>', a);
            if (g == string::npos) break;
            if (x[g - 1] == '/') { f(x.substr(a, g - a + 1), string(), true); p = g + 1; continue; }
            size_t e = x.find("</" + tag + ">", g);
            if (e == string::npos) break;
            f(x.substr(a, g - a + 1), x.substr(g + 1, e - g - 1), false);
            p = e + tag.size() + 3;
        }
    }

    string inner_tag(const string& body, const string& tag) {
        string r;
        bool done = false;
        each_tag(body, tag, 0, [&](const string&, const string& b, bool sc) { if (!done && !sc) { r = b; done = true; } });
        return r;
    }

    string collect_text(string b) {
        for (size_t a; (a = b.find("<rPh")) != string::npos;) {
            size_t e = b.find("</rPh>", a);
            b.erase(a, e == string::npos ? string::npos : e + 6 - a);
        }
        string t;
        each_tag(b, "t", 0, [&](const string&, const string& body, bool sc) { if (!sc) t += xml_unescape(body); });
        return t;
    }

    vector<string> parse_shared_strings(const string& xml) {
        vector<string> list;
        each_tag(xml, "si", 0, [&](const string&, const string& body, bool sc) { list.push_back(sc ? string() : collect_text(body)); });
        return list;
    }

    struct Cell {
        int col = 0;
        bool isText = false;
        string text;
        double num = 0.0;
    };
    using Row = vector<Cell>;

    int column_index(const string& ref) {
        int col = 0;
        for (char c : ref) { if (c < 'A' || c > 'Z') break; col = col * 26 + (c - 'A' + 1); }
        return col - 1;
    }

    string column_name(int col) {
        string name;
        for (int n = col + 1; n > 0; n = (n - 1) / 26) name.insert(name.begin(), char('A' + (n - 1) % 26));
        return name;
    }

    vector<Row> parse_sheet(const string& xml, const vector<string>& shared) {
        size_t sd = xml.find("<sheetData");
        if (sd == string::npos) throw runtime_error("sheet: no sheetData");
        vector<Row> rows;
        each_tag(xml, "row", sd, [&](const string&, const string& body, bool sc) {
            Row row;
            if (!sc) each_tag(body, "c", 0, [&](const string& tag, const string& cb, bool csc) {
                if (csc) return;
                Cell c;
                c.col = column_index(attr_value(tag, "r"));
                string type = attr_value(tag, "t"), v = inner_tag(cb, "v");
                if (type == "s") {
                    size_t i = strtoull(v.c_str(), nullptr, 10);
                    if (i >= shared.size()) throw runtime_error("sheet: bad shared string index");
                    c.isText = true;
                    c.text = shared[i];
                }
                else if (type == "inlineStr") {
                    c.isText = true;
                    c.text = collect_text(inner_tag(cb, "is"));
                }
                else if (type == "str" || type == "b" || type == "e") {
                    c.isText = true;
                    c.text = xml_unescape(v);
                }
                else {
                    if (v.empty()) return;
                    c.text = v;
                    c.num = strtod(v.c_str(), nullptr);
                }
                row.push_back(c);
                });
            rows.push_back(row);
            });
        return rows;
    }

    struct Table {
        vector<string> header;
        vector<vector<string>> rows;
        int scanCol = -1, timeCol = -1;
        vector<int> chartCols;
    };

    string clr(const string& c) { return "<a:solidFill><a:srgbClr val=\"" + c + "\"/></a:solidFill>"; }
    string ln9(const string& c) { return "<a:ln w=\"9525\">" + clr(c) + "</a:ln>"; }

    string sheet_xml(const Table& t, int valueStyle) {
        int nc = int(t.header.size());
        string x = kHdr + "<worksheet xmlns=\"" + kNsMain + "\" xmlns:r=\"" + kNsR + "\"><dimension ref=\"A1:" + column_name(nc - 1) + to_string(t.rows.size() + 1) + "\"/>";
        x += "<sheetViews><sheetView tabSelected=\"1\" workbookViewId=\"0\"/></sheetViews><sheetFormatPr defaultRowHeight=\"16.5\"/>";
        x += "<cols><col min=\"1\" max=\"1\" width=\"9\" style=\"1\"/><col min=\"2\" max=\"2\" width=\"16.625\" style=\"1\" customWidth=\"1\"/><col min=\"3\" max=\"16384\" width=\"9\" style=\"1\"/></cols><sheetData><row r=\"1\">";
        for (int c = 0; c < nc; ++c) x += "<c r=\"" + column_name(c) + "1\" s=\"1\" t=\"inlineStr\"><is><t>" + xml_escape(t.header[c]) + "</t></is></c>";
        x += "</row>";
        for (size_t r = 0; r < t.rows.size(); ++r) {
            string rn = to_string(r + 2);
            x += "<row r=\"" + rn + "\">";
            for (int c = 0; c < nc; ++c)
                if (!t.rows[r][c].empty()) x += "<c r=\"" + column_name(c) + rn + "\" s=\"" + to_string(c == t.scanCol ? 1 : c == t.timeCol ? 2 : valueStyle) + "\"><v>" + t.rows[r][c] + "</v></c>";
            x += "</row>";
        }
        x += "</sheetData><pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>";
        return x + (t.chartCols.empty() ? "" : "<drawing r:id=\"rId1\"/>") + "</worksheet>";
    }

    string chart_text(int size, const string& color) {
        return "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"" + to_string(size) + "\" b=\"0\">" + clr(color) + "</a:defRPr></a:pPr><a:endParaRPr lang=\"ko-KR\"/></a:p></c:txPr>";
    }

    string chart_axis(const string& id, const string& cross, const string& pos, const string& fmt, const string& labelPos, int labelSize) {
        return "<c:valAx><c:axId val=\"" + id + "\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"" + pos + "\"/>" +
            "<c:majorGridlines><c:spPr>" + ln9("D9D9D9") + "</c:spPr></c:majorGridlines><c:numFmt formatCode=\"" + fmt + "\" sourceLinked=\"1\"/>" +
            "<c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/><c:tickLblPos val=\"" + labelPos + "\"/><c:spPr><a:noFill/>" + ln9("BFBFBF") + "</c:spPr>" +
            chart_text(labelSize, "595959") + "<c:crossAx val=\"" + cross + "\"/><c:crosses val=\"autoZero\"/><c:crossBetween val=\"midCat\"/></c:valAx>";
    }

    string chart_xml(const Table& t, const string& sn, const string& title) {
        string last = to_string(t.rows.size() + 1), tc = column_name(t.timeCol), xRef = sn + "!$" + tc + "$2:$" + tc + "$" + last;
        string x = kHdr + "<c:chartSpace xmlns:c=\"" + kNsC + "\" xmlns:a=\"" + kNsA + "\" xmlns:r=\"" + kNsR + "\">";
        x += "<c:date1904 val=\"0\"/><c:lang val=\"ko-KR\"/><c:roundedCorners val=\"0\"/><c:chart><c:title><c:tx><c:rich><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"2400\" b=\"0\"/></a:pPr>";
        x += "<a:r><a:rPr lang=\"en-US\" sz=\"2400\" b=\"0\">" + clr("595959") + "</a:rPr><a:t>" + xml_escape(title) + "</a:t></a:r></a:p></c:rich></c:tx><c:overlay val=\"0\"/></c:title><c:autoTitleDeleted val=\"0\"/>";
        x += "<c:plotArea><c:layout/><c:scatterChart><c:scatterStyle val=\"lineMarker\"/><c:varyColors val=\"0\"/>";
        for (int col : t.chartCols) {
            string cn = column_name(col), idx = to_string(col - 2), color = kColors[(col - 2) % 24];
            x += "<c:ser><c:idx val=\"" + idx + "\"/><c:order val=\"" + idx + "\"/><c:tx><c:strRef><c:f>" + sn + "!$" + cn + "$1</c:f></c:strRef></c:tx>";
            x += "<c:spPr><a:ln w=\"19050\" cap=\"rnd\"><a:noFill/><a:round/></a:ln></c:spPr><c:marker><c:symbol val=\"circle\"/><c:size val=\"5\"/><c:spPr>" + clr(color) + ln9(color) + "</c:spPr></c:marker>";
            x += "<c:xVal><c:numRef><c:f>" + xRef + "</c:f></c:numRef></c:xVal><c:yVal><c:numRef><c:f>" + sn + "!$" + cn + "$2:$" + cn + "$" + last + "</c:f></c:numRef></c:yVal><c:smooth val=\"0\"/></c:ser>";
        }
        x += "<c:dLbls><c:showLegendKey val=\"0\"/><c:showVal val=\"0\"/><c:showCatName val=\"0\"/><c:showSerName val=\"0\"/><c:showPercent val=\"0\"/><c:showBubbleSize val=\"0\"/></c:dLbls>";
        x += "<c:axId val=\"1884610511\"/><c:axId val=\"1884610991\"/></c:scatterChart>";
        x += chart_axis("1884610511", "1884610991", "b", "m/d/yyyy\\ h:mm", "low", 900) + chart_axis("1884610991", "1884610511", "l", "General", "nextTo", 1200);
        x += "<c:spPr><a:noFill/><a:ln><a:noFill/></a:ln></c:spPr></c:plotArea><c:legend><c:legendPos val=\"b\"/><c:overlay val=\"0\"/>" + chart_text(900, "595959") + "</c:legend>";
        return x + "<c:plotVisOnly val=\"1\"/><c:dispBlanksAs val=\"gap\"/></c:chart><c:spPr>" + clr("FFFFFF") + ln9("D9D9D9") + "</c:spPr></c:chartSpace>";
    }

    string drawing_xml(int fromCol) {
        return kHdr + "<xdr:wsDr xmlns:xdr=\"" + kOx + "drawingml/2006/spreadsheetDrawing\" xmlns:a=\"" + kNsA + "\"><xdr:twoCellAnchor>" +
            "<xdr:from><xdr:col>" + to_string(fromCol) + "</xdr:col><xdr:colOff>3710</xdr:colOff><xdr:row>0</xdr:row><xdr:rowOff>191737</xdr:rowOff></xdr:from>" +
            "<xdr:to><xdr:col>" + to_string(fromCol + 15) + "</xdr:col><xdr:colOff>325193</xdr:colOff><xdr:row>34</xdr:row><xdr:rowOff>39077</xdr:rowOff></xdr:to>" +
            "<xdr:graphicFrame macro=\"\"><xdr:nvGraphicFramePr><xdr:cNvPr id=\"2\" name=\"Chart 1\"/><xdr:cNvGraphicFramePr/></xdr:nvGraphicFramePr>" +
            "<xdr:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/></xdr:xfrm><a:graphic><a:graphicData uri=\"" + kNsC + "\"><c:chart xmlns:c=\"" + kNsC +
            "\" xmlns:r=\"" + kNsR + "\" r:id=\"rId1\"/></a:graphicData></a:graphic></xdr:graphicFrame><xdr:clientData/></xdr:twoCellAnchor></xdr:wsDr>";
    }

    string rels(const vector<pair<string, string>>& items) {
        string x = kHdr + "<Relationships xmlns=\"" + kOx + "package/2006/relationships\">";
        for (size_t i = 0; i < items.size(); ++i)
            x += "<Relationship Id=\"rId" + to_string(i + 1) + "\" Type=\"" + kNsR + "/" + items[i].first + "\" Target=\"" + items[i].second + "\"/>";
        return x + "</Relationships>";
    }

    string override_part(const string& part, const string& type) {
        return "<Override PartName=\"" + part + "\" ContentType=\"application/vnd.openxmlformats-officedocument." + type + "+xml\"/>";
    }

    string xf(int fmt, bool align) {
        return "<xf numFmtId=\"" + to_string(fmt) + "\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"" + (fmt ? " applyNumberFormat=\"1\"" : "") +
            (align ? " applyAlignment=\"1\"><alignment vertical=\"center\"/></xf>" : "/>");
    }

    Bytes build_workbook(const Table& t, const string& sn, int valueStyle, const string& title) {
        bool ch = !t.chartCols.empty();
        vector<pair<string, string>> f;
        f.emplace_back("[Content_Types].xml", kHdr + "<Types xmlns=\"" + kOx + "package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>" +
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>" + override_part("/xl/workbook.xml", "spreadsheetml.sheet.main") +
            override_part("/xl/worksheets/sheet1.xml", "spreadsheetml.worksheet") + override_part("/xl/styles.xml", "spreadsheetml.styles") +
            (ch ? override_part("/xl/drawings/drawing1.xml", "drawing") + override_part("/xl/charts/chart1.xml", "drawingml.chart") : "") + "</Types>");
        f.emplace_back("_rels/.rels", rels({ {"officeDocument", "xl/workbook.xml"} }));
        f.emplace_back("xl/workbook.xml", kHdr + "<workbook xmlns=\"" + kNsMain + "\" xmlns:r=\"" + kNsR + "\"><sheets><sheet name=\"" + xml_escape(sn) + "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");
        f.emplace_back("xl/_rels/workbook.xml.rels", rels({ {"worksheet", "worksheets/sheet1.xml"}, {"styles", "styles.xml"} }));
        f.emplace_back("xl/styles.xml", kHdr + "<styleSheet xmlns=\"" + kNsMain + "\"><fonts count=\"1\"><font><sz val=\"11\"/><name val=\"" + U(u8"맑은 고딕") + "\"/><family val=\"2\"/></font></fonts>" +
            "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>" +
            "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>" +
            "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs><cellXfs count=\"4\">" +
            xf(0, false) + xf(0, true) + xf(22, true) + xf(11, true) + "</cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>");
        f.emplace_back("xl/worksheets/sheet1.xml", sheet_xml(t, valueStyle));
        if (ch) {
            f.emplace_back("xl/worksheets/_rels/sheet1.xml.rels", rels({ {"drawing", "../drawings/drawing1.xml"} }));
            f.emplace_back("xl/drawings/drawing1.xml", drawing_xml(int(t.header.size()) + 3));
            f.emplace_back("xl/drawings/_rels/drawing1.xml.rels", rels({ {"chart", "../charts/chart1.xml"} }));
            f.emplace_back("xl/charts/chart1.xml", chart_xml(t, sn, title));
        }
        return zip_store(f);
    }

    string scaled(double v, double factor) {
        double r = v * factor;
        if (r == 0.0) r = 0.0;
        char buf[48];
        snprintf(buf, sizeof(buf), "%.15g", r);
        return buf;
    }

    bool parse_factor(const string& s, double& v) {
        char* end = nullptr;
        v = strtod(s.c_str(), &end);
        return !s.empty() && end != s.c_str() && *end == '\0' && isfinite(v) && v != 0.0;
    }

}

int main(int argc, char** argv) {
    try {
        filesystem::path input = argc > 1 ? argv[1] : "RAW.xlsx";
        filesystem::path outDir = argc > 2 ? filesystem::path(argv[2]) : (input.has_parent_path() ? input.parent_path() : filesystem::path("."));
        string ftxt = argc > 3 ? argv[3] : "", title = argc > 4 ? argv[4] : kDefaultTitle;
        double factor = 0.0;
        for (bool fromArg = argc > 3; !parse_factor(ftxt, factor); fromArg = false) {
            if (fromArg && !ftxt.empty()) cerr << "invalid factor: " << ftxt << "\n";
            cout << "STEP2 factor: ";
            if (!getline(cin, ftxt)) throw runtime_error("factor input failed");
            ftxt.erase(remove_if(ftxt.begin(), ftxt.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r'; }), ftxt.end());
        }
        filesystem::create_directories(outDir);

        Bytes zip = read_file(input);
        auto idx = zip_index(zip);
        vector<string> shared;
        if (idx.count("xl/sharedStrings.xml")) shared = parse_shared_strings(zip_extract(zip, idx, "xl/sharedStrings.xml"));
        vector<Row> rows = parse_sheet(zip_extract(zip, idx, "xl/worksheets/sheet1.xml"), shared);

        string scanName = U(u8"스캔"), timeName = U(u8"시간"), alarm = U(u8"경보");
        auto hit = find_if(rows.begin(), rows.end(), [&](const Row& r) { return !r.empty() && r[0].col == 0 && r[0].isText && r[0].text == scanName; });
        if (hit == rows.end()) throw runtime_error("data header row not found");

        vector<int> keep;
        Table s1;
        int removed = 0;
        for (const Cell& c : *hit) {
            if (!c.isText || c.text.empty()) continue;
            if (c.text.compare(0, alarm.size(), alarm) == 0) { ++removed; continue; }
            if (c.text == scanName) s1.scanCol = int(keep.size());
            else if (c.text == timeName) s1.timeCol = int(keep.size());
            keep.push_back(c.col);
            s1.header.push_back(c.text);
        }
        if (s1.scanCol < 0 || s1.timeCol < 0) throw runtime_error("scan/time columns not found");

        for (auto it = hit + 1; it != rows.end(); ++it) {
            if (it->empty() || (*it)[0].col != 0 || (*it)[0].isText) continue;
            map<int, const Cell*> byCol;
            for (const Cell& c : *it) byCol[c.col] = &c;
            vector<string> o(keep.size());
            for (size_t k = 0; k < keep.size(); ++k) {
                auto f = byCol.find(keep[k]);
                if (f != byCol.end() && !f->second->isText) o[k] = f->second->text;
            }
            s1.rows.push_back(o);
        }
        if (s1.rows.empty()) throw runtime_error("no data rows found");

        Table s2 = s1;
        for (auto& r : s2.rows)
            for (size_t k = 0; k < r.size(); ++k)
                if (!r[k].empty() && int(k) != s1.scanCol && int(k) != s1.timeCol) r[k] = scaled(strtod(r[k].c_str(), nullptr), factor);
        for (int k = 0; k < int(s1.header.size()); ++k) if (k != s1.scanCol && k != s1.timeCol) s2.chartCols.push_back(k);

        write_file(outDir / "STEP1.xlsx", build_workbook(s1, "STEP1", 3, title));
        write_file(outDir / "STEP2.xlsx", build_workbook(s2, "STEP2", 1, title));

        string vc = to_string(s1.header.size() - 2);
        string readme = "\xEF\xBB\xBF" + U(u8"RAW 데이터 정리 결과\r\n\r\n") + U(u8"입력 파일: ") + input.filename().string() + "\r\n" +
            U(u8"출력 파일: STEP1.xlsx, STEP2.xlsx, readme.txt\r\n") + U(u8"스캔 수(데이터 행): ") + to_string(s1.rows.size()) + "\r\n\r\n" + U(u8"[STEP1]\r\n") +
            U(u8"RAW 상단의 계측 설정 정보와 경보 열 ") + to_string(removed) + U(u8"개를 제거하고 스캔, 시간, 채널 값 ") + vc + U(u8"개 열만 남김\r\n") +
            U(u8"수치는 RAW 원본 그대로\r\n\r\n") + U(u8"[STEP2]\r\n") + U(u8"STEP1의 전체 채널 수치(") + vc + U(u8"개 열, 스캔/시간 열 제외)에 ") + ftxt + U(u8"을(를) 곱함\r\n") +
            U(u8"그래프: 시간-채널 값 분산형 차트(") + to_string(s2.chartCols.size()) + U(u8"개 계열 전체), 제목 ") + title + "\r\n";
        write_file(outDir / "readme.txt", Bytes(readme.begin(), readme.end()));

        cout << "done: " << outDir.string() << " (rows=" << s1.rows.size() << ", removed alarm cols=" << removed << ")\n";
        return 0;
    }
    catch (const exception& e) {
        cerr << "error: " << e.what() << "\n";
        return 1;
    }
}