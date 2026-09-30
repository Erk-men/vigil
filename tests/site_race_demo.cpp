// site_race_demo.cpp -- Onerilen makro deseni (atomik OLMAYAN static + kontrol-sonra-yaz) vs bizim desen.
// 64 bagimsiz "ilk gecis" yarisi: her denemede 8 thread AYNI cagri noktasindan AYNI ANDA ilk kez gecer.
//   NAIF : static uint16_t id = 0; if (id == 0) id = Register();   -> thread'ler FARKLI id alabilir + VERI YARISI (TSan)
//   BIZIM: PROFILER_SITE_ID (atomic relaxed + (tur,isim) tekillestirmeli RegisterSite) -> tek id, yarissiz
// Derleme: g++ -std=c++17 -O1 -g -pthread -Iinclude [-fsanitize=thread] tests/site_race_demo.cpp
#include "profiler_core.h"
#include <iostream>
#include <condition_variable>
#include <utility>

static constexpr int kTrials = 64, kThreads = 8;
static std::atomic<int> g_naive_regs[kTrials];

static uint16_t NaiveRegister(int trial) { return static_cast<uint16_t>(g_naive_regs[trial].fetch_add(1) + 1); }   // tekillestirme YOK: her cagri yeni id

template <int I> static uint16_t NaiveSite() {
    static uint16_t _local_site_id = 0;                          // ONERILEN desen: atomik degil
    if (__builtin_expect(_local_site_id == 0, 0)) {
        std::this_thread::yield();                               // yaris penceresini genislet (kontrol ile yazma arasi)
        _local_site_id = NaiveRegister(I);
    }
    return _local_site_id;
}
template <int I> static uint16_t OurSite() { return PROFILER_SITE_ID("race_demo_site", SITE_LOCK, false); }

template <int... Is> static std::vector<uint16_t (*)()> MakeNaive(std::integer_sequence<int, Is...>) { return {&NaiveSite<Is>...}; }
template <int... Is> static std::vector<uint16_t (*)()> MakeOurs(std::integer_sequence<int, Is...>) { return {&OurSite<Is>...}; }

struct Gate {                                                    // dogru baslangic tabancasi: durum mutex ALTINDA
    std::mutex m; std::condition_variable cv; bool go = false;
    void wait() { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return go; }); }
    void open() { { std::lock_guard<std::mutex> lk(m); go = true; } cv.notify_all(); }
};

// Bir denemede kThreads thread ayni fn'i cagirir; gorulen FARKLI id sayisini dondurur
static int RunTrial(uint16_t (*fn)(), std::vector<uint16_t>& ids) {
    Gate g; ids.assign(kThreads, 0);
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) ts.emplace_back([&, t] { g.wait(); ids[t] = fn(); });
    g.open();
    for (auto& th : ts) th.join();
    std::vector<uint16_t> u = ids; std::sort(u.begin(), u.end()); u.erase(std::unique(u.begin(), u.end()), u.end());
    return static_cast<int>(u.size());
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "both";    // both | naive | ours
    auto naive = MakeNaive(std::make_integer_sequence<int, kTrials>{});
    auto ours = MakeOurs(std::make_integer_sequence<int, kTrials>{});
    std::vector<uint16_t> ids;

    int naive_bad = 0, naive_multi_reg = 0; std::string sample;
    for (int t = 0; t < kTrials && mode != "ours"; ++t) {
        int distinct = RunTrial(naive[t], ids);
        if (distinct > 1) { naive_bad++; if (sample.empty()) { for (auto v : ids) sample += std::to_string(v) + " "; } }
        if (g_naive_regs[t].load() > 1) naive_multi_reg++;
    }
    int ours_bad = 0;
    for (int t = 0; t < kTrials && mode != "naive"; ++t) if (RunTrial(ours[t], ids) != 1) ours_bad++;
    size_t our_entries; { SiteTable& st = Sites(); std::lock_guard<std::mutex> lk(st.m); our_entries = st.sites.size() - 1; }

    std::cout << "NAIF desen (onerilen): " << kTrials << " denemenin " << naive_bad << "'inde thread'ler FARKLI id gordu, "
              << naive_multi_reg << "'inde ayni site BIRDEN COK kez kaydedildi";
    if (!sample.empty()) std::cout << "  (ornek: thread id'leri = " << sample << ")";
    std::cout << "\nBIZIM desen          : " << kTrials << " denemenin " << ours_bad << "'inde farkli id goruldu; kayit tablosunda bu isim icin "
              << our_entries << " giris (beklenen 1)\n";
    if (mode == "naive") return 0;
    return (ours_bad == 0 && our_entries == 1) ? 0 : 2;     // NAIF sonuc bilgi amaclidir; BIZIM desen dogru olmali
}
