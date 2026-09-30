// lock_scope_bench.cpp -- PROFILE_LOCK_SCOPE'un "besin degeri tablosu".
//
// Yontem:
//  * SERPISTIRILMIS A/B: her turda (round) tum varyantlar bir kez, donen sirayla (Latin kare) calisir.
//    Zamana bagli kaymayi (bu makinede ~%25 gozlendi) ve sira etkisini dengeler.
//  * Her olcum bir BATCH: warm-up (olculmez) + K islem. K + warm < ring kapasitesi (4096), boylece olculen
//    yol NORMAL push yoludur; batch'ler arasi bekleme flush thread'in ring'i bosaltmasini saglar.
//  * Her batch icin getrusage(RUSAGE_THREAD): batch sirasinda thread bolunduyse (nivcsw>0) "temiz degil".
//  * malloc/calloc/realloc cagrilari sayilir (sicak yolda tahsis var mi?).
//  * Dolu-ring (dusurme) yolu ayri faz: flush thread durdurulur, ring doldurulur.
//  * Cekismeli mod (BENCH_THREADS>1): GERCEK cekisme icin >=2 cekirdek gerekir. Gonullu baglam degistirme
//    sayisi (nvcsw) cekismenin gercekten olusup olusmadigini gosterir.
//
// Derleme: g++ -std=c++17 -O2 -pthread -I. lock_scope_bench.cpp -o lock_scope_bench   (SANITIZER ILE KULLANMAYIN)
#include "profiler_core.h"
#include <sys/resource.h>
#include <time.h>
#include <condition_variable>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstdlib>

// ---- malloc sayaci (yalniz bu ikili; glibc'nin __libc_* girisleri uzerinden) ----
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
}
static std::atomic<uint64_t> g_alloc_calls{0};
#ifndef BENCH_NO_MALLOC_COUNT
extern "C" void* malloc(size_t n) noexcept { g_alloc_calls.fetch_add(1, std::memory_order_relaxed); return __libc_malloc(n); }
extern "C" void* calloc(size_t a, size_t b) noexcept { g_alloc_calls.fetch_add(1, std::memory_order_relaxed); return __libc_calloc(a, b); }
extern "C" void* realloc(void* p, size_t n) noexcept { g_alloc_calls.fetch_add(1, std::memory_order_relaxed); return __libc_realloc(p, n); }
#endif

static const char* kBenchOut = std::getenv("BENCH_OUT") ? std::getenv("BENCH_OUT") : "./lock_scope_bench.out.bin";   // kendi makinende degistirebilirsin
static int envi(const char* n, int d) { const char* v = std::getenv(n); return v ? std::atoi(v) : d; }

// ---------------- Varyantlar ----------------
static std::mutex g_bm;                 // tek thread fazi mutex'i
static std::mutex g_cm;                 // cekismeli faz mutex'i
static volatile uint64_t g_sink;

static void v_bare(size_t n)  { for (size_t i = 0; i < n; ++i) { std::unique_lock<std::mutex> lk(g_bm); } }
static void v_macro(size_t n) { for (size_t i = 0; i < n; ++i) { PROFILE_LOCK_SCOPE(g_bm, lk); } }
static void v_push(size_t n)  { for (size_t i = 0; i < n; ++i) push_event(EVENT_LOCK_WAIT, 0); }
static void v_clock(size_t n) {
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) acc += static_cast<uint64_t>(ProfilerClock::now().time_since_epoch().count());
    g_sink = acc;
}
static void c_bare(size_t n)  { for (size_t i = 0; i < n; ++i) { std::unique_lock<std::mutex> lk(g_cm); } }
static void c_macro(size_t n) { for (size_t i = 0; i < n; ++i) { PROFILE_LOCK_SCOPE(g_cm, lk); } }


