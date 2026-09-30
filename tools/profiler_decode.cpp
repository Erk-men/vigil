// profiler_decode.cpp -- "hangi kilit yavas? hangi bolge butceyi asti?" sorusunu yanitlar.
//   profiler_decode dosya.bin                    : site bazinda ozet tablo (terminal)
//   profiler_decode dosya.bin --csv              : tum veri olaylari (timestamp_ns,thread,tur,site,isim,payload_ns)
//   profiler_decode dosya.bin --html cikti.html  : tek dosya, cevrimdisi HTML rapor (CDN/font/script yok)
// Derleme: g++ -std=c++17 -O2 -I../include profiler_decode.cpp -o profiler_decode
#include "profiler_reader.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <ctime>

static double Pct(std::vector<uint32_t>& v, double p) {          // en-yakin-sira (nearest-rank); v siralanir
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size())));
    if (i > 0) --i;
    return v[std::min(i, v.size() - 1)];
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "kullanim: profiler_decode dosya.bin [--csv | --html cikti.html]\n"; return 1; }
    DecodedFile d;
    if (!ParseProfilerFile(argv[1], d)) { std::cerr << "dosya acilamadi: " << argv[1] << "\n"; return 1; }
    const bool csv = argc >= 3 && std::strcmp(argv[2], "--csv") == 0;
    const bool html = argc >= 4 && std::strcmp(argv[2], "--html") == 0;
    if (html) {
        std::ofstream f(argv[3], std::ios::binary);
        if (!f) { std::cerr << "yazilamadi: " << argv[3] << "\n"; return 1; }
        std::time_t t = std::time(nullptr);
        char buf[64]; std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
        WriteHtmlReport(d, argv[1], buf, f);
        f.close();
        std::cout << "yazildi: " << argv[3] << " (" << d.events.size() << " veri olayi, "
                  << d.sites.size() << " site, yetim=" << d.orphan_events << ")\n";
        return d.orphan_events == 0 && d.size_multiple_of_16 ? 0 : 2;
    }
    if (csv) {
        std::cout << "timestamp_ns,thread,tur,site,isim,payload_ns\n";
        for (auto& e : d.events)
            std::cout << e.timestamp << "," << e.thread_id << "," << (EventTypeOf(e) == EVENT_LOCK_WAIT ? "lock_wait" : "deadline_miss")
                      << "," << EventSiteOf(e) << ",\"" << d.SiteName(EventSiteOf(e)) << "\"," << e.payload << "\n";
        return 0;
    }
    std::cout << "Dosya: " << argv[1] << "\n"
              << "  bicim surumu=" << (d.header_seen ? std::to_string(d.version) : "BASLIK YOK") << "  kayit=" << d.total_records
              << " (veri=" << d.events.size() << " meta=" << d.meta_records << " dolgu=" << d.padding_records << ")"
              << "  tanimli site=" << d.sites.size() << "  YETIM olay=" << d.orphan_events << "\n";
    for (auto& w : d.warnings) std::cout << "  UYARI: " << w << "\n";

    struct Agg { std::vector<uint32_t> v; uint64_t total = 0; uint32_t mx = 0; };
    std::map<std::pair<uint16_t, uint16_t>, Agg> agg;           // (tur, site)
    for (auto& e : d.events) { Agg& a = agg[{EventTypeOf(e), EventSiteOf(e)}]; a.v.push_back(e.payload); a.total += e.payload; a.mx = std::max(a.mx, e.payload); }

    for (uint16_t type : {static_cast<uint16_t>(EVENT_LOCK_WAIT), static_cast<uint16_t>(EVENT_DEADLINE_MISS)}) {
        std::vector<std::pair<uint16_t, Agg*>> rows;
        for (auto& [k, a] : agg) if (k.first == type) rows.emplace_back(k.second, &a);
        std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.second->total > y.second->total; });
        std::cout << "\n== " << (type == EVENT_LOCK_WAIT ? "KILIT BEKLEMELERI (toplam beklemeye gore)" : "DEADLINE ASIMLARI (toplam asima gore)") << " ==\n";
        if (rows.empty()) { std::cout << "  (yok)\n"; continue; }
        std::cout << std::left << std::setw(34) << "  site" << std::right << std::setw(9) << "adet" << std::setw(13) << "toplam(ms)"
                  << std::setw(10) << "ort(us)" << std::setw(10) << "p50(us)" << std::setw(10) << "p99(us)" << std::setw(11) << "maks(us)" << "\n";
        for (auto& [site, a] : rows) {
            std::cout << std::left << std::setw(34) << ("  " + d.SiteName(site)) << std::right << std::setw(9) << a->v.size()
                      << std::fixed << std::setprecision(3) << std::setw(13) << a->total / 1e6
                      << std::setprecision(2) << std::setw(10) << static_cast<double>(a->total) / static_cast<double>(a->v.size()) / 1e3
                      << std::setw(10) << Pct(a->v, 50) / 1e3 << std::setw(10) << Pct(a->v, 99) / 1e3 << std::setw(11) << a->mx / 1e3 << "\n";
        }
    }
    return d.orphan_events == 0 && d.size_multiple_of_16 ? 0 : 2;
}
