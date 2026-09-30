// ring_memory_test.cpp -- Thread basina ring bellegi GERCEKTEN ne kadar, ve thread cikinca serbest kaliyor mu?
//  Ayni surecte uc evre: (1) N thread baslar ama KAYITSIZ, (2) hepsi ProfilerRegisterThread() cagirir,
//  (3) hepsi cikar; flush thread ring'leri bosaltip siler. Her evrede mallinfo2 (kullanimdaki bayt) ve VmRSS.
// Derleme: g++ -std=c++17 -O2 -pthread -I. [-DPROFILER_RING_SIZE=N] ring_memory_test.cpp -o ring_memory_test
#include "profiler_core.h"
#include <malloc.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <condition_variable>

static long VmRssKb() {
    std::ifstream f("/proc/self/status"); std::string line;
    while (std::getline(f, line)) if (line.rfind("VmRSS:", 0) == 0) { std::istringstream is(line.substr(6)); long kb; is >> kb; return kb; }
    return -1;
}
static long InUseBytes() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
    return static_cast<long>(mi.uordblks + mi.hblkhd);
#else
    struct mallinfo mi = mallinfo();      // eski glibc: 32-bit alanlar, cok buyuk heap'te tasabilir
    return static_cast<long>(mi.uordblks) + static_cast<long>(mi.hblkhd);
#endif
}

int main() {
    const int N = std::getenv("RING_MEM_THREADS") ? std::atoi(std::getenv("RING_MEM_THREADS")) : 256;
    const char* out = "./ring_mem.out.bin";

    DirectWriter writer;
    if (!writer.Open(out, 1, false)) { perror("Open"); return 1; }
    std::atomic<bool> stop_flush{false};
    std::thread flusher([&] { flush_thread_task(stop_flush, writer, 0, 0); });

    std::mutex m; std::condition_variable cv;
    int phase = 0;                              // 0: bekle, 1: kaydol, 2: cik
    std::atomic<int> started{0}, registered{0};
    std::vector<std::thread> ws;
    for (int i = 0; i < N; ++i)
        ws.emplace_back([&] {
            started.fetch_add(1);
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return phase >= 1; });
            lk.unlock();
            ProfilerRegisterThread();
            registered.fetch_add(1);
            lk.lock();
            cv.wait(lk, [&] { return phase >= 2; });
        });

    const long base_use = InUseBytes(), base_rss = VmRssKb();
    while (started.load() < N) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const long idle_use = InUseBytes(), idle_rss = VmRssKb();

    { std::lock_guard<std::mutex> lk(m); phase = 1; } cv.notify_all();
    while (registered.load() < N) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const long reg_use = InUseBytes(), reg_rss = VmRssKb();
    size_t reg_size; { std::lock_guard<std::mutex> lk(g_registry_mutex); reg_size = g_registry.size(); }

    { std::lock_guard<std::mutex> lk(m); phase = 2; } cv.notify_all();
    for (auto& w : ws) w.join();
    // flush thread'in cikan thread'lerin ring'lerini bosaltip SILMESINI bekle
    for (int i = 0; i < 5000; ++i) {
        { std::lock_guard<std::mutex> lk(g_registry_mutex); if (g_registry.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    size_t end_reg; { std::lock_guard<std::mutex> lk(g_registry_mutex); end_reg = g_registry.size(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const long end_use = InUseBytes(), end_rss = VmRssKb();

    stop_flush.store(true); flusher.join(); writer.Finalize(); ::unlink(out);

    const double per_use = static_cast<double>(reg_use - idle_use) / N;
    const double per_rss = static_cast<double>(reg_rss - idle_rss) * 1024.0 / N;
    const double freed = reg_use - idle_use > 0 ? 100.0 * static_cast<double>(reg_use - end_use) / static_cast<double>(reg_use - idle_use) : 0;
    std::cout << "RING_BOYUTU=" << RingBuffer::SIZE << " slot  sizeof(RingBuffer)=" << sizeof(RingBuffer) << " byte  N=" << N << " thread\n"
              << "  (1) kayitsiz, " << N << " thread bekliyor : mallinfo kullanim=" << idle_use << "  RSS=" << idle_rss << " KB (baslangic: " << base_use << " / " << base_rss << ")\n"
              << "  (2) hepsi kayitli (registry=" << reg_size << ")     : mallinfo kullanim=" << reg_use << "  RSS=" << reg_rss << " KB\n"
              << "      thread basina EK: mallinfo=" << per_use << " byte, RSS=" << per_rss << " byte\n"
              << "  (3) hepsi cikti + flush thread bosalti      : registry=" << end_reg << " mallinfo kullanim=" << end_use << "  RSS=" << end_rss << " KB\n"
              << "      ring baytlarinin serbest birakilan orani (mallinfo) = " << freed << " %\n";
    return end_reg == 0 ? 0 : 2;
}
