// macro_semantics_test.cpp -- PROFILE_LOCK_SCOPE[_N] / PROFILE_DEADLINE[_N] / push_event_at ANLAM testi (kimlik dahil).
// Zaman kontrolleri AYNI monotonik saatten okunan degerler arasindaki KESIN siralama iliskileridir
//   o0 <= t0 <= t1 <= o1   (o0,o1 = testin dis okumalari) => zamanlama TOLERANSI yok; yalniz "bekleme >= 4 ms",
//   "asim >= 2 ms" gibi alt sinirlar var (bloke etme / busy-wait tasarimiyla GARANTI).
// Kimlik kontrolleri: olayin site alani = kayit tablosundaki id; otomatik ad "dosya:satir" (satir KESIN dogru);
//   ayni kullanici adi iki cagri noktasinda TEK site; her cagri noktasi kendi statigini kullanir (dongu = tek kayit).
#include "profiler_core.h"
#include <iostream>
#include <cassert>

static ProfilerEvent LastEvent(RingBuffer* rb) {
    return rb->events[(rb->head.load() - 1) & (RingBuffer::SIZE - 1)];
}
static bool EndsWith(const std::string& s, const std::string& suf) { return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0; }

int main() {
    RingBuffer* rb = ProfilerRegisterThread();     // flush thread YOK: ring'i biz gozlemliyoruz
    int checks = 0;

    // ---- 1) CEKISMELI kilit (kullanici adli): yardimci thread kilidi >=5 ms tutar ----
    {
        std::mutex m;
        std::atomic<bool> held{false}, go{false};
        std::thread helper([&] {
            std::unique_lock<std::mutex> lk(m);
            held.store(true);
            while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        while (!held.load()) std::this_thread::yield();
        const uint64_t head0 = rb->head.load();
        go.store(true);
        const auto o0 = ProfilerClock::now();
        { PROFILE_LOCK_SCOPE_N(m, lk, "test_kilidi"); }
        const auto o1 = ProfilerClock::now();
        helper.join();

        assert(rb->head.load() == head0 + 1); checks++;
        ProfilerEvent e = LastEvent(rb);
        const uint64_t ts0 = ProfilerTimestampNs(o0), ts1 = ProfilerTimestampNs(o1);
        assert(EventTypeOf(e) == EVENT_LOCK_WAIT); checks++;
        assert(e.timestamp >= ts0 && e.timestamp <= ts1); checks++;
        assert(e.payload <= ts1 - ts0); checks++;
        assert(e.timestamp - e.payload >= ts0); checks++;
        assert(e.payload >= 4'000'000u); checks++;
        assert(e.thread_id == rb->thread_id); checks++;
        assert(EventSiteOf(e) != 0 && ProfilerSiteName(EventSiteOf(e)) == "test_kilidi"); checks += 2;
        std::cout << "1) cekismeli kilit: bekleme=" << e.payload / 1e6 << " ms, site='" << ProfilerSiteName(EventSiteOf(e)) << "': OK\n";
    }

    // ---- 2) CEKISMESIZ kilit, OTOMATIK ad: satir numarasi KESIN dogru ----
    {
        std::mutex m;
        const uint64_t head0 = rb->head.load();
        const auto o0 = ProfilerClock::now();
        int auto_line = 0;
        { PROFILE_LOCK_SCOPE(m, lk); auto_line = __LINE__; }
        const auto o1 = ProfilerClock::now();
        assert(rb->head.load() == head0 + 1); checks++;
        ProfilerEvent e = LastEvent(rb);
        assert(e.timestamp >= ProfilerTimestampNs(o0) && e.timestamp <= ProfilerTimestampNs(o1)); checks++;
        assert(e.payload <= ProfilerTimestampNs(o1) - ProfilerTimestampNs(o0)); checks++;
        const std::string nm = ProfilerSiteName(EventSiteOf(e));
        assert(EndsWith(nm, "macro_semantics_test.cpp:" + std::to_string(auto_line))); checks++;
        std::cout << "2) cekismesiz kilit: bekleme=" << e.payload << " ns, otomatik ad='" << nm << "': OK\n";
    }

    // ---- 3) DEADLINE ASIMI (adli): busy_wait(3000us), butce 1000us => asim >= 2 ms GARANTI ----
    {
        const uint64_t head0 = rb->head.load();
        const auto o0 = ProfilerClock::now();
        { PROFILE_DEADLINE_N(1000, "test_bolgesi"); busy_wait_us(3000); }
        const auto o1 = ProfilerClock::now();
        assert(rb->head.load() == head0 + 1); checks++;
        ProfilerEvent e = LastEvent(rb);
        const uint64_t ts0 = ProfilerTimestampNs(o0), ts1 = ProfilerTimestampNs(o1);
        assert(EventTypeOf(e) == EVENT_DEADLINE_MISS); checks++;
        assert(e.timestamp >= ts0 && e.timestamp <= ts1); checks++;
        assert(e.payload >= 2'000'000u); checks++;
        assert(static_cast<uint64_t>(e.payload) + 1'000'000u <= ts1 - ts0); checks++;
        assert(ProfilerSiteName(EventSiteOf(e)) == "test_bolgesi"); checks++;
        std::cout << "3) deadline asimi: asim=" << e.payload / 1e6 << " ms, site='" << ProfilerSiteName(EventSiteOf(e)) << "': OK\n";
    }

    // ---- 4) ASIM YOK => olay YOK ----
    {
        const uint64_t head0 = rb->head.load();
        { PROFILE_DEADLINE_N(60'000'000, "asilmayan"); }
        assert(rb->head.load() == head0); checks++;
        std::cout << "4) asim yok: olay uretilmedi: OK\n";
    }

    // ---- 5) push_event_at ham alani AYNEN yazar; push_event kendisi okur ve site=0 ----
    {
        push_event_at(123456789ull, MakeTypeSite(EVENT_LOCK_WAIT, 77), 77);
        ProfilerEvent e = LastEvent(rb);
        assert(e.timestamp == 123456789ull && e.payload == 77u && EventTypeOf(e) == EVENT_LOCK_WAIT && EventSiteOf(e) == 77); checks += 4;
        const uint64_t a = now_ns();
        push_event(EVENT_LOCK_WAIT, 78);
        const uint64_t b = now_ns();
        e = LastEvent(rb);
        assert(e.timestamp >= a && e.timestamp <= b && e.payload == 78u && EventSiteOf(e) == 0); checks += 3;
        // tur/site bit paketleme: 4 bit tur + 12 bit site, tasma yok
        assert(MakeTypeSite(EVENT_DEADLINE_MISS, kMaxSiteId) == 0xFFF2 && EventTypeOf(ProfilerEvent{0, 0, 0xFFF2, 0}) == 2 && EventSiteOf(ProfilerEvent{0, 0, 0xFFF2, 0}) == kMaxSiteId); checks += 3;
        std::cout << "5) push_event_at / push_event / tur+site bit paketleme: OK\n";
    }

    // ---- 6) KIMLIK: ayni kullanici adi IKI cagri noktasinda TEK site; her cagri noktasi kendi statigi ----
    {
        std::mutex m;
        const uint16_t before = g_site_count.load();
        uint16_t ids[3] = {0, 0, 0};
        // NOT: ring'e yazma artik kilit BIRAKILDIKTAN SONRA (kapsam sonunda) olur (DeferredLockPush).
        // Gercek kullanimda makro zaten kritik bolgenin kendisini tanimlar; kapsam icinde makrodan
        // SONRA ring'i elle okumak tipik degildir, o yuzden LastEvent'i kapsam KAPANDIKTAN SONRA okuyoruz.
        { PROFILE_LOCK_SCOPE_N(m, lk, "ortak_ad"); }
        ids[0] = EventSiteOf(LastEvent(rb));
        { PROFILE_LOCK_SCOPE_N(m, lk, "ortak_ad"); }      // ikinci cagri noktasi, AYNI ad
        ids[1] = EventSiteOf(LastEvent(rb));
        { PROFILE_LOCK_SCOPE_N(m, lk, "baska_ad"); }
        ids[2] = EventSiteOf(LastEvent(rb));
        assert(ids[0] == ids[1] && ids[0] != ids[2] && ids[0] != 0 && ids[2] != 0); checks += 3;
        assert(g_site_count.load() == before + 2); checks++;                                    // 2 yeni site (ortak_ad, baska_ad)
        const uint16_t c0 = g_site_count.load();
        for (int i = 0; i < 100; ++i) { PROFILE_LOCK_SCOPE_N(m, lk, "dongu_sitesi"); }         // TEK cagri noktasi, 100 gecis
        assert(g_site_count.load() == c0 + 1); checks++;                                        // yalniz BIR kayit (statik onbellek calisiyor)
        // ayni SATIRDA farkli tur: kilit ve deadline ayni adi kullanabilir ama ayri site (tur ayrimi)
        uint16_t lock_id = RegisterSite("ayni_ad", SITE_LOCK, false), dl_id = RegisterSite("ayni_ad", SITE_DEADLINE, false);
        assert(lock_id != dl_id); checks++;
        std::cout << "6) kimlik: ayni ad iki cagri noktasi = tek site; dongu = tek kayit; tur ayrimi: OK\n";
    }

    // ---- 7) DOLU RING: her yol dusurur, head ilerlemez, sayac artar, veri bozulmaz (kimlikli makro dahil) ----
    {
        while (rb->head.load() - rb->tail.load() < RingBuffer::SIZE) push_event(EVENT_LOCK_WAIT, 1);
        const uint64_t head0 = rb->head.load(), d0 = g_dropped_total.load();
        ProfilerEvent last_before = LastEvent(rb);
        push_event(EVENT_LOCK_WAIT, 2);
        push_event_at(42, MakeTypeSite(EVENT_LOCK_WAIT, 5), 3);
        std::mutex m;
        { PROFILE_LOCK_SCOPE_N(m, lk, "dolu_ring_sitesi"); }
        assert(rb->head.load() == head0); checks++;
        assert(g_dropped_total.load() == d0 + 3); checks++;
        ProfilerEvent last_after = LastEvent(rb);
        assert(std::memcmp(&last_before, &last_after, sizeof(ProfilerEvent)) == 0); checks++;
        assert(ProfilerSiteName(g_site_count.load()) != "" ); checks++;                         // site KAYDI, olay dusse de yapildi
        std::cout << "7) dolu ring (SIZE=" << RingBuffer::SIZE << "): 3 dusme sayildi, veri bozulmadi: OK\n";
    }

    std::cout << "\n" << checks << " kesin kontrol: TUM ANLAM TESTLERI BASARILI (ring boyutu=" << RingBuffer::SIZE << ")\n";
    return 0;
}
