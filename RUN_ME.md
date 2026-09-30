# Vigil — çalıştırma talimatı

Bu paket, geliştirme sırasında yalnızca **tek çekirdekli bir sanal sandbox'ta** çalıştırılabilen testleri
kendi makinende çalıştırmak için hazırlandı. Amaç: burada ölçülemeyenleri (çok çekirdekte kilit çekişmesi,
gerçek donanım, gerçek depolama) ölçmek ve burada yaptığımız ölçümleri başka bir makinede **tekrarlamak**.

## Gereksinimler

- Linux (x86-64 ya da ARM64). Windows'ta WSL2 kullan, **`/mnt/c` altında değil**, Linux dosya sisteminde
  (`~/`) çalıştır; macOS desteklenmiyor (`O_DIRECT`, `pwrite`, `/proc`, `LD_PRELOAD`, glibc kullanılıyor).
- `g++` (C++17), `gcc`, `bash`, `timeout` (coreutils). Sanitizer'lar (ASan/TSan/UBSan) varsa kullanılır,
  yoksa o adımlar **atlanır** (betik önce her birini derleyip çalıştırarak yoklar).
- 32-bit ARM'da `atomic<uint64_t>` için link hatası çıkarsa: `EXTRA_LIBS=-latomic`.
- `mallinfo2` için glibc ≥ 2.33 gerekir; eski glibc'de otomatik olarak `mallinfo`'ya düşer.

## Çalıştırma

```bash
bash run_all.sh                          # MODE=quick, ~1-2 dk
MODE=full bash run_all.sh                # tüm sanitizer'lar, çökme testleri, yavaş depolama, tekrarlı ölçümler; 10-20 dk
ONLY=test3,bench MODE=full bash run_all.sh   # yalnız seçili adımlar: unit, e2e, example, test3, slow, mem, bench
```

**Ortam değişkenleri `taskset`'ten ÖNCE yazılır.** `taskset -c 0-3 MODE=full bash run_all.sh` **çalışmaz** (`MODE=full` bir program sanılır,
exit 127). Doğrusu: `MODE=full taskset -c 0,2,4,6 bash run_all.sh` (ya da `taskset -c 0,2,4,6 env MODE=full bash run_all.sh`).

## Donanım koşusu: sonuçların geçerli olması için 3 koşul

1. **Farklı fiziksel çekirdekler.** SMT (hyperthreading) kardeşleri **L1 önbelleği paylaşır**; kardeşler arasında çekirdekler-arası L1 tutarlılık trafiği
   oluşmaz. `lscpu -e` çıktısında **CORE sütunu farklı** CPU'ları seç (ör. `taskset -c 0,2,4,6`). Betik, izinli CPU'lar SMT kardeşi içeriyorsa uyarır.
   Raspberry Pi (4 çekirdek, SMT yok) ve benzeri kartlarda `taskset -c 0-3` uygundur.
2. **Hedef depolama.** `OUT`, ölçmek istediğin SD/eMMC/SSD üzerinde olmalı: `OUT=/mnt/sd/results MODE=full bash run_all.sh`.
   tmpfs, OverlayFS, WSL `/mnt/c`, NFS, FUSE üzerinde `slow` ve `O_DIRECT` sonuçları gerçek depolama hakkında bir şey söylemez (betik uyarır).
   `env.txt`'deki "dosya sistemi" satırını kontrol et. Not: `slow` adımındaki "sürekli yavaş"/"takılma" senaryoları **shim ile yapay gecikme** enjekte eder;
   gerçek cihaz yavaşlığını gösteren, "hızlı depolama (taban çizgisi)" adımının sonucudur (`p99`, `dusen`).
3. **Sakin makine.** Ölçüm sırasında başka yük çalıştırma; mümkünse CPU governor'ı `performance` yap; Raspberry Pi'de termal kısılmayı (throttling) izle
   (`vcgencmd get_throttled`). Sonucu `env.txt`'deki `cpu governor` satırıyla birlikte yorumla.

## Ne paylaşılacak

`results/<host>-<zaman>/summary.txt` ve aynı dizindeki `env.txt` (metin olarak yapıştırabilirsin; ikisi de kısadır). Başarısız bir adım varsa
`results/.../logs/` altındaki ilgili `.log` dosyasını da ekle. Yorum yaparken `env.txt`'deki `nproc`,
`clocksource`, `cpu`, `sanallaştırma` satırları **gereklidir**: sonuçlar bunlara bağlı.

## Adımlar ne doğrular

| Adım | Doğruladığı |
|---|---|
| `unit` | Yazıcı (`O_DIRECT` ve buffered modda bayt-bayt aynı dosya), kısmi flush'ın "çökme anı" disk durumu, makro anlamı **ve kimlik** (40 kesin kontrol), site tablosu (eşzamanlı kayıt, taşma, tanım kodlama-çözme), site ilk-geçiş yarışı demosu |
| `e2e` | Makro → ring → registry → flush → dosya → **çözücü ile** geri okuma. 4 site (adlı/otomatik, aynı ad iki çağrı noktasında) için olaylar **site bazında zemin doğruluğuyla birebir** eşleşmeli; `SIGKILL` altında kalıcı kayıp ve **yetim olay = 0**; `O_DIRECT` arıza simülasyonu |
| `test3` | Thread doğum/ölüm stresi. **Düzeltilmiş** başlangıç tabancası ve, `full` modda, **orijinal hatalı** kodla karşılaştırma (aşağıya bak) |
| `slow` | Depolama yavaşlayınca worker'ın gördüğü `push_event` gecikmesi ve düşen olay muhasebesi |
| `mem` | Canlı thread başına ring belleği; thread çıkınca serbest kalıyor mu |
| `bench` | `PROFILE_LOCK_SCOPE` maliyeti: serpiştirilmiş A/B; çekişmesiz, dolu-ring ve (nproc ≥ 2 ise) çekişmeli |

