// e2e_durable.cpp -- UCTAN UCA: makrolar (KIMLIKLI) -> ring -> registry -> flush -> O_DIRECT dosya -> cozucu ile dogrulama.
// Kimlik dogrulamasi: 2 kilit sitesi (biri kullanici adli "queue_lock" ve AYNI ISIM IKI FARKLI CAGRI NOKTASINDA,
// biri otomatik "dosya:satir") + 2 deadline sitesi (biri adli, biri otomatik). Her site icin profiler'DAN BAGIMSIZ
// zemin dogrulugu sayaci tutulur; cozulen site adlari + adetleri birebir eslesmeli. SIGKILL altinda "yetim olay
// (tanimi kendisinden once dosyada gelmeyen) = 0" sirasi da dogrulanir.
#include "profiler_core.h"
#include "profiler_reader.h"
#include <iostream>
#include <map>
#include <random>
#include <cassert>
#include <new>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>

static const std::string kOutPathStr = ProfilerTestDir() + "profiler_output_durable.bin";
static const char* kOutPath = kOutPathStr.c_str();

static std::atomic<int> g_line_auto_lock{0}, g_line_auto_deadline{0};   // makronun satirinda __LINE__ ile yazilir (ayni deger)

static bool EndsWith(const std::string& s, const std::string& suf) { return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0; }

