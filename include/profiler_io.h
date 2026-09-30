// profiler_io.h -- ProfilerEvent formati + donanim-farkinda O_DIRECT yazici
//
// NOT: O_DIRECT icin _GNU_SOURCE gerekir (g++ varsayilan olarak tanimlar).
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>

// Test yardimcisi: test dosyalarinin yazilacagi dizin. PROFILER_TEST_DIR ortam degiskeni, yoksa gecerli dizin.
inline std::string ProfilerTestDir() {
    const char* d = std::getenv("PROFILER_TEST_DIR");
    std::string s = d ? d : ".";
    if (s.empty() || s.back() != '/') s += '/';
    return s;
}

// ---------------------------------------------------------------------
// Olay formati (16 byte, sifir padding) -- daha once benchmark'la kanitlandi
// ---------------------------------------------------------------------
struct ProfilerEvent {
    uint64_t timestamp;
    uint16_t thread_id;
    uint16_t event_type;
    uint32_t payload;
};
static_assert(sizeof(ProfilerEvent) == 16, "ProfilerEvent 16 byte olmali");

// event_type alani 16 bit: DUSUK 4 bit = TUR (0..15), YUKSEK 12 bit = CAGRI NOKTASI KIMLIGI (site id, 0..4095).
// Boyut 16 byte KALIR (24 byte'a cikmak olculen %64 cache-line-bolunme cezasi getiriyordu).
// site 0 = "kimliksiz". Dolgu (tur 0) tamamen sifir kayittir.
enum EventType : uint16_t {
    EVENT_PADDING = 0,        // NOP: bloku hizalamak icin dolgu, okuyucu atlar
    EVENT_LOCK_WAIT = 1,      // veri: payload = kilit bekleme suresi (ns), site = kilit kimligi
    EVENT_DEADLINE_MISS = 2,  // veri: payload = butce asimi (ns), site = bolge kimligi
    EVENT_SITE_DEF = 3,       // META: site tanimi. site=id, thread_id=tur(SiteKind), payload=isim uzunlugu (bayt)
    EVENT_SITE_CHUNK = 4,     // META: isim parcasi. site=id, thread_id=parca no, timestamp(8B)+payload(4B) = 12 isim baytı
    EVENT_FILE_HEADER = 15,   // META: dosya basligi. payload = bicim surumu, timestamp = akis basladiginda now_ns()
};
constexpr uint32_t kProfilerFormatVersion = 2;
constexpr uint16_t kSiteBits = 12;
constexpr uint16_t kMaxSiteId = (1u << kSiteBits) - 1;          // 4095; bu deger TASMA kovasi icin ayrilmistir
constexpr uint16_t MakeTypeSite(uint16_t type, uint16_t site) { return static_cast<uint16_t>((site << 4) | (type & 0xF)); }
inline uint16_t EventTypeOf(const ProfilerEvent& e) { return static_cast<uint16_t>(e.event_type & 0xF); }
inline uint16_t EventSiteOf(const ProfilerEvent& e) { return static_cast<uint16_t>(e.event_type >> 4); }
inline bool IsDataEvent(uint16_t type) { return type == EVENT_LOCK_WAIT || type == EVENT_DEADLINE_MISS; }

// ---------------------------------------------------------------------
// Hizalama sorgusu: statvfs() -- ham blok cihazina erisim (ioctl) GEREKTIRMEZ,
// yani ayricaliksiz (unprivileged) surecte de calisir.
// Mantiksiz bir deger donerse guvenli varsayilan 4096.
// (2'nin kuvveti ve >=512 olan her deger 16'nin kati, yani event boyutuna bolunur.)
// ---------------------------------------------------------------------
inline size_t QueryIoAlignment(const std::string& file_path) {
    std::string dir = ".";
    size_t pos = file_path.find_last_of('/');
    if (pos != std::string::npos) dir = (pos == 0) ? "/" : file_path.substr(0, pos);
    struct statvfs vfs;
    if (statvfs(dir.c_str(), &vfs) != 0) return 4096;
    size_t b = static_cast<size_t>(vfs.f_bsize);
    bool pow2 = b != 0 && (b & (b - 1)) == 0;
    if (!pow2 || b < 512 || b > 65536) return 4096;
    return b;
}

