// profiler_core.h -- per-thread ring buffer, registry, kullanici makrolari, flush thread.
// TEK KAYNAK: testler bunu include eder (onceki kopya-yapistir cekirdek dagitimi kaldirildi).
// C++17: 'inline' degiskenler sayesinde birden cok TU'da tek ornek (ODR guvenli).
#pragma once
#include "profiler_io.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <algorithm>
#include <type_traits>
#include <map>
#include <string>
#include <utility>

using ProfilerClock = std::chrono::steady_clock;

inline uint32_t clamp_u32(int64_t v) {
    if (v < 0) return 0;
    if (v > 0xFFFFFFFFLL) return 0xFFFFFFFFu;
    return static_cast<uint32_t>(v);
}
inline uint64_t ProfilerTimestampNs(ProfilerClock::time_point t) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
}
inline uint64_t now_ns() { return ProfilerTimestampNs(ProfilerClock::now()); }


// =====================================================================
// CAGRI NOKTASI (site) KIMLIGI
//  * Kimlik 12 bit (1..4094 gercek, 4095 = TASMA kovasi, 0 = kimliksiz).
//  * Tablo KUCUK ve SOGUK yoldur: yalniz bir cagri noktasindan ILK gecista (ve flush thread yeni site
//    gordugunde) dokunulur. Sicak yol yalniz bir 'relaxed atomic load' + tahmin edilebilir dal yapar.
//  * Tanimlar RING'E GIRMEZ (ring kayiplidir, tanim bir kereye mahsustur; dusen tanim o sitenin butun
//    olaylarini sonsuza dek cozulemez kilardi). Flush thread tanimlari kucuk tablodan okuyup dosyaya
//    yazar ve "tanim, ona basvuran HER olaydan once dosyada gelir" sirasini garanti eder.
// =====================================================================
enum SiteKind : uint8_t { SITE_LOCK = 1, SITE_DEADLINE = 2 };
constexpr size_t kMaxSiteName = 120;     // 10 parca x 12 bayt

struct SiteInfo { std::string name; uint8_t kind = 0; };
struct SiteTable {
    std::mutex m;
    std::vector<SiteInfo> sites = std::vector<SiteInfo>(1);            // indeks = id; 0 = "kimliksiz"
    std::map<std::pair<uint8_t, std::string>, uint16_t> index;         // (tur, isim) -> id: ayni isim = ayni site
};
inline SiteTable& Sites() { static SiteTable t; return t; }
inline std::atomic<uint16_t> g_site_count{0};         // atanmis en buyuk id (flush thread'in "yeni site var mi" gostergesi)
inline std::atomic<uint64_t> g_site_overflow{0};      // 4095'e dusen (tablo dolu) kayit denemesi sayisi
inline std::atomic<uint64_t> g_site_truncated{0};     // kisaltilan isim sayisi

// Otomatik konum adi "yol/dosya.cpp:123": yalnizca SON IKI yol bileseni tutulur (kisa ama ayirt edici).
inline std::string NormalizeAutoSiteName(const std::string& raw) {
    size_t colon = raw.rfind(':');
    std::string file = colon == std::string::npos ? raw : raw.substr(0, colon);
    std::string tail = colon == std::string::npos ? "" : raw.substr(colon);
    int seps = 0; size_t cut = 0;
    for (size_t i = file.size(); i-- > 0;) {
        if (file[i] == '/' || file[i] == '\\') { if (++seps == 2) { cut = i + 1; break; } }
    }
    return file.substr(cut) + tail;
}