static int RunPipeline() {
    const char* OUT_PATH = kOutPath;
    const int partial_ms = std::getenv("PROFILER_PARTIAL_MS") ? std::atoi(std::getenv("PROFILER_PARTIAL_MS")) : 100;
    const int sync_ms = std::getenv("PROFILER_SYNC_MS") ? std::atoi(std::getenv("PROFILER_SYNC_MS")) : 1000;
    const bool force_buffered = std::getenv("PROFILER_FORCE_BUFFERED") != nullptr;
    const size_t staging_blocks = std::getenv("PROFILER_STAGING_BLOCKS")
        ? static_cast<size_t>(std::atoi(std::getenv("PROFILER_STAGING_BLOCKS"))) : 4;

    DirectWriter writer;
    if (!writer.Open(OUT_PATH, staging_blocks, force_buffered)) { perror("DirectWriter::Open"); return 1; }
    std::cout << "I/O modu=" << (writer.mode() == DirectWriter::Mode::Direct ? "O_DIRECT" : "BUFFERED")
              << "  hizalama(statvfs)=" << writer.block_size()
              << "  staging=" << writer.staging_bytes() << " byte"
              << "  probe_errno=" << writer.probe_errno()
              << "  kismi_flush_ms=" << partial_ms << " sync_ms=" << sync_ms << "\n";

    std::atomic<bool> stop_flush{false};
    std::thread flusher(flush_thread_task, std::ref(stop_flush), std::ref(writer), partial_ms, sync_ms);

    std::mutex mtxA, mtxB;
    constexpr int NUM_WORKERS = 8;
    constexpr int ITERS_PER_WORKER = 2000;
    constexpr uint64_t DEADLINE_BUDGET_US = 1000;
    // ZEMIN DOGRULUGU (profiler'dan bagimsiz), SITE BAZINDA:
    std::atomic<uint64_t> gt_queue_lock{0}, gt_auto_lock{0}, gt_miss_named{0}, gt_miss_auto{0}, total_lock_ops{0}, total_deadline_calls{0};

    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_WORKERS; ++i) {
        workers.emplace_back([&, i] {
            std::mt19937 rng(1000 + i);
            std::uniform_int_distribution<int> dist_us(0, 3000);
            for (int j = 0; j < ITERS_PER_WORKER; ++j) {
                if (j % 2 == 0) { PROFILE_LOCK_SCOPE_N(mtxA, lk, "queue_lock"); busy_wait_us(5); gt_queue_lock++; }
                else { PROFILE_LOCK_SCOPE(mtxB, lk); g_line_auto_lock.store(__LINE__, std::memory_order_relaxed); busy_wait_us(5); gt_auto_lock++; }
                total_lock_ops++;
                if (j % 4 == 0) { PROFILE_LOCK_SCOPE_N(mtxA, lk2, "queue_lock"); gt_queue_lock++; total_lock_ops++; }   // AYNI ISIM, ikinci cagri noktasi => tek site

                int work_us = dist_us(rng);
                auto gt_start = ProfilerClock::now();
                if (j % 2 == 0) { PROFILE_DEADLINE_N(DEADLINE_BUDGET_US, "frame_even"); busy_wait_us(work_us); }
                else { PROFILE_DEADLINE(DEADLINE_BUDGET_US); g_line_auto_deadline.store(__LINE__, std::memory_order_relaxed); busy_wait_us(work_us); }
                auto gt_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ProfilerClock::now() - gt_start).count();
                if (gt_ns > static_cast<int64_t>(DEADLINE_BUDGET_US * 1000ULL)) (j % 2 == 0 ? gt_miss_named : gt_miss_auto)++;
                total_deadline_calls++;
            }
        });
    }
    for (auto& w : workers) w.join();

    while (true) {   // registry bosalana kadar (ring'ler tahliye edilip silinene kadar) bekle
        { std::lock_guard<std::mutex> lk(g_registry_mutex); if (g_registry.empty()) break; }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    stop_flush.store(true, std::memory_order_release);
    flusher.join();
    const bool fin_ok = writer.Finalize();

    const DirectWriter::Stats& st = writer.stats();
    std::cout << "Yazici istatistikleri: write_calls=" << st.write_calls << " bytes_written=" << st.bytes_written
              << " events_appended=" << st.events_appended << " meta_records=" << st.meta_records
              << " padding_records=" << st.padding_records << " kismi_flush=" << st.partial_flushes
              << " sync=" << st.sync_calls << " io_errors=" << st.io_errors << "\n";
    std::cout << "Uretim: kilit_islemi=" << total_lock_ops.load() << " deadline_cagrisi=" << total_deadline_calls.load()
              << " zemin_asim(adli)=" << gt_miss_named.load() << " zemin_asim(otomatik)=" << gt_miss_auto.load()
              << " dusen_event=" << g_dropped_total.load() << "\n";

    // ================= DOSYAYI COZUCU ILE BAGIMSIZ OLARAK OKU =================
    struct stat fst;
    if (::stat(OUT_PATH, &fst) != 0) { perror("stat"); return 1; }
    const size_t B = writer.block_size();
    DecodedFile d;
    if (!ParseProfilerFile(OUT_PATH, d)) { std::cerr << "dosya cozulemedi\n"; return 1; }

    // site adi -> (id, tur); beklenen 4 site
    std::map<std::string, uint16_t> by_name; std::map<uint16_t, uint8_t> kind_of;
    for (auto& [id, ds] : d.sites) { by_name[ds.name] = id; kind_of[id] = ds.kind; }
    std::string auto_lock_name, auto_deadline_name;
    for (auto& [name, id] : by_name) {
        if (kind_of[id] == SITE_LOCK && name != "queue_lock") auto_lock_name = name;
        if (kind_of[id] == SITE_DEADLINE && name != "frame_even") auto_deadline_name = name;
    }
    std::map<uint16_t, uint64_t> data_by_site[3];   // [tur][site]
    std::map<uint16_t, uint64_t> lock_by_thread; std::map<uint16_t, uint64_t> last_ts; bool mono = true;
    for (auto& e : d.events) {
        data_by_site[EventTypeOf(e)][EventSiteOf(e)]++;
        if (EventTypeOf(e) == EVENT_LOCK_WAIT) lock_by_thread[e.thread_id]++;
        if (e.timestamp < last_ts[e.thread_id]) mono = false;
        last_ts[e.thread_id] = e.timestamp;
    }
    auto count = [&](uint16_t type, const std::string& name) -> uint64_t { auto it = by_name.find(name); return it == by_name.end() ? 0 : data_by_site[type][it->second]; };
    const uint64_t n_queue = count(EVENT_LOCK_WAIT, "queue_lock"), n_auto_lock = count(EVENT_LOCK_WAIT, auto_lock_name);
    const uint64_t m_named = count(EVENT_DEADLINE_MISS, "frame_even"), m_auto = count(EVENT_DEADLINE_MISS, auto_deadline_name);

    std::cout << "\n--- Cozucu ile dosyadan geri okunan ---\n"
              << "  dosya_boyutu=" << fst.st_size << " (blok=" << B << ")  surum=" << d.version << "  toplam_kayit=" << d.total_records
              << " veri=" << d.events.size() << " meta=" << d.meta_records << " dolgu=" << d.padding_records << "\n"
              << "  tanimli site=" << d.sites.size() << ": ";
    for (auto& [name, id] : by_name) std::cout << "[" << id << "]" << name << (kind_of[id] == SITE_LOCK ? "(kilit) " : "(deadline) ");
    std::cout << "\n  kilit  queue_lock=" << n_queue << " (zemin " << gt_queue_lock.load() << ")  " << auto_lock_name << "=" << n_auto_lock << " (zemin " << gt_auto_lock.load() << ")\n"
              << "  asim   frame_even=" << m_named << " (zemin " << gt_miss_named.load() << ")  " << auto_deadline_name << "=" << m_auto << " (zemin " << gt_miss_auto.load() << ")\n"
              << "  yetim_olay=" << d.orphan_events << " cift_tanim=" << d.duplicate_defs << " bilinmeyen_kayit=" << d.unknown_records << " uyari=" << d.warnings.size() << "\n";
    std::cout.flush();

    // ===== INVARIANT'LAR: yazici / bicim =====
    assert(fin_ok); assert(st.io_errors == 0); assert(g_dropped_total.load() == 0);
    assert(static_cast<size_t>(fst.st_size) % B == 0);
    const uint64_t real_records = d.total_records - d.padding_records;               // veri + meta (dolgu haric)
    assert(static_cast<size_t>(fst.st_size) == ((real_records * sizeof(ProfilerEvent) + B - 1) / B) * B);
    assert(real_records == st.events_appended + st.meta_records);                    // dosyadaki = yazicinin yazdigi (veri + meta)
    assert(d.events.size() == st.events_appended);
    assert(d.padding_records == st.padding_records);
    assert(d.padding_records < B / sizeof(ProfilerEvent));
    assert(d.padding_only_at_tail);
    if (st.partial_flushes == 0) assert(st.bytes_written == static_cast<uint64_t>(fst.st_size));
    else { assert(st.bytes_written >= static_cast<uint64_t>(fst.st_size)); assert(st.bytes_written <= static_cast<uint64_t>(fst.st_size) + st.partial_flushes * B); }
    if (st.partial_flushes == 0) {
        uint64_t total_bytes = real_records * sizeof(ProfilerEvent);
        assert(st.write_calls == total_bytes / writer.staging_bytes() + ((total_bytes % writer.staging_bytes()) ? 1 : 0));
    }
    // ===== INVARIANT'LAR: KIMLIK =====
    assert(d.header_seen && d.version == kProfilerFormatVersion);
    assert(d.orphan_events == 0);                                                   // tanim, basvuran her olaydan ONCE dosyada
    assert(d.duplicate_defs == 0 && d.unknown_records == 0 && d.warnings.empty());
    assert(d.sites.size() == 4);                                                    // baska site OLMAMALI (ayni ad, iki cagri noktasi = TEK site)
    assert(by_name.count("queue_lock") && kind_of[by_name["queue_lock"]] == SITE_LOCK);
    assert(by_name.count("frame_even") && kind_of[by_name["frame_even"]] == SITE_DEADLINE);
    assert(!auto_lock_name.empty() && !auto_deadline_name.empty() && auto_lock_name != auto_deadline_name);
    assert(EndsWith(auto_lock_name, "e2e_durable.cpp:" + std::to_string(g_line_auto_lock.load())));         // __LINE__ TAM dogru
    assert(EndsWith(auto_deadline_name, "e2e_durable.cpp:" + std::to_string(g_line_auto_deadline.load())));
    assert(n_queue == gt_queue_lock.load());                                        // adli kilit: SIFIR kayip, birebir
    assert(n_auto_lock == gt_auto_lock.load());                                     // otomatik kilit: birebir
    assert(n_queue + n_auto_lock == total_lock_ops.load());
    for (int t = 0; t < 2; ++t) {                                                   // deadline: zemin dogrulugu ile ayni bant (site bazinda)
        int64_t diff = t == 0 ? (int64_t)m_named - (int64_t)gt_miss_named.load() : (int64_t)m_auto - (int64_t)gt_miss_auto.load();
        std::cout << "  deadline_miss farki (" << (t == 0 ? "frame_even" : "otomatik") << ": dosya - zemin dogrulugu)=" << diff << " (tolerans=5)\n";
        assert(diff >= -5 && diff <= 5);
    }
    assert(lock_by_thread.size() == static_cast<size_t>(NUM_WORKERS));
    for (auto& [tid, n] : lock_by_thread) { assert(n == 2500u); (void)tid; }        // 2000 ana + 500 ikinci cagri noktasi
    assert(mono);
    assert(g_registry.empty());

    std::cout << "\nTUM INVARIANT'LAR BASARILI (mod=" << (writer.mode() == DirectWriter::Mode::Direct ? "O_DIRECT" : "BUFFERED") << ")\n";
    return 0;
}