// ---------------------------------------------------------------------
// DirectWriter: TEK thread (flush thread) tarafindan kullanilmak uzere.
//  - Open():     alignment sorgusu, aligned staging buffer, O_DIRECT probe,
//                basarisizsa sessizce buffered I/O'ya dusme
//  - Append():   event'leri staging buffer'a kopyalar, dolunca tek write()
//  - Finalize(): son kismi blogu event_type=0 kayitlariyla doldurup yazar,
//                fdatasync + close
// Dosya formati her iki modda da BAYT BAYT AYNI (kapanista her zaman blok
// siniri dolgusu yapilir) -- boylece okuyucu hangi modda yazildigini bilmek zorunda degil.
// ---------------------------------------------------------------------
class DirectWriter {
public:
    enum class Mode { None, Direct, Buffered }; // kapanistan SONRA da hangi modda calisildigi korunur

    struct Stats {
        uint64_t write_calls = 0;
        uint64_t bytes_written = 0;
        uint64_t events_appended = 0;   // yalniz VERI olaylari (Append)
        uint64_t meta_records = 0;      // baslik + site tanimlari (AppendMeta) -- events_appended'a KATILMAZ
        uint64_t padding_records = 0;   // dosyanin SON halindeki kuyruk dolgusu (yerinde yeniden yazimla ustune yazilir)
        uint64_t partial_flushes = 0;
        uint64_t sync_calls = 0;
        uint64_t io_errors = 0;
        uint64_t records_lost_to_io_error = 0;
        uint64_t max_write_ns = 0;     // tek bir WriteRange()'in en uzun suresi (flush thread'in bloke oldugu en uzun an)
        uint64_t total_write_ns = 0;
        uint64_t max_sync_ns = 0;
    };

    DirectWriter() = default;
    DirectWriter(const DirectWriter&) = delete;
    DirectWriter& operator=(const DirectWriter&) = delete;
    ~DirectWriter() {
        if (fd_ >= 0) ::close(fd_);
        std::free(staging_);
    }

    // force_buffered: test kancasi -- O_DIRECT'i hic denemeden buffered'a gec.
    bool Open(const std::string& path, size_t staging_blocks = 4, bool force_buffered = false) {
        block_ = QueryIoAlignment(path);
        staging_bytes_ = block_ * (staging_blocks ? staging_blocks : 1);
        if (posix_memalign(reinterpret_cast<void**>(&staging_), block_, staging_bytes_) != 0) {
            staging_ = nullptr;
            return false;
        }
        std::memset(staging_, 0, staging_bytes_);
        used_ = 0; clean_bytes_ = 0; file_offset_ = 0; dirty_ = false;

        if (!force_buffered) {
            int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
            if (fd < 0) {
                probe_errno_ = errno;               // ornegin EINVAL: dosya sistemi O_DIRECT desteklemiyor
            } else {
                // PROBE: hizali, sifir dolu bir blok yaz (=> gecerli bir dolgu blogu), sonra geri al
                ssize_t n = ::write(fd, staging_, block_);
                if (n == static_cast<ssize_t>(block_) && ::ftruncate(fd, 0) == 0 &&
                    ::lseek(fd, 0, SEEK_SET) == 0) {
                    fd_ = fd;
                    mode_ = Mode::Direct;
                } else {
                    probe_errno_ = (n < 0) ? errno : EIO; // open OK ama write EINVAL (overlayfs copy-up kose durumu)
                    ::close(fd);
                }
            }
        }
        if (fd_ < 0) {
            fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_ < 0) {
                std::free(staging_);
                staging_ = nullptr;
                return false;
            }
            mode_ = Mode::Buffered;
        }
        open_ = true;
        return true;
    }

    // Flush thread'den cagrilir: 'count' adet VERI olayini staging'e kopyalar.
    void Append(const ProfilerEvent* ev, size_t count) { AppendRaw(ev, count); stats_.events_appended += count; }

    // META kayitlari (dosya basligi, site tanimlari): ayni akisa yazilir ama events_appended'a katilmaz.
    void AppendMeta(const ProfilerEvent* ev, size_t count) { AppendRaw(ev, count); stats_.meta_records += count; }

    // Dayaniklilik: staging'deki henuz diske inmemis event'leri, kuyruk blogunu SIFIRLARLA doldurup
    // YERINDE (ayni offset'e) yazar. used_ sifirlanmaz; sonraki cagri ayni kuyruk blogunu, artik daha
    // fazla event iceren haliyle tekrar yazar. Tam dolan bloklar bir daha yazilmaz (clean_bytes_).
    // Boylece dolgu dosyanin ORTASINDA birikmez, sadece SONUNDA kalir.
    bool FlushPartial() {
        if (!open_ || !dirty_ || used_ == 0) return true;
        size_t end = ((used_ + block_ - 1) / block_) * block_;
        std::memset(staging_ + used_, 0, end - used_);      // bayat baytlar dolgu olarak diske gitmesin
        stats_.padding_records = (end - used_) / sizeof(ProfilerEvent);
        bool ok = WriteRange(clean_bytes_, end);
        if (ok) {
            clean_bytes_ = (used_ / block_) * block_;       // tam dolu bloklar artik nihai
            dirty_ = false;
            stats_.partial_flushes++;
        }
        return ok;
    }

    // Kalicilik: cihaz onbellegini bosaltmasini iste (garanti, cihazin flush'a uymasina baglidir).
    bool Sync() {
        if (!open_) return true;
        stats_.sync_calls++;
        const auto t0 = std::chrono::steady_clock::now();
        const int rc = ::fdatasync(fd_);
        const uint64_t dt = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
        if (dt > stats_.max_sync_ns) stats_.max_sync_ns = dt;
        if (rc != 0) { stats_.io_errors++; return false; }
        return true;
    }

    // Kapanis: kalan kismi yerinde yaz, kalici kil, kapat.
    bool Finalize() {
        if (!open_) return true;
        bool ok = FlushPartial();
        stats_.sync_calls++;
        ok = (::fdatasync(fd_) == 0) && ok;
        ok = (::close(fd_) == 0) && ok;
        fd_ = -1;
        open_ = false;
        return ok && stats_.io_errors == 0;
    }

    Mode mode() const { return mode_; }
    size_t block_size() const { return block_; }
    size_t staging_bytes() const { return staging_bytes_; }
    int probe_errno() const { return probe_errno_; }
    bool dirty() const { return dirty_; }
    const Stats& stats() const { return stats_; }

