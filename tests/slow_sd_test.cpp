#include "profiler_core.h"
#include "profiler_reader.h"
#include <iostream>
#include <map>
#include <cmath>
#include <sys/resource.h>
#include <sys/syscall.h>

static const std::string kOutPathStr = ProfilerTestDir() + "profiler_output_durable.bin";
static const char* kOutPath = kOutPathStr.c_str();

// =====================================================================
// YAVAS DEPOLAMA TESTI
//  Soru: depolama yavaslayinca UYGULAMA thread'lerinin gorduğu gecikme degisiyor mu, ve veri
//        kaybi TAM OLARAK sayilan (g_dropped_total) kadar mi?
//  Yuk: P uretici, her biri: push_event(seq) -> kisa uyku (dongu). Her olayin payload'i thread
//       ici SIRA NUMARASI. push_event'in kendi suresi steady_clock ile olculur.
// =====================================================================
static int envi(const char* n, int d) { const char* v = std::getenv(n); return v ? std::atoi(v) : d; }

struct alignas(64) PerProducer {
    std::vector<uint32_t> lat;     // push_event suresi (ns), ILK cagri haric
    uint64_t first_ns = 0;         // ILK cagri: 64KB ring tahsisi + registry kilidi dahil
    uint64_t attempted = 0;
    uint64_t reg_ns = 0;           // ProfilerRegisterThread() suresi (sadece SLOW_PREREGISTER=1)
    uint64_t first16_max = 0;      // ilk cagri HARIC, sonraki 16 cagrinin en yavasi (soguk cache/TLB etkisi)
};

static uint32_t Pct(const std::vector<uint32_t>& v, double p) {
    if (v.empty()) return 0;
    size_t i = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size()))) ;
    if (i > 0) --i;
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

