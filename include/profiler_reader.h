// profiler_reader.h -- .bin dosyasini cozer: baslik, site tanimlari, veri olaylari. Testler ve profiler_decode ayni kodu kullanir.
#pragma once
#include "profiler_io.h"
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>

struct DecodedSite {
    std::string name;
    uint8_t kind = 0;
    uint32_t declared_len = 0;
    uint32_t received = 0;
    bool complete = false;
};

struct DecodedFile {
    bool header_seen = false;
    uint32_t version = 0;
    std::map<uint16_t, DecodedSite> sites;
    std::vector<ProfilerEvent> events;          // yalniz VERI olaylari (dosya sirasiyla)
    uint64_t total_records = 0, padding_records = 0, meta_records = 0, unknown_records = 0;
    // Veri olayi, DOSYADA KENDISINDEN ONCE tamamlanmis tanimi olmayan (site != 0) bir siteye basvuruyorsa YETIM sayilir.
    uint64_t orphan_events = 0;
    uint64_t duplicate_defs = 0;
    bool size_multiple_of_16 = true;
    bool padding_only_at_tail = true;           // dolgu kaydindan SONRA dolgu-olmayan kayit geldiyse false
    std::vector<std::string> warnings;

    std::string SiteName(uint16_t id) const {
        if (id == 0) return "<kimliksiz>";
        auto it = sites.find(id);
        if (it == sites.end() || !it->second.complete) return "<tanimsiz site " + std::to_string(id) + ">";
        return it->second.name;
    }
};

// ---------------------------------------------------------------------
// HTML rapor: TAMAMEN CEVRIMDISI, tek dosya. CDN yok, harici font/script yok --
// hedef kitle (gomulu/savunma) genelde internete kapali gelistirme makineleri kullanir.
// ---------------------------------------------------------------------
struct SiteAgg { std::vector<uint32_t> v; uint64_t total = 0; uint32_t mx = 0; };

inline double PercentileSorted(std::vector<uint32_t>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size())));
    if (i > 0) --i;
    return v[std::min(i, v.size() - 1)];
}

inline std::string HtmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

inline void WriteHtmlSection(std::ostream& out, const DecodedFile& d, uint16_t type,
                              const char* title, const char* unit_label) {
    std::map<uint16_t, SiteAgg> agg;
    for (auto& e : d.events) {
        if (EventTypeOf(e) != type) continue;
        SiteAgg& a = agg[EventSiteOf(e)];
        a.v.push_back(e.payload);
        a.total += e.payload;
        a.mx = std::max(a.mx, e.payload);
    }
    out << "<section><h2>" << title << "</h2>\n";
    if (agg.empty()) { out << "<p class=\"empty\">(bu turde olay yok)</p></section>\n"; return; }

    std::vector<std::pair<uint16_t, SiteAgg*>> rows;
    for (auto& [site, a] : agg) rows.emplace_back(site, &a);
    std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.second->total > y.second->total; });
    const uint64_t max_total = rows.front().second->total;

    out << "<table><thead><tr><th>Site</th><th>Adet</th><th>Toplam (ms)</th><th>Ort (" << unit_label
        << ")</th><th>P50</th><th>P99</th><th>Maks</th><th class=\"barcol\"></th></tr></thead><tbody>\n";
    for (auto& [site, a] : rows) {
        std::vector<uint32_t> v = a->v;   // PercentileSorted siralar, kopya uzerinde calis
        double p50 = PercentileSorted(v, 50), p99 = PercentileSorted(v, 99);
        double pct = max_total ? 100.0 * static_cast<double>(a->total) / static_cast<double>(max_total) : 0.0;
        double avg_us = static_cast<double>(a->total) / static_cast<double>(a->v.size()) / 1000.0;
        out << "<tr><td class=\"site\">" << HtmlEscape(d.SiteName(site)) << "</td>"
            << "<td class=\"num\">" << a->v.size() << "</td>"
            << "<td class=\"num\">" << std::fixed << std::setprecision(2) << a->total / 1e6 << "</td>"
            << "<td class=\"num\">" << std::setprecision(2) << avg_us << "</td>"
            << "<td class=\"num\">" << std::setprecision(2) << p50 / 1000.0 << "</td>"
            << "<td class=\"num\">" << std::setprecision(2) << p99 / 1000.0 << "</td>"
            << "<td class=\"num\">" << std::setprecision(2) << a->mx / 1000.0 << "</td>"
            << "<td class=\"barcol\"><div class=\"bar\" style=\"width:" << std::setprecision(1) << pct << "%\"></div></td>"
            << "</tr>\n";
    }
    out << "</tbody></table></section>\n";
}