// =====================================================================
// COKME TESTI: cocuk surecte boru hatti calisir, ebeveyn SIGKILL atar (yakalanamaz: hicbir handler/Finalize calismaz).
// Kalan dosya cozucu ile okunur: kayip olcumu + KIMLIK SIRASI (yetim olay = 0: tanim, basvuran olaydan once yazilmis).
// NOT: SIGKILL, isletim sisteminin sayfa onbellegini ve cihaza inmis veriyi ETKILEMEZ; elektrik kesintisini dogrulamaz.
// =====================================================================
static int CrashTestMain(int kill_ms) {
    auto* shared = static_cast<std::atomic<uint64_t>*>(
        mmap(nullptr, sizeof(std::atomic<uint64_t>), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared == MAP_FAILED) { perror("mmap"); return 1; }
    new (shared) std::atomic<uint64_t>(0);
    ::unlink(kOutPath);

    pid_t pid = ::fork();   // henuz hic thread yok: fork guvenli
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        g_test_pushed = shared;
        if (!std::freopen("/dev/null", "w", stdout)) _exit(3);
        _exit(RunPipeline());
    }
    ::usleep(static_cast<useconds_t>(kill_ms) * 1000);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    const bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    const uint64_t pushed = shared->load();

    struct stat fst;
    if (::stat(kOutPath, &fst) != 0) { std::cout << "cikti dosyasi yok\n"; return 1; }
    const size_t B = QueryIoAlignment(kOutPath);
    DecodedFile d;
    if (!ParseProfilerFile(kOutPath, d)) { std::cout << "dosya cozulemedi\n"; return 1; }
    bool mono = true; std::map<uint16_t, uint64_t> last_ts;
    for (auto& e : d.events) { if (e.timestamp < last_ts[e.thread_id]) mono = false; last_ts[e.thread_id] = e.timestamp; }

    const uint64_t real = d.events.size();
    const uint64_t lost = pushed > real ? pushed - real : 0;
    const double rate = static_cast<double>(pushed) / (kill_ms / 1000.0);
    const char* pm = std::getenv("PROFILER_PARTIAL_MS");
    std::cout << "kismi_flush_ms=" << (pm ? pm : "100(varsayilan)")
              << " kill_ms=" << kill_ms << " SIGKILL_ile_oldu=" << (killed ? "evet" : "HAYIR(surec kendi bitti)")
              << " | uretilen=" << pushed << " kalici=" << real << " KAYIP=" << lost
              << " (~" << static_cast<int>(rate > 0 ? lost / rate * 1000.0 : 0) << " ms'lik veri)"
              << " | dosya=" << fst.st_size << " dolgu=" << d.padding_records
              << " blok_kati=" << (fst.st_size % B == 0 ? "evet" : "HAYIR")
              << " dolgu_sadece_kuyrukta=" << (d.padding_only_at_tail ? "evet" : "HAYIR")
              << " thread_ici_sira=" << (mono ? "ok" : "BOZUK")
              << " basliktan_surum=" << (d.header_seen ? d.version : 0) << " tanimli_site=" << d.sites.size()
              << " YETIM_OLAY=" << d.orphan_events << "\n";
    const bool format_ok = real <= pushed && fst.st_size % B == 0 && d.padding_only_at_tail && mono
            && d.header_seen && d.orphan_events == 0 && d.duplicate_defs == 0 && d.unknown_records == 0;
    if (!killed) {
        // Surec SIGKILL'den ONCE kendi bitirmis: bu kill_ms degeri, bu donanimda/bu is yukunde cokme
        // senaryosunu hic TETIKLEMEDI (yavas degil, hizli makine). Bu bir hata degil, testin bu SATIRI
        // uygulanabilir degil demek -- ama normal kapanista hicbir kayip OLMAMALI, onu sikica dogrula.
        return (format_ok && lost == 0) ? 3 : 2;   // 3 = "uygulanamaz" (run_all.sh ayirt eder), 2 = gercek hata
    }
    return format_ok ? 0 : 2;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--crash-test") == 0) return CrashTestMain(std::atoi(argv[2]));
    return RunPipeline();
}
