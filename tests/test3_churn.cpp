// test3_churn.cpp -- Yuksek thread-churn stres testi ("Test 3") + TANI (watchdog).
//
// Senaryo: her iterasyonda bir flush thread + W worker thread; worker'lar bir "baslangic tabancasi" ile
// ayni anda serbest birakilir, registry'ye kayit olur, kapanir; ana thread registry'nin bosalmasini bekler.
//
// BASLANGIC TABANCASI (T3_START_GUN):
//   buggy : ORIJINAL kod. `go` mutex TUTULMADAN yazilir + notify_all. Predicate mutex altinda
//           degerlendirildigi icin KAYIP UYANDIRMA (lost wakeup) riski var: bir worker predicate'i false gorup
//           wait'e girmeden once bildirim gelirse, o worker sonsuza kadar bekler ve join() asilir.
//   fixed : `go` mutex ALTINDA yazilir (varsayilan). Atomik degisken de olsa, bekleyenin predicate'iyle
//           tutarli olmasi icin mutex altinda degistirilmelidir.
//
// T3_WIDEN_RACE=1 : predicate false donmeden hemen once yield() yapar => yaris penceresini genisletir.
//                   Hatanin MEKANIZMASINI sansa birakmadan gostermek icin (buggy + widen => hizla asilir).
// T3_STALL_MS     : bu kadar ms hicbir ilerleme olmazsa watchdog thread durumu dokup cikar (exit 3).
// T3_FLUSH_SLEEP_US: flush thread dongusune uyku ekler (varsayilan 0 = orijinal, uykusuz/CPU yiyen).
//
// Derleme:  g++ -std=c++17 -O2 -pthread [-DENABLE_CHAOS_TESTING] [-fsanitize=thread] test3_churn.cpp -o test3_churn
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <unistd.h>

struct RingBuffer {
    std::atomic<bool> closing{false};
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
};

static std::mutex registry_mutex;
static std::vector<RingBuffer*> global_registry;

#ifdef ENABLE_CHAOS_TESTING
#define CHAOS_YIELD() std::this_thread::yield()
#else
#define CHAOS_YIELD()
#endif

static std::mutex start_mtx;
static std::condition_variable start_cv;
static std::atomic<bool> go{false};

// ---- tani durumu ----
static std::atomic<int> g_phase{0}, g_iter{0};
static std::atomic<uint64_t> g_progress{0};
static std::atomic<int> g_spawned{0}, g_started{0}, g_done{0};
static bool g_buggy_gun = false, g_widen = false;
static int g_flush_sleep_us = 0;

static int envi(const char* n, int d) { const char* v = std::getenv(n); return v ? std::atoi(v) : d; }

static void worker_thread_task() {
    g_started.fetch_add(1);
    {
        std::unique_lock<std::mutex> lck(start_mtx);
        start_cv.wait(lck, [] {
            bool g = go.load(std::memory_order_acquire);
            if (!g && g_widen) std::this_thread::yield();   // yaris penceresini genislet (predicate false donmeden hemen once)
            return g;
        });
    }
    RingBuffer* buf = new RingBuffer();
    { std::lock_guard<std::mutex> lk(registry_mutex); global_registry.push_back(buf); }
    CHAOS_YIELD();
    buf->head.store(10, std::memory_order_release);
    CHAOS_YIELD();
    buf->closing.store(true, std::memory_order_release);
    g_done.fetch_add(1);
}

static void flush_thread_task(std::atomic<bool>& stop_flush) {
    while (!stop_flush.load(std::memory_order_acquire)) {
        std::vector<RingBuffer*> snapshot;
        { std::lock_guard<std::mutex> lk(registry_mutex); snapshot = global_registry; }
        CHAOS_YIELD();
        for (RingBuffer* buf : snapshot) {
            if (buf->closing.load(std::memory_order_acquire)) {
                buf->tail.store(buf->head.load(std::memory_order_acquire), std::memory_order_release);
                if (buf->head.load(std::memory_order_acquire) == buf->tail.load(std::memory_order_acquire)) {
                    {   // kilit ALTINDA erase (swap-and-pop), kilit DISINDA delete
                        std::lock_guard<std::mutex> lk(registry_mutex);
                        auto it = std::find(global_registry.begin(), global_registry.end(), buf);
                        if (it != global_registry.end()) { std::swap(*it, global_registry.back()); global_registry.pop_back(); }
                    }
                    CHAOS_YIELD();
                    delete buf;
                    g_progress.fetch_add(1);
                }
            }
        }
        if (g_flush_sleep_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(g_flush_sleep_us));
    }
}