// ---- OLCUM ICIN YEREL KOPYA: cekirdekteki push_event ile AYNI govde, tek fark zaman damgasi disaridan gelir.
// (Benimsenirse cekirdege sablon olarak tasinir; simdilik olcum icin burada, cekirdek degismedi.)
static inline uint64_t ts_ns(ProfilerClock::time_point t) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
}
static inline void bench_push_at(uint64_t ts, uint16_t type, uint32_t payload) {
    RingBuffer* rb = GetThreadLocalBuffer();
    uint64_t h = rb->head.load(std::memory_order_relaxed);
    uint64_t t = rb->tail.load(std::memory_order_acquire);
    if (h - t >= RingBuffer::SIZE) { g_dropped_total.fetch_add(1, std::memory_order_relaxed); return; }
    ProfilerEvent& slot = rb->events[h & (RingBuffer::SIZE - 1)];
    slot.timestamp = ts; slot.thread_id = rb->thread_id; slot.event_type = type; slot.payload = payload;
    rb->head.store(h + 1, std::memory_order_release);
    if (g_test_pushed) g_test_pushed->fetch_add(1, std::memory_order_relaxed);
}
// V2a: makronun zaten okudugu t1, olayin zaman damgasi olur (3 yerine 2 saat okumasi)
static void v_macro_reuse(size_t n) {
    for (size_t i = 0; i < n; ++i) {
        auto t0 = ProfilerClock::now();
        std::unique_lock<std::mutex> lk(g_bm);
        auto t1 = ProfilerClock::now();
        bench_push_at(ts_ns(t1), EVENT_LOCK_WAIT, clamp_u32(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
}
// V2b: once try_lock; basarili ise HIC olay/saat okumasi yok. Yalniz gercekten BEKLENEN kilitler kaydedilir.
static void v_macro_try(size_t n) {
    for (size_t i = 0; i < n; ++i) {
        std::unique_lock<std::mutex> lk(g_bm, std::try_to_lock);
        if (!lk.owns_lock()) {
            auto t0 = ProfilerClock::now();
            lk.lock();
            auto t1 = ProfilerClock::now();
            bench_push_at(ts_ns(t1), EVENT_LOCK_WAIT, clamp_u32(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
    }
}


// ---- ESKI makro (V2a ONCESI, basliktaki halinin birebir kopyasi): push_event kendi saat okumasini yapar => toplam 3 okuma
#define PROFILE_LOCK_SCOPE_OLD(mtx, lock_name) \
    auto _t0_##lock_name = ProfilerClock::now(); \
    std::unique_lock<std::decay_t<decltype(mtx)>> lock_name(mtx); \
    push_event(EVENT_LOCK_WAIT, clamp_u32(std::chrono::duration_cast<std::chrono::nanoseconds>( \
        ProfilerClock::now() - _t0_##lock_name).count()))
static void v_macro_old(size_t n) { for (size_t i = 0; i < n; ++i) { PROFILE_LOCK_SCOPE_OLD(g_bm, lk); } }


// ---- FAZ 0 mikro varyantlari: YALNIZ site-kimligi kontrolunun maliyeti (olay uretmez) ----
#define BENCH_BARRIER() asm volatile("" ::: "memory")           // derleyici dongu disina tasimasin / birlestirmesin
static void m_none(size_t n)   { uint64_t a = 0; for (size_t i = 0; i < n; ++i) { BENCH_BARRIER(); a += 1; } g_sink = a; }
static void m_atomic(size_t n) { uint64_t a = 0; for (size_t i = 0; i < n; ++i) { BENCH_BARRIER(); a += PROFILER_SITE_ID("bench_mikro_atomic", SITE_LOCK, false); } g_sink = a; }
static void m_magic(size_t n) {
    uint64_t a = 0;
    for (size_t i = 0; i < n; ++i) {
        BENCH_BARRIER();
        a += ([]() -> uint16_t { static const uint16_t v = RegisterSite("bench_mikro_magic", SITE_LOCK, false); return v; })();   // C++11 'magic static' (guard)
    }
    g_sink = a;
}

struct Variant { const char* name; void (*fn)(size_t); bool pushes; };

struct Sample { double ns = 0; uint64_t nvcsw = 0, nivcsw = 0, allocs = 0; bool clean() const { return nvcsw == 0 && nivcsw == 0; } };

static Sample TimedBatch(void (*fn)(size_t), size_t warm, size_t K) {
    fn(warm);                                            // olculmeyen isinma
    struct rusage a, b;
    getrusage(RUSAGE_THREAD, &a);
    const uint64_t al0 = g_alloc_calls.load(std::memory_order_relaxed);
    const auto t0 = ProfilerClock::now();
    fn(K);
    const auto t1 = ProfilerClock::now();
    const uint64_t al1 = g_alloc_calls.load(std::memory_order_relaxed);
    getrusage(RUSAGE_THREAD, &b);
    Sample s;
    s.ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(K);
    s.nvcsw = static_cast<uint64_t>(b.ru_nvcsw - a.ru_nvcsw);
    s.nivcsw = static_cast<uint64_t>(b.ru_nivcsw - a.ru_nivcsw);
    s.allocs = al1 - al0;
    return s;
}

static uint64_t g_quiesce_timeouts = 0;
static void Quiesce(const std::vector<RingBuffer*>& rings, int spacing_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(spacing_ms));
    for (RingBuffer* rb : rings) {
        for (int i = 0; i < 2000; ++i) {
            if (rb->tail.load(std::memory_order_acquire) == rb->head.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (i == 1999) g_quiesce_timeouts++;
        }
    }
}

static double Pctl(std::vector<double> v, double p) {
    if (v.empty()) return std::nan("");
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size())));
    if (i > 0) --i;
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

static std::vector<double> Clean(const std::vector<Sample>& s) { std::vector<double> o; for (auto& x : s) if (x.clean()) o.push_back(x.ns); return o; }
static std::vector<double> All(const std::vector<Sample>& s)   { std::vector<double> o; for (auto& x : s) o.push_back(x.ns); return o; }
static std::vector<double> PairedDiff(const std::vector<Sample>& a, const std::vector<Sample>& b, bool clean_only) {
    std::vector<double> d;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (!clean_only || (a[i].clean() && b[i].clean())) d.push_back(a[i].ns - b[i].ns);
    return d;
}
static void PrintRow(const char* name, const std::vector<Sample>& s) {
    auto c = Clean(s); auto a = All(s);
    uint64_t allocs = 0, nv = 0; for (auto& x : s) { allocs += x.allocs; nv += x.nvcsw; }
    std::cout << "  " << std::left << std::setw(26) << name << std::right << std::fixed << std::setprecision(1)
              << " temiz=" << std::setw(3) << c.size() << "/" << std::setw(3) << s.size()
              << " | medyan=" << std::setw(7) << Pctl(c, 50) << " p10=" << std::setw(7) << Pctl(c, 10)
              << " p90=" << std::setw(7) << Pctl(c, 90) << " ns/islem | tum_batch_medyan=" << std::setw(7) << Pctl(a, 50)
              << " | malloc=" << allocs << " gonullu_bd=" << nv << "\n";
}
static void PrintDiff(const char* label, const std::vector<double>& d) {
    std::cout << "  " << std::left << std::setw(34) << label << std::right << std::fixed << std::setprecision(1)
              << " n=" << std::setw(3) << d.size() << " medyan=" << std::setw(7) << Pctl(d, 50)
              << " p10=" << std::setw(7) << Pctl(d, 10) << " p90=" << std::setw(7) << Pctl(d, 90) << " ns\n";
}

// ---------------- Cekismeli faz altyapisi ----------------

struct Barrier {
    std::mutex m; std::condition_variable cv; size_t n, count = 0, gen = 0;
    explicit Barrier(size_t n_) : n(n_) {}
    void wait() {
        std::unique_lock<std::mutex> lk(m);
        size_t g = gen;
        if (++count == n) { count = 0; ++gen; cv.notify_all(); }
        else cv.wait(lk, [&] { return g != gen; });
    }
};

int main() {
    const int R = envi("BENCH_ROUNDS", 100);
    const size_t K = static_cast<size_t>(envi("BENCH_K", 2000));
    const size_t WARM = 256;
    const int spacing_ms = envi("BENCH_SLEEP_MS", 25);
    const int T = envi("BENCH_THREADS", 1);
    const int R2 = envi("BENCH_ROUNDS_CONTENDED", 60);
    const size_t K2 = static_cast<size_t>(envi("BENCH_K_CONTENDED", 1000));
    if (K + WARM >= RingBuffer::SIZE || K2 + WARM >= RingBuffer::SIZE) { std::cerr << "K+warm ring kapasitesini asiyor\n"; return 1; }

    ::unlink(kBenchOut);
    DirectWriter writer;
    if (!writer.Open(kBenchOut, 4, false)) { perror("Open"); return 1; }
    std::atomic<bool> stop_flush{false};
    std::atomic<uint64_t> flush_cpu_ns{0};
    std::thread flusher([&] {
        flush_thread_task(stop_flush, writer, 100, 1000);
        struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        flush_cpu_ns.store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec));
    });

    RingBuffer* my_ring = ProfilerRegisterThread();    // ACIK KAYIT: ilk-cagri maliyeti olcumlere girmesin
    std::vector<RingBuffer*> my_rings{my_ring};
    uint64_t attempted12 = 0;


    // ======================= FAZ 0: site-kimligi kontrolunun MIKRO maliyeti =======================
    {
        const Variant V0[] = {{"bos dongu (taban)", m_none, false}, {"PROFILER_SITE_ID (atomic relaxed)", m_atomic, false},
                              {"C++11 magic-static (guard)", m_magic, false}};
        std::vector<std::vector<Sample>> S0(3);
        for (int r = 0; r < 60; ++r)
            for (int j = 0; j < 3; ++j) {
                int idx = (j + r) % 3;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                S0[idx].push_back(TimedBatch(V0[idx].fn, WARM, 20000));
            }
        std::cout << "=== FAZ 0: SITE-KIMLIGI KONTROLUNUN MIKRO MALIYETI (K=20000, 60 tur, serpistirilmis; olay uretmez) ===\n";
        for (int i = 0; i < 3; ++i) PrintRow(V0[i].name, S0[i]);
        PrintDiff("atomic relaxed - bos dongu", PairedDiff(S0[1], S0[0], true));
        PrintDiff("magic-static   - bos dongu", PairedDiff(S0[2], S0[0], true));
        PrintDiff("magic-static   - atomic relaxed", PairedDiff(S0[2], S0[1], true));
        std::cout << "  (bu, ONLEM alinmis 'if (id==0)' kontrolunun tek basina maliyetidir; makronun tamamini FAZ 1'deki\n"
                     "   'BASLIK makro - V2a prototip' satiri verir: V2a prototip kimliksizdir)\n\n";
    }
    // ======================= FAZ 1: cekismesiz, tek thread =======================
    {
        const Variant V[] = {{"bare unique_lock", v_bare, false}, {"PROFILE_LOCK_SCOPE (BASLIK)", v_macro, true},
                             {"push_event tek basina", v_push, true}, {"steady_clock::now tek basina", v_clock, false},
                             {"V2a prototip (yerel kopya)", v_macro_reuse, true}, {"V2b try_lock hizli yol", v_macro_try, false},
                             {"ESKI makro (3 saat okumasi)", v_macro_old, true}};
        const int NV = 7;
        std::vector<std::vector<Sample>> S(NV);
        for (int r = 0; r < R; ++r)
            for (int j = 0; j < NV; ++j) {
                int idx = (j + r) % NV;                      // donen sira (Latin kare)
                Quiesce(my_rings, spacing_ms);
                S[idx].push_back(TimedBatch(V[idx].fn, WARM, K));
                if (V[idx].pushes) attempted12 += WARM + K;
            }
        std::cout << "=== FAZ 1: CEKISMESIZ (tek thread), K=" << K << " islem/batch, " << R << " tur, serpistirilmis ===\n";
        for (int i = 0; i < NV; ++i) PrintRow(V[i].name, S[i]);
        std::cout << "  (temiz = batch sirasinda thread bolunmedi ve bloke olmadi; istatistik temiz batch'lerle)\n";
        PrintDiff("makro - bare  (esli fark, temiz)", PairedDiff(S[1], S[0], true));
        PrintDiff("makro - bare  (esli fark, hepsi)", PairedDiff(S[1], S[0], false));
        PrintDiff("V2a - bare (esli, temiz)", PairedDiff(S[4], S[0], true));
        PrintDiff("V2b - bare (esli, temiz)", PairedDiff(S[5], S[0], true));
        PrintDiff("ESKI makro - bare (esli, temiz)", PairedDiff(S[6], S[0], true));
        PrintDiff("BASLIK makro - ESKI makro (esli)", PairedDiff(S[1], S[6], true));
        PrintDiff("BASLIK makro - V2a prototip (esli)", PairedDiff(S[1], S[4], true));
        const double clk = Pctl(Clean(S[3]), 50), push = Pctl(Clean(S[2]), 50);
        const double meas = Pctl(PairedDiff(S[1], S[0], true), 50);
        const double old_meas = Pctl(PairedDiff(S[6], S[0], true), 50);
        std::cout << "  AYRISTIRMA (ESKI makro): 2 x clock(" << clk << ") + push(" << push << ") = " << 2 * clk + push
                  << " ns ONGORULEN  vs  olculen = " << old_meas << " ns  (fark " << old_meas - (2 * clk + push) << " ns)\n"
                  << "  AYRISTIRMA (BASLIK makro, 2 okuma): 2 x clock + (push - clock) = " << 2 * clk + (push - clk)
                  << " ns ONGORULEN  vs  olculen = " << meas << " ns  (fark " << meas - (2 * clk + (push - clk)) << " ns)\n";
    }

    // ======================= FAZ 2: cekismeli (BENCH_THREADS>1) =======================
    if (T > 1) {
        Barrier bar(static_cast<size_t>(T) + 1);
        std::atomic<int> cmd{0};
        std::atomic<bool> quit{false};
        std::vector<RingBuffer*> slot(T, nullptr);
        std::vector<Sample> out(T);
        std::vector<std::thread> ws;
        for (int id = 0; id < T; ++id)
            ws.emplace_back([&, id] {
                slot[id] = ProfilerRegisterThread();
                bar.wait();                                   // hazir
                while (true) {
                    bar.wait();                               // tur baslangici
                    if (quit.load()) break;
                    void (*fn)(size_t) = (cmd.load() == 0) ? c_bare : c_macro;
                    fn(WARM);
                    bar.wait();                               // olculen kisim senkron baslangic
                    struct rusage a, b; getrusage(RUSAGE_THREAD, &a);
                    auto t0 = ProfilerClock::now(); fn(K2); auto t1 = ProfilerClock::now();
                    getrusage(RUSAGE_THREAD, &b);
                    out[id].ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(K2);
                    out[id].nvcsw = static_cast<uint64_t>(b.ru_nvcsw - a.ru_nvcsw);
                    out[id].nivcsw = static_cast<uint64_t>(b.ru_nivcsw - a.ru_nivcsw);
                    bar.wait();                               // tur sonu
                }
            });
        bar.wait();                                           // hepsi kayitli
        std::vector<RingBuffer*> rings(slot.begin(), slot.end());
        std::vector<double> mean_ns[2]; std::vector<double> nvcsw_mean[2];
        for (int r = 0; r < R2; ++r)
            for (int j = 0; j < 2; ++j) {
                int v = (j + r) % 2;
                Quiesce(rings, spacing_ms);
                cmd.store(v);
                bar.wait(); bar.wait(); bar.wait();
                double s = 0, nv = 0; for (auto& o : out) { s += o.ns; nv += static_cast<double>(o.nvcsw); }
                mean_ns[v].push_back(s / T); nvcsw_mean[v].push_back(nv / T);
                if (v == 1) attempted12 += static_cast<uint64_t>(T) * (WARM + K2);
            }
        quit.store(true); bar.wait();
        for (auto& w : ws) w.join();
        std::vector<double> d; for (size_t i = 0; i < mean_ns[0].size(); ++i) d.push_back(mean_ns[1][i] - mean_ns[0][i]);
        std::cout << "\n=== FAZ 2: CEKISMELI, " << T << " thread ayni mutex, K=" << K2 << " islem/thread/batch, " << R2 << " tur ===\n";
        std::cout << std::fixed << std::setprecision(1)
                  << "  bare  : medyan(thread ort.) = " << Pctl(mean_ns[0], 50) << " ns/islem  | ort. gonullu baglam degistirme/batch = " << Pctl(nvcsw_mean[0], 50) << "\n"
                  << "  makro : medyan(thread ort.) = " << Pctl(mean_ns[1], 50) << " ns/islem  | ort. gonullu baglam degistirme/batch = " << Pctl(nvcsw_mean[1], 50) << "\n";
        PrintDiff("makro - bare (esli, tur bazinda)", d);
        std::cout << "  NOT: gonullu baglam degistirme ~0 ise mutex uzerinde FIILEN BLOKE OLUNMADI = gercek cekisme olusmadi\n"
                  << "       (tek cekirdekte beklenir). Gercek cekisme + onbellek trafigi icin >=2 cekirdekli makinede calistirin.\n";
        // ddis thread'lerin ring'lerinin flush thread tarafindan silinmesini bekle
        for (int i = 0; i < 4000; ++i) {
            { std::lock_guard<std::mutex> lk(g_registry_mutex); if (g_registry.size() == 1) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // ======================= Flush thread'i durdur, muhasebe =======================
    Quiesce(my_rings, spacing_ms);
    const uint64_t dropped12 = g_dropped_total.load();
    const bool drained = (my_ring->tail.load() == my_ring->head.load());
    stop_flush.store(true, std::memory_order_release);
    flusher.join();
    const bool fin_ok = writer.Finalize();
    const uint64_t appended = writer.stats().events_appended;
    std::cout << "\n=== MUHASEBE (faz 1+2) ===\n"
              << "  uretilen=" << attempted12 << " dosyaya_giren=" << appended << " dusen=" << dropped12
              << "  -> uretilen==dosyaya_giren+dusen: " << (attempted12 == appended + dropped12 ? "EVET" : "HAYIR!")
              << "  (ring bosaltildi=" << (drained ? "evet" : "HAYIR") << ", quiesce_zaman_asimi=" << g_quiesce_timeouts << ")\n"
              << "  flush thread CPU: " << std::fixed << std::setprecision(1) << flush_cpu_ns.load() / 1e6 << " ms toplam -> "
              << static_cast<double>(flush_cpu_ns.load()) / static_cast<double>(appended ? appended : 1)
              << " ns/olay (sabit tur maliyeti + kopyalama + pwrite/fdatasync dahil)\n";

    // ======================= FAZ 3: dolu ring (dusurme yolu) =======================
    {
        for (size_t i = 0; i < RingBuffer::SIZE + 16; ++i) push_event(EVENT_LOCK_WAIT, 0);   // tuketici yok: ring dolar
        const uint64_t d0 = g_dropped_total.load();
        push_event(EVENT_LOCK_WAIT, 0);
        const bool full = (g_dropped_total.load() == d0 + 1);
        const Variant V[] = {{"bare unique_lock", v_bare, false}, {"PROFILE_LOCK_SCOPE (dolu ring)", v_macro, true}};
        std::vector<std::vector<Sample>> S(2);
        uint64_t dropped_in_timed = 0;
        for (int r = 0; r < 60; ++r)
            for (int j = 0; j < 2; ++j) {
                int idx = (j + r) % 2;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                const uint64_t a = g_dropped_total.load();
                S[idx].push_back(TimedBatch(V[idx].fn, WARM, K));
                if (idx == 1) dropped_in_timed += g_dropped_total.load() - a;
            }
        std::cout << "\n=== FAZ 3: DOLU RING (dusurme yolu; flush thread durdurulmus), K=" << K << " ===\n"
                  << "  ring dolu mu=" << (full ? "evet" : "HAYIR!") << "  batch'lerde dusen olay=" << dropped_in_timed
                  << " (beklenen " << 60 * (WARM + K) << ")\n";
        for (int i = 0; i < 2; ++i) PrintRow(V[i].name, S[i]);
        PrintDiff("makro(dolu) - bare (esli, temiz)", PairedDiff(S[1], S[0], true));
        if (!full || dropped_in_timed != static_cast<uint64_t>(60) * (WARM + K)) { std::cout << "  !! dusurme yolu beklenen sekilde calismadi\n"; return 2; }
    }
    ::unlink(kBenchOut);
    return (fin_ok && attempted12 == appended + dropped12 && drained) ? 0 : 2;
}