private:
    void AppendRaw(const ProfilerEvent* ev, size_t count) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(ev);
        size_t remaining = count * sizeof(ProfilerEvent);
        while (remaining > 0) {
            size_t room = staging_bytes_ - used_;
            size_t n = remaining < room ? remaining : room;
            std::memcpy(staging_ + used_, src, n);
            used_ += n;
            src += n;
            remaining -= n;
            dirty_ = true;
            if (used_ == staging_bytes_) {
                WriteRange(clean_bytes_, staging_bytes_);   // yalniz henuz diske inmemis kisim
                file_offset_ += staging_bytes_;
                used_ = 0;
                clean_bytes_ = 0;
                dirty_ = false;
                stats_.padding_records = 0;                 // dosya sonu artik tam dolu blok
            }
        }
    }

    // staging_[start, end) araligini dosyada file_offset_+start konumuna yazar (pwrite: acik offset)
    bool WriteRange(size_t start, size_t end) {
        const size_t want = end - start;
        size_t done = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (done < want) {
            ssize_t n = ::pwrite(fd_, staging_ + start + done, want - done,
                                 static_cast<off_t>(file_offset_ + start + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            done += static_cast<size_t>(n);
        }
        const uint64_t dt = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
        stats_.total_write_ns += dt;
        if (dt > stats_.max_write_ns) stats_.max_write_ns = dt;
        stats_.write_calls++;
        stats_.bytes_written += done;
        if (done != want) {
            stats_.io_errors++;
            stats_.records_lost_to_io_error += (want - done) / sizeof(ProfilerEvent);
            return false;
        }
        return true;
    }

    int fd_ = -1;
    Mode mode_ = Mode::None;
    bool open_ = false;
    size_t block_ = 4096;
    size_t staging_bytes_ = 0;
    size_t used_ = 0;
    size_t clean_bytes_ = 0;    // staging basindan itibaren diske NIHAI olarak inmis (tam dolu bloklar) bayt sayisi
    uint64_t file_offset_ = 0;  // staging'in dosyadaki baslangic offset'i (blok hizali)
    bool dirty_ = false;        // diske inmemis yeni veri var mi
    uint8_t* staging_ = nullptr;
    int probe_errno_ = 0;
    Stats stats_;
};