static void RunScenario(int num_workers) {
    std::atomic<bool> stop_flush{false};
    g_spawned.store(0); g_started.store(0); g_done.store(0);
    go.store(false, std::memory_order_release);
    g_phase = 1; g_progress++;
    std::thread flusher(flush_thread_task, std::ref(stop_flush));
    std::vector<std::thread> workers;
    for (int i = 0; i < num_workers; ++i) { workers.emplace_back(worker_thread_task); g_spawned++; }
    g_phase = 2; g_progress++;
    if (g_buggy_gun) {
        go.store(true, std::memory_order_release);                 // ORIJINAL: mutex TUTULMADAN
        start_cv.notify_all();
    } else {
        { std::lock_guard<std::mutex> lk(start_mtx); go.store(true, std::memory_order_release); }   // DUZELTILMIS
        start_cv.notify_all();
    }
    g_phase = 3; g_progress++;
    for (auto& w : workers) { w.join(); g_progress++; }
    g_phase = 4; g_progress++;
    while (true) {
        { std::lock_guard<std::mutex> lk(registry_mutex); if (global_registry.empty()) break; }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    g_phase = 5; g_progress++;
    stop_flush.store(true, std::memory_order_release);
    flusher.join();
    g_phase = 0; g_progress++;
}

// ---- watchdog: hicbir ilerleme yoksa thread'lerin durumunu dok ----
static std::string ReadSmall(const std::string& path) {
    std::ifstream f(path); std::string s, line;
    if (!f) return "(okunamadi)";
    std::getline(f, s);
    return s;
}
static void DumpState(long stalled_ms) {
    std::fprintf(stderr, "\n===== WATCHDOG: %ld ms boyunca ILERLEME YOK =====\n", stalled_ms);
    std::fprintf(stderr, "iterasyon=%d evre=%d (1=olustur 2=tabanca 3=join 4=drain-bekleme 5=flusher-join) go=%d\n",
                 g_iter.load(), g_phase.load(), go.load() ? 1 : 0);
    std::fprintf(stderr, "worker: olusturulan=%d basladi=%d bitti=%d  (basladi-bitti=%d hala bitmemis)\n",
                 g_spawned.load(), g_started.load(), g_done.load(), g_started.load() - g_done.load());
    if (registry_mutex.try_lock()) { std::fprintf(stderr, "registry boyutu=%zu\n", global_registry.size()); registry_mutex.unlock(); }
    else std::fprintf(stderr, "registry kilidi ALINAMADI (biri tutuyor)\n");
    int futex_waiters = 0, running = 0, total = 0;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string base = std::string("/proc/self/task/") + e->d_name + "/";
            std::string stat = ReadSmall(base + "stat");
            size_t rp = stat.rfind(')');
            char state = (rp != std::string::npos && rp + 2 < stat.size()) ? stat[rp + 2] : '?';
            std::string wchan = ReadSmall(base + "wchan"), sc = ReadSmall(base + "syscall");
            total++;
            if (state == 'R') running++;
            if (wchan.find("futex") != std::string::npos) futex_waiters++;
            if (total <= 12)
                std::fprintf(stderr, "  tid=%s durum=%c wchan=%s syscall=%.40s\n", e->d_name, state, wchan.c_str(), sc.c_str());
        }
        closedir(d);
    }
    std::fprintf(stderr, "toplam thread=%d, CPU'da calisan(R)=%d, futex'te bekleyen=%d\n", total, running, futex_waiters);
    if (go.load() && g_phase.load() == 3 && g_started.load() > g_done.load())
        std::fprintf(stderr, "IMZA: go=TRUE ama %d worker hala tamamlanmadi ve join'de bekleniyor => bu worker(lar) "
                             "`go` degistikten sonra bile UYANMADI: KAYIP UYANDIRMA (lost wakeup) imzasi.\n",
                     g_started.load() - g_done.load());
    std::fflush(stderr);
}

int main() {
    const int W = envi("T3_WORKERS", 100), N = envi("T3_ITERS", 200), stall_ms = envi("T3_STALL_MS", 20000);
    const char* gun = std::getenv("T3_START_GUN");
    g_buggy_gun = gun && std::strcmp(gun, "buggy") == 0;
    g_widen = envi("T3_WIDEN_RACE", 0) != 0;
    g_flush_sleep_us = envi("T3_FLUSH_SLEEP_US", 0);
    std::printf("Test 3: worker=%d iterasyon=%d tabanca=%s yaris_penceresi_genis=%d flush_uyku_us=%d watchdog=%dms\n",
                W, N, g_buggy_gun ? "buggy(orijinal)" : "fixed", g_widen ? 1 : 0, g_flush_sleep_us, stall_ms);
    std::fflush(stdout);

    std::thread([stall_ms] {
        uint64_t last = g_progress.load();
        auto last_t = std::chrono::steady_clock::now();
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            uint64_t p = g_progress.load();
            auto now = std::chrono::steady_clock::now();
            if (p != last) { last = p; last_t = now; continue; }
            long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_t).count());
            if (ms >= stall_ms) { DumpState(ms); _exit(3); }
        }
    }).detach();

    std::vector<double> it_ms;
    const auto t_all0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        g_iter = i;
        const auto t0 = std::chrono::steady_clock::now();
        RunScenario(W);
        it_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    const double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_all0).count();
    std::vector<double> sorted = it_ms; std::sort(sorted.begin(), sorted.end());
    size_t argmax = static_cast<size_t>(std::max_element(it_ms.begin(), it_ms.end()) - it_ms.begin());
    size_t reg; { std::lock_guard<std::mutex> lk(registry_mutex); reg = global_registry.size(); }
    std::printf("TAMAMLANDI: toplam=%.2f sn | iterasyon suresi ms: medyan=%.1f p99=%.1f maks=%.1f (iterasyon %zu) | registry=%zu\n",
                total_s, sorted[sorted.size() / 2], sorted[std::min(sorted.size() - 1, static_cast<size_t>(0.99 * sorted.size()))],
                sorted.back(), argmax, reg);
    return reg == 0 ? 0 : 2;
}
