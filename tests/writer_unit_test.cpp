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
    e.event_type = static_cast<uint16_t>(1 + (i % 2)); // hicbir zaman 0 (dolgu) degil
    e.payload = static_cast<uint32_t>(i * 2654435761u);
    return e;
}

static std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Yazar, dosyayi geri okur, icerigi tek tek dogrular. Ham dosya baytlarini dondurur.
static std::vector<uint8_t> RunCase(const std::string& path, uint64_t N, size_t staging_blocks,
                                    bool force_buffered, size_t& block_out,
                                    DirectWriter::Mode& mode_out) {
    DirectWriter w;
    bool opened = w.Open(path, staging_blocks, force_buffered);
    assert(opened);
    block_out = w.block_size();
    mode_out = w.mode();

    // Farkli boyutlu parcalarla ekle (staging siniri ile parca siniri ust uste bindirilsin)
    std::mt19937 rng(12345 + N);
    std::uniform_int_distribution<int> chunk_dist(1, 300);
    std::vector<ProfilerEvent> chunk;
    uint64_t i = 0;
    while (i < N) {
        uint64_t c = std::min<uint64_t>(chunk_dist(rng), N - i);
        chunk.clear();
        for (uint64_t k = 0; k < c; ++k) chunk.push_back(MakeEvent(i + k));
        w.Append(chunk.data(), chunk.size());
        i += c;
    }
    bool fin = w.Finalize();
    assert(fin);
    assert(w.stats().io_errors == 0);
    assert(w.stats().events_appended == N);

    std::vector<uint8_t> bytes = ReadAll(path);
    const size_t B = w.block_size();

    // --- Dosya boyutu: ceil(N*16 / B) * B ---
    size_t expected_size = ((N * sizeof(ProfilerEvent) + B - 1) / B) * B;
    assert(bytes.size() == expected_size);
    assert(bytes.size() % B == 0);
    assert(w.stats().bytes_written == bytes.size());

    // --- write() sayisi: dolu staging'ler + (varsa) tek kalan parca ---
    uint64_t total_bytes = N * sizeof(ProfilerEvent);
    uint64_t expected_calls = total_bytes / w.staging_bytes() + ((total_bytes % w.staging_bytes()) ? 1 : 0);
    assert(w.stats().write_calls == expected_calls);

    // --- Icerik: ilk N kayit birebir MakeEvent(i), kalanlar TAMAMEN sifir (dolgu) ---
    size_t records = bytes.size() / sizeof(ProfilerEvent);
    const ProfilerEvent* ev = reinterpret_cast<const ProfilerEvent*>(bytes.data());
    for (uint64_t k = 0; k < N; ++k) {
        ProfilerEvent expect = MakeEvent(k);
        assert(std::memcmp(&ev[k], &expect, sizeof(expect)) == 0);
    }
    uint64_t padding = 0;
    for (size_t k = N; k < records; ++k) {
        assert(ev[k].event_type == EVENT_PADDING);
        assert(ev[k].timestamp == 0 && ev[k].thread_id == 0 && ev[k].payload == 0);
        padding++;
    }
    assert(padding == w.stats().padding_records);
    assert(padding < B / sizeof(ProfilerEvent)); // dolgu asla bir blogu gecmez
    return bytes;
}

int main() {
    const std::string dir = ProfilerTestDir();
    const uint64_t Ns[] = {0, 1, 255, 256, 257, 1023, 1024, 1025, 4096, 10000};
    const size_t staging_options[] = {1, 4};
    int cases = 0;

    for (size_t sb : staging_options) {
        for (uint64_t N : Ns) {
            size_t block = 0;
            DirectWriter::Mode m_direct, m_buf;
            auto a = RunCase(dir + "unit_direct.bin", N, sb, /*force_buffered=*/false, block, m_direct);
            auto b = RunCase(dir + "unit_buffered.bin", N, sb, /*force_buffered=*/true, block, m_buf);
            assert(m_buf == DirectWriter::Mode::Buffered);
            // Iki modun urettigi dosya BAYT BAYT AYNI olmali
            assert(a == b);
            cases++;
            std::cout << "OK  N=" << N << " staging_blocks=" << sb << " block=" << block
                      << " direct_mode=" << (m_direct == DirectWriter::Mode::Direct ? "O_DIRECT" : "buffered(fallback)")
                      << " dosya_boyutu=" << a.size() << " (direct==buffered bayt bayt)\n";
        }
    }
    std::remove((dir + "unit_direct.bin").c_str());
    std::remove((dir + "unit_buffered.bin").c_str());
    std::cout << "\n" << cases << " durum x 2 mod: TUM BIRIM TESTLER BASARILI\n";
    return 0;
}