// generated_at_label: cagiranin verdigi bir zaman etiketi (bu kutuphane saat/tarih API'si kullanmaz).
inline bool WriteHtmlReport(const DecodedFile& d, const std::string& source_name,
                             const std::string& generated_at_label, std::ostream& out) {
    out << "<!doctype html><html lang=\"tr\"><head><meta charset=\"utf-8\">"
           "<title>Vigil raporu: " << HtmlEscape(source_name) << "</title><style>\n"
           ":root{--bg:#0f1115;--panel:#161922;--text:#e6e8ec;--muted:#8b93a3;--accent:#5fb3ff;--line:#262b38;}\n"
           "@media (prefers-color-scheme: light){:root{--bg:#f7f8fa;--panel:#ffffff;--text:#12151c;--muted:#5b6472;--accent:#0b6fce;--line:#e3e6ec;}}\n"
           "*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--text);"
           "font:14px/1.5 -apple-system,Segoe UI,Helvetica,Arial,sans-serif;padding:32px 16px}\n"
           ".wrap{max-width:980px;margin:0 auto}\n"
           "h1{font-size:20px;margin:0 0 4px} .meta{color:var(--muted);font-size:12.5px;margin-bottom:28px}\n"
           ".meta b{color:var(--text)}\n"
           "section{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:18px 20px;margin-bottom:20px}\n"
           "h2{font-size:15px;margin:0 0 14px}\n"
           "table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}\n"
           // NOT: text-transform:uppercase KASITLI OLARAK kullanilmiyor -- 'µs' gibi Unicode karakterleri
           // buyuk harfe cevirince mu (µ) harfi Yunan Mu'suna (Μ) donusuyor, bu da 'M' ile gorsel olarak
           // ayirt edilemiyor ('µs' -> 'Ms' gibi goruniyor, birim yanlis anlasilir). Basliklar kaynakta
           // zaten dogru harfle yazildigi icin CSS donusumune gerek yok.\n"
           "th{text-align:left;color:var(--muted);font-weight:600;font-size:11.5px;"
           "letter-spacing:.02em;padding:0 8px 8px;border-bottom:1px solid var(--line)}\n"
           "td{padding:7px 8px;border-bottom:1px solid var(--line);font-size:13px}\n"
           "td.site{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12.5px}\n"
           "td.num{text-align:right;font-family:ui-monospace,Menlo,Consolas,monospace;white-space:nowrap}\n"
           "th.barcol,td.barcol{width:120px} .bar{height:8px;background:var(--accent);border-radius:3px;min-width:2px}\n"
           ".empty{color:var(--muted)} .warn{color:#e0a030}\n"
           ".badge{display:inline-block;padding:2px 8px;border-radius:99px;font-size:11.5px;margin-right:6px;"
           "border:1px solid var(--line);color:var(--muted)}\n"
           ".badge.bad{color:#e0524d;border-color:#e0524d55}\n"
           "footer{color:var(--muted);font-size:11.5px;text-align:center;margin-top:24px}\n"
           "</style></head><body><div class=\"wrap\">\n";

    out << "<h1>Vigil raporu</h1>\n<div class=\"meta\"><b>" << HtmlEscape(source_name) << "</b> &middot; "
        << HtmlEscape(generated_at_label) << " &middot; biçim sürümü "
        << (d.header_seen ? std::to_string(d.version) : std::string("YOK"))
        << "<br>" << d.total_records << " kayıt (" << d.events.size() << " veri, " << d.meta_records
        << " meta, " << d.padding_records << " dolgu) &middot; " << d.sites.size() << " tanımlı site";
    if (d.orphan_events || d.duplicate_defs || d.unknown_records || !d.warnings.empty() || !d.size_multiple_of_16)
        out << " &middot; <span class=\"badge bad\">" << d.orphan_events << " yetim olay</span>";
    else
        out << " &middot; <span class=\"badge\">bütünlük tamam</span>";
    out << "</div>\n";

    WriteHtmlSection(out, d, EVENT_LOCK_WAIT, "Kilit beklemeleri", "\u00b5s");
    WriteHtmlSection(out, d, EVENT_DEADLINE_MISS, "Deadline aşımları", "\u00b5s");

    if (!d.warnings.empty()) {
        out << "<section><h2 class=\"warn\">Uyarılar</h2><ul>";
        for (auto& w : d.warnings) out << "<li>" << HtmlEscape(w) << "</li>";
        out << "</ul></section>\n";
    }
    out << "<footer>vigil profiler_decode --html &middot; çevrimdışı üretildi, harici bağımlılık yok</footer>\n"
           "</div></body></html>\n";
    return true;
}

