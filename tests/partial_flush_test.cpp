#include "profiler_io.h"
#include <vector>
#include <random>
#include <iostream>
#include <fstream>
#include <iterator>
#include <cassert>

static ProfilerEvent MakeEvent(uint64_t i) {
    ProfilerEvent e;
    e.timestamp = i * 7 + 1;
    e.thread_id = static_cast<uint16_t>(i % 5);
    e.event_type = static_cast<uint16_t>(1 + (i % 2));   // asla 0 (dolgu) degil
    e.payload = static_cast<uint32_t>(i * 2654435761u);
    return e;
}

static std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Referans: HIC kismi flush yapmadan ayni event'leri yazar
static std::vector<uint8_t> Reference(const std::string& path, uint64_t N, size_t staging_blocks) {
    DirectWriter w;
    bool ok = w.Open(path, staging_blocks, /*force_buffered=*/true);
    assert(ok); (void)ok;
    for (uint64_t i = 0; i < N; ++i) { ProfilerEvent e = MakeEvent(i); w.Append(&e, 1); }
    w.Finalize();
    return ReadAll(path);
}

// "Cokme anindaki" disk durumunu dogrula: ilk `total` kayit birebir event, kalani TAMAMEN sifir dolgu.
static void CheckOnDiskState(const std::string& path, uint64_t total, size_t B) {
    std::vector<uint8_t> bytes = ReadAll(path);
    size_t expected = ((total * sizeof(ProfilerEvent) + B - 1) / B) * B;
    assert(bytes.size() == expected);
    const ProfilerEvent* ev = reinterpret_cast<const ProfilerEvent*>(bytes.data());
    for (uint64_t k = 0; k < total; ++k) {
        ProfilerEvent want = MakeEvent(k);
        assert(std::memcmp(&ev[k], &want, sizeof(want)) == 0);
    }
    for (size_t k = total; k < bytes.size() / sizeof(ProfilerEvent); ++k) {
        assert(ev[k].event_type == EVENT_PADDING && ev[k].timestamp == 0 && ev[k].payload == 0);  // dolgu SADECE kuyrukta
    }
}

int main() {
    const std::string dir = ProfilerTestDir();
    const uint64_t Ns[] = {0, 1, 300, 1000, 5000};
    const size_t staging_options[] = {1, 4};
    const unsigned seeds[] = {1, 2, 3};
    int cases = 0; uint64_t checks = 0, max_partial = 0;
    double worst_amp = 0;

    for (size_t sb : staging_options) for (uint64_t N : Ns) {
        std::vector<uint8_t> ref = Reference(dir + "pf_ref.bin", N, sb);
        for (bool force_buffered : {false, true}) for (unsigned seed : seeds) {
            DirectWriter w;
            bool ok = w.Open(dir + "pf_test.bin", sb, force_buffered);
            assert(ok); (void)ok;
            const size_t B = w.block_size();
            std::mt19937 rng(seed * 1000 + N);
            uint64_t appended = 0;
            while (appended < N) {
                int action = rng() % 10;
                if (action < 7) {                                   // Append (1..300 event)
                    uint64_t c = std::min<uint64_t>(1 + rng() % 300, N - appended);
                    std::vector<ProfilerEvent> chunk;
                    for (uint64_t k = 0; k < c; ++k) chunk.push_back(MakeEvent(appended + k));
                    w.Append(chunk.data(), chunk.size());
                    appended += c;
                } else if (action < 9) {                            // FlushPartial + CIKIS ANI DURUMUNU DOGRULA
                    bool fp = w.FlushPartial();
                    assert(fp); (void)fp;
                    CheckOnDiskState(dir + "pf_test.bin", appended, B);
                    checks++;
                } else {
                    bool sy = w.Sync();
                    assert(sy); (void)sy;
                }
            }
            bool fp = w.FlushPartial(); assert(fp); (void)fp;      // bitmeden hemen once de bir kez
            CheckOnDiskState(dir + "pf_test.bin", appended, B);
            checks++;
            bool fin = w.Finalize(); assert(fin); (void)fin;

            // NIHAI dosya, hic kismi flush yapilmamis referansla BAYT BAYT AYNI olmali
            std::vector<uint8_t> got = ReadAll(dir + "pf_test.bin");
            assert(got == ref);
            assert(w.stats().io_errors == 0);
            assert(w.stats().events_appended == N);
            // Yeniden yazma sinirli: her kismi flush en fazla BIR kuyruk blogu fazladan yazar
            assert(w.stats().bytes_written <= got.size() + w.stats().partial_flushes * B);
            assert(w.stats().bytes_written >= got.size());
            if (!got.empty()) {
                double amp = static_cast<double>(w.stats().bytes_written) / static_cast<double>(got.size());
                if (amp > worst_amp) worst_amp = amp;
            }
            if (w.stats().partial_flushes > max_partial) max_partial = w.stats().partial_flushes;
            cases++;
        }
    }
    std::remove((dir + "pf_ref.bin").c_str());
    std::remove((dir + "pf_test.bin").c_str());
    std::cout << cases << " rastgele senaryo (O_DIRECT + buffered, 2 staging boyutu, 3 seed), "
              << checks << " 'cokme-ani disk durumu' dogrulamasi\n"
              << "en cok kismi flush (tek senaryoda)=" << max_partial
              << "  en kotu yazma amplifikasyonu (yazilan/nihai boyut)=" << worst_amp << "\n"
              << "TUM KISMI FLUSH TESTLERI BASARILI\n";
    return 0;
}
