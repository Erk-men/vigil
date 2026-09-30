// site_registry_test.cpp -- site tablosu: tekillestirme, isim normallestirme, kisaltma, eszamanli kayit,
// tanim kodlama<->cozme gidis-donusu (parca sinirlari, UTF-8), TASMA davranisi.
// Derleme: g++ -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude tests/site_registry_test.cpp
#include "profiler_core.h"
#include "profiler_reader.h"
#include <iostream>
#include <cassert>
#include <random>
#include <cmath>

int main() {
    int checks = 0;
    // --- 1) normallestirme ---
    assert(NormalizeAutoSiteName("a/b/c/d.cpp:12") == "c/d.cpp:12"); checks++;
    assert(NormalizeAutoSiteName("d.cpp:12") == "d.cpp:12"); checks++;
    assert(NormalizeAutoSiteName("x/d.cpp:12") == "x/d.cpp:12"); checks++;
    assert(NormalizeAutoSiteName("C:\\proj\\src\\z.cpp:5") == "src\\z.cpp:5"); checks++;

    // --- 2) tekillestirme / tur ayrimi / bos isim / id sirasi ---
    const uint16_t base = g_site_count.load();
    uint16_t a = RegisterSite("kuyruk_kilidi", SITE_LOCK, false);
    uint16_t a2 = RegisterSite("kuyruk_kilidi", SITE_LOCK, false);
    uint16_t b = RegisterSite("kuyruk_kilidi", SITE_DEADLINE, false);
    uint16_t c = RegisterSite("", SITE_LOCK, false);
    assert(a == base + 1 && a2 == a && b == a + 1 && c == b + 1 && a != 0); checks += 5;
    assert(ProfilerSiteName(c) == "<unnamed>"); checks++;
    assert(g_site_count.load() == base + 3); checks++;

    // --- 3) kisaltma: kullanici adi bastan, otomatik ad SONDAN (':satir' korunur) ---
    const uint64_t tr0 = g_site_truncated.load();
    std::string longuser(300, 'u');
    uint16_t lu = RegisterSite(longuser.c_str(), SITE_LOCK, false);
    assert(ProfilerSiteName(lu).size() == kMaxSiteName && g_site_truncated.load() == tr0 + 1); checks += 2;
    std::string longauto = "d1/" + std::string(200, 'p') + "/f.cpp:777";
    uint16_t la = RegisterSite(longauto.c_str(), SITE_LOCK, true);
    std::string an = ProfilerSiteName(la);
    assert(an.size() <= kMaxSiteName); checks++;
    assert(an.size() >= 10 && an.substr(an.size() - 10) == "/f.cpp:777"); checks++;   // ':satir' ve dosya adi KORUNDU (isim SONDAN kisaltildi)

    // --- 4) ESZAMANLI kayit: 16 thread ayni 200 ismi FARKLI sirada kaydeder ---
    {
        const uint16_t before = g_site_count.load();
        std::vector<std::string> names; for (int i = 0; i < 200; ++i) names.push_back("es_" + std::to_string(i));
        std::vector<std::vector<uint16_t>> got(16, std::vector<uint16_t>(200, 0));
        std::vector<std::thread> ts;
        for (int t = 0; t < 16; ++t)
            ts.emplace_back([&, t] {
                std::vector<int> order(200); for (int i = 0; i < 200; ++i) order[i] = i;
                std::mt19937 rng(t * 7919 + 1); std::shuffle(order.begin(), order.end(), rng);
                for (int i : order) got[t][i] = RegisterSite(names[i].c_str(), SITE_LOCK, false);
            });
        for (auto& th : ts) th.join();
        std::vector<uint16_t> ids0 = got[0];
        for (int t = 1; t < 16; ++t) assert(got[t] == ids0);                       // her isim TEK id
        std::vector<uint16_t> u = ids0; std::sort(u.begin(), u.end());
        assert(std::unique(u.begin(), u.end()) == u.end());                        // idler benzersiz
        assert(g_site_count.load() == before + 200); checks += 3;
    }

    // --- 5) TANIM KODLAMA <-> COZME gidis-donusu ---
    {
        const std::vector<std::string> nm = {"x", std::string(11, 'a'), std::string(12, 'b'), std::string(13, 'c'), std::string(24, 'd'),
                                             std::string(kMaxSiteName, 'e'), "\xC5\x9F\xC4\x9F\xC3\xBC\xC4\xB1 UTF-8 isim", std::string("ikili\0bayt", 10)};
        for (size_t i = 0; i < nm.size(); ++i) RegisterSite(nm[i].c_str(), i % 2 ? SITE_DEADLINE : SITE_LOCK, false);   // not: '\0' iceren isim C-string'de kesilir
        const std::string path = ProfilerTestDir() + "site_roundtrip.bin";
        DirectWriter w; bool ok = w.Open(path, 1, true); assert(ok); (void)ok;
        EmitFileHeader(w);
        SiteDefEmitter em;
        em.EmitNew(w);
        const uint64_t after1 = w.stats().meta_records;
        em.EmitNew(w);                                                            // yeni site yok: hicbir sey yazmamali
        assert(w.stats().meta_records == after1); checks++;
        RegisterSite("sonradan_gelen_site", SITE_LOCK, false);
        em.EmitNew(w);                                                            // artimli: yalniz yeni tanim
        w.Finalize();
        DecodedFile d; assert(ParseProfilerFile(path, d));
        assert(d.header_seen && d.version == kProfilerFormatVersion); checks += 2;
        assert(d.duplicate_defs == 0 && d.unknown_records == 0 && d.warnings.empty() && d.size_multiple_of_16); checks += 4;
        assert(d.sites.size() == g_site_count.load()); checks++;
        uint64_t expected_meta = 1;
        for (auto& [id, ds] : d.sites) {
            assert(ds.complete && ds.name == ProfilerSiteName(id)); checks += 2;
            expected_meta += 1 + (ds.name.size() + 11) / 12;
        }
        assert(d.meta_records == expected_meta); checks++;
        assert(d.events.empty() && d.orphan_events == 0); checks++;
        std::remove(path.c_str());
        std::cout << "tanim gidis-donusu: " << d.sites.size() << " site, " << d.meta_records << " meta kaydi: OK\n";
    }

    // --- 6) BILGI: ilk kayit (soguk yol) maliyeti ---
    {
        std::vector<double> us;
        for (int i = 0; i < 300; ++i) {
            std::string nmx = "olcum_" + std::to_string(i);
            auto t0 = ProfilerClock::now(); RegisterSite(nmx.c_str(), SITE_LOCK, false);
            us.push_back(std::chrono::duration<double, std::micro>(ProfilerClock::now() - t0).count());
        }
        std::sort(us.begin(), us.end());
        std::cout << "BILGI: RegisterSite (soguk yol) medyan=" << us[us.size() / 2] << " us p90=" << us[us.size() * 9 / 10] << " us maks=" << us.back() << " us\n";
    }

    // --- 7) TASMA (SON test: tum id'leri tuketir) ---
    {
        const uint64_t of0 = g_site_overflow.load();
        uint16_t last_real = 0, first_overflow = 0;
        for (int i = 0; i < 5000; ++i) {
            uint16_t id = RegisterSite(("dolgu_" + std::to_string(i)).c_str(), SITE_LOCK, false);
            assert(id != 0 && id <= kMaxSiteId); checks += 2;
            if (id == kMaxSiteId) { if (!first_overflow) first_overflow = id; } else last_real = id;
        }
        assert(first_overflow == kMaxSiteId && last_real == kMaxSiteId - 1); checks += 2;
        assert(g_site_count.load() == kMaxSiteId && g_site_overflow.load() > of0); checks += 2;
        assert(ProfilerSiteName(kMaxSiteId) == "<site-table-overflow>"); checks++;
        assert(RegisterSite("bir_daha_yeni_isim", SITE_LOCK, false) == kMaxSiteId); checks++;   // tasma kalici: kovaya duser
        std::cout << "tasma: gercek id'ler 1.." << last_real << ", sonrasi " << kMaxSiteId << " kovasina; sayac=" << g_site_overflow.load() << ": OK\n";
    }
    std::cout << checks << " kontrol: TUM SITE TABLOSU TESTLERI BASARILI\n";
    return 0;
}