inline bool ParseProfilerFile(const std::string& path, DecodedFile& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    out = DecodedFile{};
    if (bytes.size() % sizeof(ProfilerEvent) != 0) {
        out.size_multiple_of_16 = false;
        out.warnings.push_back("dosya boyutu 16'nin kati degil (" + std::to_string(bytes.size()) + " bayt)");
    }
    const size_t n = bytes.size() / sizeof(ProfilerEvent);
    for (size_t i = 0; i < n; ++i) {
        ProfilerEvent e;
        std::memcpy(&e, bytes.data() + i * sizeof(ProfilerEvent), sizeof(e));
        out.total_records++;
        const uint16_t type = EventTypeOf(e), site = EventSiteOf(e);
        if (type != EVENT_PADDING && out.padding_records > 0) out.padding_only_at_tail = false;
        switch (type) {
            case EVENT_PADDING: out.padding_records++; break;
            case EVENT_FILE_HEADER: out.meta_records++; out.header_seen = true; out.version = e.payload; break;
            case EVENT_SITE_DEF: {
                out.meta_records++;
                if (out.sites.count(site)) { out.duplicate_defs++; break; }
                DecodedSite d; d.kind = static_cast<uint8_t>(e.thread_id); d.declared_len = e.payload;
                d.name.assign(d.declared_len, '\0'); d.complete = (d.declared_len == 0);
                out.sites.emplace(site, std::move(d));
                break;
            }
            case EVENT_SITE_CHUNK: {
                out.meta_records++;
                auto it = out.sites.find(site);
                if (it == out.sites.end()) { out.warnings.push_back("tanimsiz site icin isim parcasi: " + std::to_string(site)); break; }
                DecodedSite& d = it->second;
                const size_t off = static_cast<size_t>(e.thread_id) * 12;
                if (off >= d.declared_len) break;
                char buf[12];
                std::memcpy(buf, &e.timestamp, 8);
                std::memcpy(buf + 8, &e.payload, 4);
                const size_t take = std::min<size_t>(12, d.declared_len - off);
                std::memcpy(&d.name[off], buf, take);
                d.received += static_cast<uint32_t>(take);
                if (d.received >= d.declared_len) d.complete = true;
                break;
            }
            case EVENT_LOCK_WAIT:
            case EVENT_DEADLINE_MISS: {
                if (site != 0) {
                    auto it = out.sites.find(site);
                    if (it == out.sites.end() || !it->second.complete) out.orphan_events++;
                }
                out.events.push_back(e);
                break;
            }
            default: out.unknown_records++; break;
        }
    }
    if (out.unknown_records) out.warnings.push_back(std::to_string(out.unknown_records) + " bilinmeyen tur kaydi");
    return true;
}