inline uint16_t RegisterSite(const char* raw_name, uint8_t kind, bool auto_location) {
    std::string name = raw_name ? raw_name : "";
    if (auto_location) name = NormalizeAutoSiteName(name);
    if (name.empty()) name = "<unnamed>";
    if (name.size() > kMaxSiteName) {
        g_site_truncated.fetch_add(1, std::memory_order_relaxed);
        name = auto_location ? name.substr(name.size() - kMaxSiteName) : name.substr(0, kMaxSiteName);   // otomatikte ':satir' korunur
    }
    SiteTable& t = Sites();
    std::lock_guard<std::mutex> lk(t.m);
    auto key = std::make_pair(kind, name);
    auto it = t.index.find(key);
    if (it != t.index.end()) return it->second;                        // ayni (tur,isim): AYNI id (eszamanli kayitlar da guvenli)
    const size_t assigned = t.sites.size() - 1;
    if (assigned >= static_cast<size_t>(kMaxSiteId) - 1) {             // 4094 dolu: TASMA kovasi
        g_site_overflow.fetch_add(1, std::memory_order_relaxed);
        if (assigned == static_cast<size_t>(kMaxSiteId) - 1) {
            t.sites.push_back(SiteInfo{"<site-table-overflow>", 0});
            g_site_count.store(kMaxSiteId, std::memory_order_release);
        }
        return kMaxSiteId;
    }
    const uint16_t id = static_cast<uint16_t>(assigned + 1);
    t.sites.push_back(SiteInfo{name, kind});
    t.index.emplace(std::move(key), id);
    g_site_count.store(id, std::memory_order_release);                 // push_back'ten SONRA yayinla
    return id;
}
inline std::string ProfilerSiteName(uint16_t id) {                      // test/teshis yardimcisi
    SiteTable& t = Sites();
    std::lock_guard<std::mutex> lk(t.m);
    return id < t.sites.size() ? t.sites[id].name : std::string("<bilinmeyen>");
}

// Cagri noktasi basina KIMLIK: fonksiyon-yerel STATIK atomic (sabit ilklendirme => korumasiz, 'guard' yok).
// Eszamanli ilk gecislerde birden cok thread RegisterSite cagirabilir; RegisterSite (tur,isim) ile tekillestirdigi
// icin hepsi AYNI id'yi alir ve ayni degeri yazar => yarissiz VE idempotent. Lambda: her makro genislemesi kendi
// closure tipini (=> kendi statigini) alir; isim BIR STRING LITERALI olmalidir (yakalamasiz lambda).
#define PROFILER_STR_(x) #x
#define PROFILER_STR(x) PROFILER_STR_(x)
#define PROFILER_CAT_(a, b) a##b
#define PROFILER_CAT(a, b) PROFILER_CAT_(a, b)
#define PROFILER_SITE_ID(name, kind, auto_loc) \
    ([]() -> uint16_t { \
        static std::atomic<uint16_t> s_id{0}; \
        uint16_t v = s_id.load(std::memory_order_relaxed); \
        if (__builtin_expect(v == 0, 0)) { v = RegisterSite(name, kind, auto_loc); s_id.store(v, std::memory_order_relaxed); } \
        return v; \
    })()

// ---------------- Per-thread SPSC ring buffer ----------------
// Ring boyutu (slot sayisi): -DPROFILER_RING_SIZE=<2'nin kuvveti>. Varsayilan 4096 (64 KB/thread).
// Secim kurali: takilma_toleransi = SIZE / olay_hizi ; surdurulebilir tepe hiz ~= SIZE / flush_turu (20 ms).
#ifndef PROFILER_RING_SIZE
#define PROFILER_RING_SIZE 4096
#endif
struct RingBuffer {
    static constexpr size_t SIZE = PROFILER_RING_SIZE;
    static_assert((SIZE & (SIZE - 1)) == 0, "PROFILER_RING_SIZE 2'nin kuvveti olmali");
    static_assert(SIZE >= 64, "PROFILER_RING_SIZE >= 64 olmali");
    alignas(64) std::atomic<uint64_t> head{0};   // sadece sahibi thread yazar
    alignas(64) std::atomic<uint64_t> tail{0};   // sadece flush thread yazar
    alignas(64) std::atomic<bool> closing{false};
    uint16_t thread_id = 0;
    ProfilerEvent events[SIZE];
};

