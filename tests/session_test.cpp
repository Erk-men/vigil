// session_test.cpp -- ProfilerSession (Start/Stop) ve flush thread'in SON TAHLIYE turu.
// Kritik: Stop() cagrildiginda hala YASAYAN thread'lerin (ana thread) ring'lerinde bekleyen olaylar da dosyaya girmeli;
// muhasebe: uretilen == dosyadaki veri olayi + dusen. Ayrica: idempotent Stop, ikinci Start reddi, yikici-tabanli durdurma,
// hem O_DIRECT hem zorlanmis buffered mod.
// Derleme: g++ -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude tests/session_test.cpp
#include "profiler_core.h"
#include "profiler_reader.h"
#include <iostream>
#include <cassert>

static uint64_t CountByName(const DecodedFile& d, uint16_t type, const std::string& name) {
    uint64_t n = 0;
    for (auto& e : d.events) if (EventTypeOf(e) == type && d.SiteName(EventSiteOf(e)) == name) n++;
    return n;
}

// Bir oturum kosar; dosyayi cozup dogrular. explicit_stop=false: yikici durdurur.
static void RunCase(bool force_buffered, bool explicit_stop) {
    const std::string path = ProfilerTestDir() + "session_test.bin";
    std::remove(path.c_str());
    const uint64_t drop0 = g_dropped_total.load();
    uint64_t attempted = 0;
    {
        ProfilerSession s;
        ProfilerOptions o; o.path = path; o.force_buffered = force_buffered;
        assert(s.Start(o));
        assert(!s.Start(o));                                        // ikinci Start reddedilir

        std::mutex m;
        constexpr int kWorkers = 4, kIters = 700, kMain = 3000;
        std::vector<std::thread> ws;
        for (int w = 0; w < kWorkers; ++w)
            ws.emplace_back([&] {
                ProfilerRegisterThread();
                for (int i = 0; i < kIters; ++i) {
                    { PROFILE_LOCK_SCOPE_N(m, lk, "worker_lock"); }
                    { PROFILE_DEADLINE_N(1, "worker_deadline"); busy_wait_us(50); }   // 50us > 1us butce: HER SEFERINDE asim
                }
            });
        // ANA thread de olay uretir ve Stop'a kadar YASAR (ring'i registry'de kalir)
        ProfilerRegisterThread();
        for (int i = 0; i < kMain; ++i) { PROFILE_LOCK_SCOPE_N(m, lk, "main_lock"); }
        for (auto& t : ws) t.join();                                // isciler Stop'tan ONCE cikar
        attempted = static_cast<uint64_t>(kMain) + 2ull * kWorkers * kIters;
        if (explicit_stop) {
            assert(s.Stop());
            assert(s.Stop());                                       // idempotent
            assert(!s.running());
            assert(s.writer().stats().io_errors == 0);
            assert(s.writer().mode() == (force_buffered ? DirectWriter::Mode::Buffered : s.writer().mode()));
        }
    }   // explicit_stop=false ise yikici Stop eder

    DecodedFile d;
    assert(ParseProfilerFile(path, d));
    const uint64_t dropped = g_dropped_total.load() - drop0;
    assert(d.header_seen && d.version == kProfilerFormatVersion);
    assert(d.orphan_events == 0 && d.duplicate_defs == 0 && d.unknown_records == 0 && d.warnings.empty());
    assert(d.padding_only_at_tail && d.size_multiple_of_16);
    assert(d.events.size() + dropped == attempted);                 // MUHASEBE: yasayan ana thread'in olaylari DAHIL, hicbiri sessizce kaybolmaz
    assert(dropped == 0);                                           // 3000/1400 olay << 4096: ring tasmamali
    assert(CountByName(d, EVENT_LOCK_WAIT, "main_lock") == 3000);   // yasayan thread: Stop'taki SON TAHLIYE ile geldi
    assert(CountByName(d, EVENT_LOCK_WAIT, "worker_lock") == 2800);
    assert(CountByName(d, EVENT_DEADLINE_MISS, "worker_deadline") == 2800);
    assert(d.sites.size() == 3);
    std::cout << "  " << (force_buffered ? "buffered" : "O_DIRECT") << (explicit_stop ? " + acik Stop()" : " + yikici Stop()")
              << ": uretilen=" << attempted << " dosyada=" << d.events.size() << " dusen=" << dropped
              << " site=" << d.sites.size() << " meta=" << d.meta_records << ": OK\n";
    std::remove(path.c_str());
}

int main() {
    // NOT: registry/site tablosu surec-genelidir; her RunCase ayni sitelere (ayni adlar) yeniden basvurur (tekillestirme).
    // Ana thread'in ring'i RunCase'ler arasinda YASAR; bu yuzden her senaryoda ana thread'in yeni olaylari eklenir.
    std::cout << "ProfilerSession:\n";
    RunCase(false, true);
    RunCase(true, true);
    RunCase(false, false);
    RunCase(true, false);
    std::cout << "TUM OTURUM TESTLERI BASARILI\n";
    return 0;
}
