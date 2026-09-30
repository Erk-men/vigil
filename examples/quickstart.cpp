// quickstart.cpp -- en kucuk kullanim ornegi. README'deki kod BURADAN alinmistir (derlenip calistirilir: run_all.sh 'example' adimi).
// Derleme:  g++ -std=c++17 -O2 -pthread -Iinclude examples/quickstart.cpp -o quickstart
// Calistir: ./quickstart trace.bin && ./profiler_decode trace.bin
#include "profiler_core.h"
#include <cstdio>

int main(int argc, char** argv) {
    ProfilerSession session;                             // surec basina TEK oturum
    ProfilerOptions opt;
    opt.path = argc > 1 ? argv[1] : "trace.bin";
    if (!session.Start(opt)) { std::perror("ProfilerSession::Start"); return 1; }

    std::mutex queue_mtx;
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w)
        workers.emplace_back([&] {
            ProfilerRegisterThread();                    // KURULUM: ring tahsisini sicak yoldan cikarir (~36 us -> thread baslangicina)
            for (int i = 0; i < 2000; ++i) {
                { PROFILE_LOCK_SCOPE_N(queue_mtx, lk, "queue_lock"); /* kritik bolum */ }
                { PROFILE_DEADLINE_N(200, "frame_work"); busy_wait_us(i % 5 == 0 ? 400 : 20); }   // her 5. is 200 us butceyi asar
            }
        });
    for (auto& t : workers) t.join();                    // uretici thread'leri Stop'tan ONCE durdur
    return session.Stop() ? 0 : 1;                       // son tahliye + kismi blok dolgusu + fdatasync + kapat
}