## Çekişmeli ölçümü nasıl okumalı

`bench` çekişmeli fazı (nproc ≥ 2) `nvcsw` sütununu (gönüllü bağlam değiştirme / batch) da yazar.
**`nvcsw ≈ 0` ise mutex üzerinde fiilen bloke olunmamıştır: gerçek çekişme oluşmamıştır** ve sonuç, çekişmesiz
sonuçtan ayırt edilemez. Tek çekirdekli makinede bu beklenir. Gerçek çekişme için ≥ 2 çekirdek ve `nvcsw > 0` gerekir.
Çekişmeli sonuçlar farklı `BENCH_THREADS` değerleriyle (2, 4, çekirdek sayısı) karşılaştırılmalıdır.

## Test 3'te bulunan hata (bilmen gereken)

Orijinal Test 3'ün başlangıç tabancası `go` bayrağını **mutex tutmadan** yazıp `notify_all` çağırıyordu. Worker'lar
predicate'i mutex altında değerlendirdiği için bu, bir **kayıp uyandırma (lost wakeup)** yarışı: bildirim, worker
`wait`'e girmeden önce gelirse worker sonsuza kadar bekler ve `join()` asılır. Bu hata aralıklı çıkar (bizim
sandbox'ımızda TSan altında 100 worker × 150 iterasyonda ~%24/koşu, 42 koşuda 10 asılma). `T3_START_GUN=buggy`
orijinali, `fixed` düzeltilmişi çalıştırır; `T3_WIDEN_RACE=1` yarış penceresini genişleterek asılmayı hızla üretir.
Asılma olursa watchdog, thread durumlarını `/proc`'tan dökerek `KAYIP UYANDIRMA` imzasını basar. `full` modda betik
orijinal kodla da koşar: **0 asılma çıkması mümkündür ve hatanın yokluğu anlamına gelmez.**

## Olay kimliği (v2 biçimi)

Her olay 16 bayt kalır. `event_type` alanı **4 bit tür + 12 bit çağrı noktası kimliği (site)**:

| Tür | Anlam |
|---|---|
| 0 | dolgu (tamamen sıfır kayıt) |
| 1 `LOCK_WAIT` | veri: `payload` = bekleme (ns), `site` = kilit kimliği |
| 2 `DEADLINE_MISS` | veri: `payload` = bütçe aşımı (ns), `site` = bölge kimliği |
| 3 `SITE_DEF`, 4 `SITE_CHUNK` | meta: site adı (12 baytlık parçalar); **ring'den geçmez**, flush thread küresel tablodan yazar |
| 15 `FILE_HEADER` | meta: biçim sürümü (2) |

Makrolar: `PROFILE_LOCK_SCOPE(mtx, lk)` (otomatik ad `dosya:satır`), `PROFILE_LOCK_SCOPE_N(mtx, lk, "ad")`,
`PROFILE_DEADLINE(bütçe_us)`, `PROFILE_DEADLINE_N(bütçe_us, "ad")`. **Ad bir string literali olmalı.** Aynı ad birden çok
çağrı noktasında kullanılırsa tek site olarak toplanır. Sınırlar: 4094 site (sonrası `<site-table-overflow>` kovasına düşer
ve sayılır), ad en fazla 120 bayt (kullanıcı adı baştan, otomatik ad sondan kısaltılır). Tanım, kendisine başvuran her olaydan
**önce** dosyada gelir. Çözmek için: `tools/profiler_decode dosya.bin` (site bazında adet, toplam, ortalama, p50, p99, maks;
`--csv` ile ham olaylar).

Kimlik kaydı maliyeti: site tablosuna yalnızca bir çağrı noktasından **ilk geçişte** dokunulur (~0.3 µs, bu makinede) ve bu,
kilit alınmadan **önce** olur, yani kritik bölüme ve ölçülen bekleme süresine girmez. Sıcak yolda kimlik kontrolü ~0.5 ns.

## Bilinen eksikler (bu paket bunları kapsamaz)

- Elektrik kesintisi dayanıklılığı (`fdatasync` etkisi) test edilmedi; `SIGKILL` testi yalnızca süreç ölümünü kapsar.
- Ring boyutu derleme zamanı sabiti (`-DPROFILER_RING_SIZE`), çalışma zamanında thread başına seçilemez.
- İsimler yalnızca string literali olabilir (çalışma zamanında üretilen dinamik adlar desteklenmez).
- Bir çağrı noktası için 12 baytlık parçalarla en fazla 120 bayt ad; UTF-8 ortasından kesilebilir (okuyucu baytları olduğu gibi gösterir).
- Yalnızca glibc/Linux; `O_DIRECT` yalnızca ext4 üzerinde (sanal disk) ve `tmpfs`'te denendi. Biçim little-endian.
- Ölçümlerin hiçbiri gerçek SD/eMMC, ARM veya çok çekirdekli donanımda alınmadı; paket tam bunun içindir.