int main() {
    const int P = envi("SLOW_PRODUCERS", 4);
    const int dur_ms = envi("SLOW_DURATION_MS", 8000);
    const int sleep_us = envi("SLOW_SLEEP_US", 200);
    const int partial_ms = envi("PROFILER_PARTIAL_MS", 100);
    const int sync_ms = envi("PROFILER_SYNC_MS", 1000);
    const int flush_nice = envi("FLUSH_NICE", 0);
    const bool force_buffered = std::getenv("PROFILER_FORCE_BUFFERED") != nullptr;
    const bool prereg = envi("SLOW_PREREGISTER", 0) != 0;

    ::unlink(kOutPath);
    DirectWriter writer;
    if (!writer.Open(kOutPath, 4, force_buffered)) { perror("Open"); return 1; }

    std::atomic<bool> stop_flush{false};
    std::thread flusher([&] {
        if (flush_nice != 0) setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), flush_nice);
        flush_thread_task(stop_flush, writer, partial_ms, sync_ms);
    });

    std::vector<PerProducer> pp(P);
    for (auto& x : pp) x.lat.reserve(400000);
    std::atomic<int> finished{0};
    const auto t_start = std::chrono::steady_clock::now();
    const auto t_end = t_start + std::chrono::milliseconds(dur_ms);

    std::vector<std::thread> prod;
    for (int i = 0; i < P; ++i) {
        prod.emplace_back([&, i] {
            if (prereg) {
                auto r0 = std::chrono::steady_clock::now();
                ProfilerRegisterThread();
                auto r1 = std::chrono::steady_clock::now();
                pp[i].reg_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(r1 - r0).count());
            }
            bool first = true;
            uint32_t seq = 0;
            while (std::chrono::steady_clock::now() < t_end) {
                auto a = std::chrono::steady_clock::now();
                push_event(EVENT_LOCK_WAIT, seq);          // payload = thread ici sira numarasi
                auto b = std::chrono::steady_clock::now();
                uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
                if (first) { pp[i].first_ns = ns; first = false; }
                else {
                    pp[i].lat.push_back(static_cast<uint32_t>(std::min<uint64_t>(ns, 0xFFFFFFFFu)));
                    if (pp[i].lat.size() <= 16) pp[i].first16_max = std::max<uint64_t>(pp[i].first16_max, ns);
                }
                ++seq;
                pp[i].attempted++;
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
            }
            finished.fetch_add(1);
        });
    }

    // Ana thread: dusen event zaman cizelgesi (250 ms aralikla)
    std::vector<std::pair<int, uint64_t>> timeline;
    const int inproc_stall = envi("SLOW_INPROC_STALL_MS", 0);
    bool stall_set = false;
    while (finished.load() < P) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        int t = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_start).count());
        if (!stall_set && inproc_stall > 0 && t >= 500) { g_test_flush_stall_ms.store(inproc_stall); stall_set = true; }
        timeline.emplace_back(t, g_dropped_total.load(std::memory_order_relaxed));
    }
    for (auto& t : prod) t.join();

    // Kapanis: ring'ler bosalip registry bosalana kadar bekle (yavas cihazda bu SURE alabilir)
    const auto d0 = std::chrono::steady_clock::now();
    while (true) {
        { std::lock_guard<std::mutex> lk(g_registry_mutex); if (g_registry.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const int drain_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - d0).count());
    stop_flush.store(true, std::memory_order_release);
    flusher.join();
    const bool fin_ok = writer.Finalize();
    const auto& st = writer.stats();

    // ---- Uygulama tarafi gecikme dagilimi ----
    std::vector<uint32_t> all;
    uint64_t attempted = 0, first_max = 0, first_sum = 0;
    for (auto& x : pp) {
        all.insert(all.end(), x.lat.begin(), x.lat.end());
        attempted += x.attempted;
        first_max = std::max(first_max, x.first_ns);
        first_sum += x.first_ns;
    }
    std::sort(all.begin(), all.end());

    // ---- Dosyayi geri oku ----
    struct PT { uint64_t count = 0; int64_t last = -1; bool strictly_inc = true; bool contiguous = true; };
    std::map<uint16_t, PT> per;
    uint64_t durable = 0;
    DecodedFile df;
    if (!ParseProfilerFile(kOutPath, df)) { std::cerr << "dosya cozulemedi\n"; return 1; }
    for (const ProfilerEvent& ev : df.events) {           // yalniz VERI olaylari (baslik/tanim kayitlari haric)
        durable++;
        PT& p = per[ev.thread_id];
        int64_t sq = static_cast<int64_t>(ev.payload);
        if (sq <= p.last) p.strictly_inc = false;
        if (sq != p.last + 1) p.contiguous = false;
        p.last = sq;
        p.count++;
    }
    const uint64_t dropped = g_dropped_total.load();

    const double rate_per_thread = static_cast<double>(attempted) / P / (dur_ms / 1000.0);
    std::cout << "I/O=" << (writer.mode() == DirectWriter::Mode::Direct ? "O_DIRECT" : "BUFFERED")
              << " uretici=" << P << " sure=" << dur_ms << "ms flush_nice=" << flush_nice
              << " kismi_flush_ms=" << partial_ms << " sync_ms=" << sync_ms << "\n";
    std::cout << "  gercek hiz ~" << static_cast<int>(rate_per_thread) << " olay/sn/thread"
              << " -> " << RingBuffer::SIZE << "'lik ring ~" << static_cast<int>(static_cast<double>(RingBuffer::SIZE) / rate_per_thread * 1000) << " ms'de dolar\n"
              << "  RING_BOYUTU=" << RingBuffer::SIZE << "\n";
    std::cout << "  push_event gecikmesi (ns, ilk cagri haric, n=" << all.size() << "): "
              << "p50=" << Pct(all, 50) << " p99=" << Pct(all, 99) << " p99.9=" << Pct(all, 99.9)
              << " p99.99=" << Pct(all, 99.99) << " maks=" << (all.empty() ? 0 : all.back()) << "\n";
    std::cout << "  kayit_modu=" << (prereg ? "ACIK (ProfilerRegisterThread)" : "GEC (lazy)")
              << " lazy_kayit_sayisi=" << g_lazy_registrations.load() << "\n";
    std::cout << "  ILK push_event: ort=" << first_sum / P << " ns, maks=" << first_max << " ns\n";
    if (prereg) {
        uint64_t rs = 0, rm = 0;
        for (auto& x : pp) { rs += x.reg_ns; rm = std::max(rm, x.reg_ns); }
        std::cout << "  ProfilerRegisterThread() suresi (KURULUM evresinde odenen): ort=" << rs / P << " ns, maks=" << rm << " ns\n";
    }
    { uint64_t f16 = 0; for (auto& x : pp) f16 = std::max(f16, x.first16_max);
      std::cout << "  ilk cagridan SONRAKI 16 cagrinin en yavasi (tum thread'ler): " << f16 << " ns\n"; }
    std::cout << "RAW_FIRST:"; for (auto& x : pp) std::cout << " " << x.first_ns; std::cout << "\n";
    std::cout << "RAW_REG:";   for (auto& x : pp) std::cout << " " << x.reg_ns;   std::cout << "\n";
    std::cout << "RAW_F16:";   for (auto& x : pp) std::cout << " " << x.first16_max; std::cout << "\n";
    std::cout << "  flush thread: en_uzun_write=" << st.max_write_ns / 1000000.0 << "ms en_uzun_sync="
              << st.max_sync_ns / 1000000.0 << "ms toplam_write=" << st.total_write_ns / 1000000.0
              << "ms write_calls=" << st.write_calls << " kapanis_bosaltma=" << drain_ms << "ms\n";
    std::cout << "  HESAP: uretilen=" << attempted << " kalici=" << durable << " dusen=" << dropped
              << " -> uretilen==kalici+dusen: " << (attempted == durable + dropped ? "EVET" : "HAYIR!") << "\n";
    std::cout << "  dusen zaman cizelgesi (ms:dusen): ";
    uint64_t prev = ~0ull;
    for (auto& tp : timeline) { if (tp.second != prev) std::cout << tp.first << ":" << tp.second << " "; prev = tp.second; }
    std::cout << "\n";
    std::cout.flush();

    bool ok = fin_ok && st.io_errors == 0 && attempted == durable + dropped;
    ok = ok && (g_lazy_registrations.load() == (prereg ? 0u : static_cast<uint64_t>(P)));   // acik kayitta HIC lazy olmamali, degilse tam P
    for (auto& [tid, p] : per) {
        ok = ok && p.strictly_inc;                        // thread ici sira asla bozulmaz, tekrar yok
        if (dropped == 0) ok = ok && p.contiguous;        // dusme yoksa: 0..n-1 DELIKSIZ
        (void)tid;
    }
    std::cout << (ok ? "  INVARIANT'LAR: TAMAM" : "  INVARIANT'LAR: HATA!") << "\n";
    return ok ? 0 : 2;
}