// ---------------- Global durum ----------------
inline std::mutex g_registry_mutex;
inline std::vector<RingBuffer*> g_registry;
inline std::atomic<uint16_t> g_next_thread_id{0};
inline std::atomic<uint64_t> g_dropped_total{0};
// Sicak yolda (ilk push_event icinde) GEC kayit yapan thread sayisi. Beklenen deger: 0.
// Sifirdan buyukse biri ProfilerRegisterThread()'i cagirmayi unutmus demektir; ilk olayda
// ~40-120 us'lik tahsis+kilit maliyetini sicak yolda oduyor. Sessiz kalmasin diye SAYILIR.
inline std::atomic<uint64_t> g_lazy_registrations{0};
// TEST KANCALARI (varsayilanda etkisiz):
inline std::atomic<uint64_t>* g_test_pushed = nullptr;   // cokme testinde paylasimli bellek sayaci
inline std::atomic<int> g_test_flush_stall_ms{0};        // >0: flush thread bir sonraki turda bu kadar ms uyur

struct ThreadLocalSentinel {
    RingBuffer* buf = nullptr;
    ~ThreadLocalSentinel() { if (buf) buf->closing.store(true, std::memory_order_release); }  // delete YOK: sahiplik flush thread'de
};
inline thread_local ThreadLocalSentinel g_sentinel;

// ACIK KAYIT: thread'in KURULUM evresinde (sicak yolun DISINDA) bir kez cagrilir. Idempotent.
// Tum pahali isi burada odenir: 64 KB ring tahsisi, sayfalarin onceden dokunulmasi (value-init
// diziyi sifirlar => sayfa hatalari BURADA olur, sicak yolda degil), registry kilidi, TLS ilk erisimi.
inline RingBuffer* ProfilerRegisterThread() {
    if (g_sentinel.buf) return g_sentinel.buf;
    RingBuffer* rb = new RingBuffer();   // '()' KASITLI: value-initialization => events[] sifirlanir, sayfalar cozulur
    rb->thread_id = g_next_thread_id.fetch_add(1, std::memory_order_relaxed);
    g_sentinel.buf = rb;
    std::lock_guard<std::mutex> lk(g_registry_mutex);
    g_registry.push_back(rb);
    return rb;
}

// GEC KAYIT (geri dusus): kayit cagrilmadiysa sistem calismaya devam eder, ama ceza SAYILIR.
inline RingBuffer* GetThreadLocalBuffer() {
    RingBuffer* rb = g_sentinel.buf;
    if (__builtin_expect(rb != nullptr, 1)) return rb;
    g_lazy_registrations.fetch_add(1, std::memory_order_relaxed);
    return ProfilerRegisterThread();
}

namespace profiler_detail {
// kReadClock=true : zaman damgasini kendisi okur (push_event). Dusurme yolunda SAAT OKUMASI YOK.
// kReadClock=false: zaman damgasi cagirandan gelir (push_event_at): makro zaten okudugu t1'i verir.
template <bool kReadClock>
inline void PushImpl(uint64_t ts, uint16_t event_type, uint32_t payload) {
    RingBuffer* rb = GetThreadLocalBuffer();
    uint64_t h = rb->head.load(std::memory_order_relaxed);
    uint64_t t = rb->tail.load(std::memory_order_acquire);
    if (h - t >= RingBuffer::SIZE) {
        g_dropped_total.fetch_add(1, std::memory_order_relaxed);
        return;   // buffer dolu: bloklanma, veriyi bozma -- event'i dusur ve say
    }
    ProfilerEvent& slot = rb->events[h & (RingBuffer::SIZE - 1)];
    slot.timestamp = kReadClock ? now_ns() : ts;
    slot.thread_id = rb->thread_id;
    slot.event_type = event_type;
    slot.payload = payload;
    rb->head.store(h + 1, std::memory_order_release);
    if (g_test_pushed) g_test_pushed->fetch_add(1, std::memory_order_relaxed);
}
}  // namespace profiler_detail

