// html_report_test.cpp -- WriteHtmlReport() dogrulugu: kacis (escaping), rozetler, siralama, harici bagimlilik yoklugu.
// DecodedFile'i ELLE kurar (gercek .bin dosyasina ihtiyac yok) -- ozellikle uc durumlari hedeflemek icin.
#include "profiler_reader.h"
#include <sstream>

// SITE_LOCK/SITE_DEADLINE profiler_core.h icinde (bu test sadece profiler_reader.h icin, cekirdegi gerektirmez).
// DecodedSite::kind sadece bir uint8_t; testte kullandigimiz deger onemli degil, tutarli olmasi yeterli.
static constexpr uint8_t kSiteLockKind = 1;
#include <iostream>
#include <cassert>

static ProfilerEvent MkEvent(uint16_t type, uint16_t site, uint32_t payload) {
    ProfilerEvent e{};
    e.event_type = MakeTypeSite(type, site);
    e.payload = payload;
    return e;
}

int main() {
    int checks = 0;

    // ---- 1) HTML KACISI: site adinda < > & " gecerse ham HTML olarak sizmamali ----
    {
        DecodedFile d;
        d.header_seen = true; d.version = kProfilerFormatVersion;
        d.sites[1] = {"<script>alert(1)</script>&\"tehlikeli\"", kSiteLockKind, 0, 0, true};
        d.events.push_back(MkEvent(EVENT_LOCK_WAIT, 1, 1000));
        d.total_records = 1;

        std::ostringstream out;
        WriteHtmlReport(d, "test.bin", "simdi", out);
        std::string html = out.str();

        assert(html.find("<script>alert") == std::string::npos); checks++;      // HAM etiket sizmamis
        assert(html.find("&lt;script&gt;") != std::string::npos); checks++;     // kacisli hali VAR
        assert(html.find("&amp;") != std::string::npos); checks++;
        assert(html.find("&quot;") != std::string::npos); checks++;
        std::cout << "1) HTML kacisi: tehlikeli site adi guvenli sekilde kacisli: OK\n";
    }

    // ---- 2) YETIM OLAY rozeti: 0 iken "butunluk tamam", >0 iken "bad" rozeti ----
    {
        DecodedFile clean; clean.header_seen = true; clean.version = 2;
        std::ostringstream o1; WriteHtmlReport(clean, "a.bin", "t", o1);
        assert(o1.str().find("bütünlük tamam") != std::string::npos); checks++;
        assert(o1.str().find("badge bad") == std::string::npos); checks++;

        DecodedFile bad; bad.header_seen = true; bad.version = 2; bad.orphan_events = 7;
        std::ostringstream o2; WriteHtmlReport(bad, "b.bin", "t", o2);
        assert(o2.str().find("badge bad") != std::string::npos); checks++;
        assert(o2.str().find("7 yetim olay") != std::string::npos); checks++;
        std::cout << "2) yetim-olay rozeti: temiz/bozuk dosya dogru isaretleniyor: OK\n";
    }

    // ---- 3) BOS KATEGORI: deadline olayi hic yoksa "olay yok" mesaji, tablo YOK ----
    {
        DecodedFile d; d.header_seen = true; d.version = 2;
        d.sites[1] = {"tek_kilit", kSiteLockKind, 0, 0, true};
        d.events.push_back(MkEvent(EVENT_LOCK_WAIT, 1, 500));
        std::ostringstream out; WriteHtmlReport(d, "t.bin", "t", out);
        std::string html = out.str();
        size_t dl_section = html.find("Deadline aşımları");
        assert(dl_section != std::string::npos); checks++;
        assert(html.find("olay yok", dl_section) != std::string::npos); checks++;
        assert(html.find("<table>", dl_section) == std::string::npos); checks++;   // bos kategoride tablo basilmamali
        std::cout << "3) bos kategori: tablo yerine 'olay yok' mesaji: OK\n";
    }

    // ---- 4) SIRALAMA: en yuksek TOPLAM en ustte; cubuk genisligi en buyuge gore orantili ----
    {
        DecodedFile d; d.header_seen = true; d.version = 2;
        d.sites[1] = {"kucuk", kSiteLockKind, 0, 0, true};
        d.sites[2] = {"buyuk", kSiteLockKind, 0, 0, true};
        for (int i = 0; i < 10; ++i) d.events.push_back(MkEvent(EVENT_LOCK_WAIT, 1, 100));   // toplam 1000
        for (int i = 0; i < 10; ++i) d.events.push_back(MkEvent(EVENT_LOCK_WAIT, 2, 900));   // toplam 9000
        std::ostringstream out; WriteHtmlReport(d, "t.bin", "t", out);
        std::string html = out.str();
        size_t p_buyuk = html.find("buyuk"), p_kucuk = html.find("kucuk");
        assert(p_buyuk != std::string::npos && p_kucuk != std::string::npos && p_buyuk < p_kucuk); checks++;  // buyuk ONCE gelmeli
        assert(html.find("width:100.0%") != std::string::npos); checks++;                      // en buyuk site = %100
        std::cout << "4) siralama: en yuksek toplam ustte, en buyuk cubuk %100: OK\n";
    }

    // ---- 5) BASLIK BILGISI dogru yansitiliyor mu (surum, kayit sayilari) ----
    {
        DecodedFile d; d.header_seen = true; d.version = 2;
        d.total_records = 123; d.padding_records = 4; d.meta_records = 5;
        d.events.push_back(MkEvent(EVENT_LOCK_WAIT, 0, 1));
        std::ostringstream out; WriteHtmlReport(d, "kaynak.bin", "2026-01-01 00:00", out);
        std::string html = out.str();
        assert(html.find("kaynak.bin") != std::string::npos); checks++;
        assert(html.find("2026-01-01 00:00") != std::string::npos); checks++;
        assert(html.find("biçim sürümü 2") != std::string::npos); checks++;
        assert(html.find("123 kayıt") != std::string::npos); checks++;
        std::cout << "5) baslik bilgisi (kaynak/zaman/surum/sayimlar) dogru: OK\n";
    }

    // ---- 6) TAMAMEN CEVRIMDISI: hicbir harici kaynak referansi olmamali ----
    {
        DecodedFile d; d.header_seen = true; d.version = 2;
        d.sites[1] = {"x", kSiteLockKind, 0, 0, true};
        d.events.push_back(MkEvent(EVENT_LOCK_WAIT, 1, 1));
        std::ostringstream out; WriteHtmlReport(d, "t.bin", "t", out);
        std::string html = out.str();
        for (const char* forbidden : {"http://", "https://", "cdn.", "<script src", "@import", "googleapis"}) {
            assert(html.find(forbidden) == std::string::npos);
            checks++;
        }
        assert(html.find("<!doctype html>") == 0); checks++;                 // gecerli, tek parca belge
        assert(html.find("</html>") != std::string::npos); checks++;
        std::cout << "6) harici bagimlilik YOK (CDN/script/font referansi bulunamadi): OK\n";
    }

    std::cout << "\n" << checks << " kontrol: TUM HTML RAPOR TESTLERI BASARILI\n";
    return 0;
}