inline void push_event(uint16_t event_type, uint32_t payload) { profiler_detail::PushImpl<true>(0, event_type, payload); }
// push_event_at: 'type_site' HAM 16 bit alandir = MakeTypeSite(tur, site). push_event(tur, payload) ise site=0 (kimliksiz).
inline void push_event_at(uint64_t ts_ns, uint16_t type_site, uint32_t payload) { profiler_detail::PushImpl<false>(ts_ns, type_site, payload); }

// ---------------- Kullanici makrolari ----------------
// Saat okumasi: t0 (kilit oncesi) ve t1 (kilit sonrasi) = 2 tane; t1 hem bekleme suresini hem olayin
// zaman damgasini verir. Site kimligi t0'DAN ONCE cozulur: ilk gecisteki kayit maliyeti (mutex+map+string)
// ne olculen bekleme suresine ne de kullanicinin KRITIK BOLUMUNE girer.
#define PROFILE_LOCK_SCOPE_IMPL(mtx, lock_name, site_expr) \
    const uint16_t _site_##lock_name = (site_expr); \
    const auto _t0_##lock_name = ProfilerClock::now(); \
    std::unique_lock<std::decay_t<decltype(mtx)>> lock_name(mtx); \
    const auto _t1_##lock_name = ProfilerClock::now(); \
    push_event_at(ProfilerTimestampNs(_t1_##lock_name), MakeTypeSite(EVENT_LOCK_WAIT, _site_##lock_name), \
                  clamp_u32(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                      _t1_##lock_name - _t0_##lock_name).count()))
// Otomatik isim: "dosya.cpp:satir"
#define PROFILE_LOCK_SCOPE(mtx, lock_name) \
    PROFILE_LOCK_SCOPE_IMPL(mtx, lock_name, PROFILER_SITE_ID(__FILE__ ":" PROFILER_STR(__LINE__), SITE_LOCK, true))
// Kullanici adi (STRING LITERALI): ayni isim birden cok cagri noktasinda kullanilirsa TEK site olarak toplanir
#define PROFILE_LOCK_SCOPE_N(mtx, lock_name, name) \
    PROFILE_LOCK_SCOPE_IMPL(mtx, lock_name, PROFILER_SITE_ID(name, SITE_LOCK, false))

class DeadlineProfiler {
public:
    explicit DeadlineProfiler(uint64_t budget_us, uint16_t site = 0)
        : budget_ns_(budget_us * 1000ULL), site_(site), start_(ProfilerClock::now()) {}
    ~DeadlineProfiler() {
        const auto end = ProfilerClock::now();
        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        if (elapsed_ns > static_cast<int64_t>(budget_ns_))
            push_event_at(ProfilerTimestampNs(end), MakeTypeSite(EVENT_DEADLINE_MISS, site_),
                          clamp_u32(elapsed_ns - static_cast<int64_t>(budget_ns_)));
    }
private:
    uint64_t budget_ns_;
    uint16_t site_;
    ProfilerClock::time_point start_;
};
// Kimlik, DeadlineProfiler NESNESI kurulmadan (=> zamanlayici baslamadan) once cozulur.
#define PROFILE_DEADLINE(budget_us) \
    DeadlineProfiler PROFILER_CAT(_dl_profiler_, __LINE__)(budget_us, PROFILER_SITE_ID(__FILE__ ":" PROFILER_STR(__LINE__), SITE_DEADLINE, true))
#define PROFILE_DEADLINE_N(budget_us, name) \
    DeadlineProfiler PROFILER_CAT(_dl_profiler_, __LINE__)(budget_us, PROFILER_SITE_ID(name, SITE_DEADLINE, false))

inline void busy_wait_us(double target_us) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() < target_us) {}
}

// ---------------- Dosya basligi ve site TANIMLARI: flush thread yazar, RING'DEN GECMEZ ----------------
inline void EmitFileHeader(DirectWriter& w) {
    ProfilerEvent h{};
    h.timestamp = now_ns();
    h.event_type = MakeTypeSite(EVENT_FILE_HEADER, 0);
    h.payload = kProfilerFormatVersion;
    w.AppendMeta(&h, 1);
}
struct SiteDefEmitter {
    uint16_t emitted = 0;
    // Yeni site tanimlarini (SITE_DEF + ceil(len/12) SITE_CHUNK) yazar. Cagirma noktasi bir ring'in
    // head'ini ACQUIRE ile yukledikten SONRA, o ring'in olaylarini kopyalamadan ONCE olmalidir:
    // olay push'undan once site kaydi tamamlanmis (happens-before) => burada g_site_count o siteyi kapsar
    // => tanim dosyada olaydan ONCE gelir.
    void EmitNew(DirectWriter& w) {
        const uint16_t n = g_site_count.load(std::memory_order_acquire);
        if (n <= emitted) return;
        std::vector<std::pair<uint16_t, SiteInfo>> fresh;
        {
            SiteTable& t = Sites();
            std::lock_guard<std::mutex> lk(t.m);
            for (uint16_t id = static_cast<uint16_t>(emitted + 1); id <= n; ++id) fresh.emplace_back(id, t.sites[id]);
        }
        std::vector<ProfilerEvent> recs;
        for (auto& [id, si] : fresh) {
            ProfilerEvent d{};
            d.event_type = MakeTypeSite(EVENT_SITE_DEF, id);
            d.thread_id = si.kind;
            d.payload = static_cast<uint32_t>(si.name.size());
            recs.push_back(d);
            for (size_t off = 0, idx = 0; off < si.name.size(); off += 12, ++idx) {
                ProfilerEvent c{};
                c.event_type = MakeTypeSite(EVENT_SITE_CHUNK, id);
                c.thread_id = static_cast<uint16_t>(idx);
                char buf[12] = {0};
                std::memcpy(buf, si.name.data() + off, std::min<size_t>(12, si.name.size() - off));
                std::memcpy(&c.timestamp, buf, 8);
                std::memcpy(&c.payload, buf + 8, 4);
                recs.push_back(c);
            }
        }
        w.AppendMeta(recs.data(), recs.size());
        emitted = n;
    }
};

// ---------------- Flush thread: ring buffer -> staging (DirectWriter) ----------------
inline void flush_thread_task(std::atomic<bool>& stop_flush, DirectWriter& writer, int partial_ms, int sync_ms) {
    using Clk = std::chrono::steady_clock;
    auto last_partial = Clk::now(), last_sync = last_partial;
    EmitFileHeader(writer);
    SiteDefEmitter site_defs;

    // Bir tahliye turu: registry'deki her ring'in bekleyen olaylarini staging'e alir; kapanmis+bosalmis ring'leri siler.
    auto drain_pass = [&]() {
        std::vector<RingBuffer*> snapshot;
        {
            std::lock_guard<std::mutex> lk(g_registry_mutex);
            snapshot = g_registry;
        }
        for (RingBuffer* rb : snapshot) {
            uint64_t h = rb->head.load(std::memory_order_acquire);
            uint64_t t = rb->tail.load(std::memory_order_relaxed);
            site_defs.EmitNew(writer);   // head ACQUIRE'dan SONRA, kopyalamadan ONCE (bkz. SiteDefEmitter)
            while (t < h) {
                size_t idx = static_cast<size_t>(t & (RingBuffer::SIZE - 1));
                size_t run = static_cast<size_t>(std::min<uint64_t>(h - t, RingBuffer::SIZE - idx));
                writer.Append(&rb->events[idx], run);
                t += run;
            }
            rb->tail.store(t, std::memory_order_release);   // event'ler staging'e KOPYALANDIKTAN sonra

            if (rb->closing.load(std::memory_order_acquire)) {
                uint64_t h2 = rb->head.load(std::memory_order_acquire);
                uint64_t t2 = rb->tail.load(std::memory_order_acquire);
                if (h2 == t2) {
                    {   // kilit ALTINDA erase (swap-and-pop)
                        std::lock_guard<std::mutex> lk(g_registry_mutex);
                        auto it = std::find(g_registry.begin(), g_registry.end(), rb);
                        if (it != g_registry.end()) { std::swap(*it, g_registry.back()); g_registry.pop_back(); }
                    }
                    delete rb;   // kilit DISINDA delete
                }
            }
        }
    };

    while (!stop_flush.load(std::memory_order_acquire)) {
        {
            int st = g_test_flush_stall_ms.exchange(0, std::memory_order_relaxed);
            if (st > 0) std::this_thread::sleep_for(std::chrono::milliseconds(st));
        }
        drain_pass();
        auto now = Clk::now();
        if (partial_ms > 0 && writer.dirty() && now - last_partial >= std::chrono::milliseconds(partial_ms)) {
            writer.FlushPartial();
            last_partial = now;
        }
        if (sync_ms > 0 && now - last_sync >= std::chrono::milliseconds(sync_ms)) {
            writer.Sync();
            last_sync = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    drain_pass();                    // SON TAHLIYE: durdurma isteginden sonra, YASAYAN thread'lerin ring'lerinde kalan her sey de alinir
    site_defs.EmitNew(writer);       // tablo HER ZAMAN eksiksiz kapansin
}

// =====================================================================
// ProfilerSession: kullaniciya donuk TEK giris noktasi (yazici + flush thread yasam dongusu).
//   ProfilerSession s; ProfilerOptions o; o.path = "trace.bin"; s.Start(o); ... s.Stop();   (Stop yikicida da cagrilir)
//  * Surec basina TEK oturum (kayit tablosu ve registry kuresel). Start, Stop'tan sonra tekrar cagrilamaz.
//  * Stop(): flush thread'e durma istegi gonderir; thread SON TAHLIYE turunu yapar (yasayan thread'lerin
//    ring'leri dahil), kismi blogu dolgulayip yazar, fdatasync + close. Stop cagrilmadan ONCE uretici
//    thread'leri durdurun: Stop'tan sonra push edilen olaylar dosyaya girmez.
// =====================================================================
struct ProfilerOptions {
    std::string path;
    int partial_flush_ms = 100;      // 0 = kapali (yalniz Stop'ta yazilir; cokme kaybi buyur)
    int sync_ms = 1000;              // 0 = kapali
    size_t staging_blocks = 4;
    bool force_buffered = false;
};
class ProfilerSession {
public:
    ProfilerSession() = default;
    ProfilerSession(const ProfilerSession&) = delete;
    ProfilerSession& operator=(const ProfilerSession&) = delete;
    ~ProfilerSession() { Stop(); }

    bool Start(const ProfilerOptions& o) {
        if (started_) return false;                      // tek oturum: yeniden baslatma yok
        if (!writer_.Open(o.path, o.staging_blocks, o.force_buffered)) return false;
        started_ = running_ = true;
        stop_.store(false, std::memory_order_release);
        flusher_ = std::thread(flush_thread_task, std::ref(stop_), std::ref(writer_), o.partial_flush_ms, o.sync_ms);
        return true;
    }
    // true: dosya tam ve hatasiz kapandi. Tekrar cagrilabilir (idempotent).
    bool Stop() {
        if (!running_) return stop_ok_;
        stop_.store(true, std::memory_order_release);
        flusher_.join();
        stop_ok_ = writer_.Finalize();
        running_ = false;
        return stop_ok_;
    }
    bool running() const { return running_; }
    const DirectWriter& writer() const { return writer_; }   // Stop'tan sonra istatistikler guvenle okunur

private:
    DirectWriter writer_;
    std::atomic<bool> stop_{false};
    std::thread flusher_;
    bool started_ = false, running_ = false, stop_ok_ = true;
};
