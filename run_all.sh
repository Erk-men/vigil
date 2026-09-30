#!/usr/bin/env bash
# run_all.sh -- Vigil'i derler, tum testleri ve olcumleri calistirir, results/ altina yazar.
#
#   MODE=quick ./run_all.sh          # ~1-2 dk  (varsayilan)
#   MODE=full  ./run_all.sh          # 10-20 dk (tum sanitizer'lar, cokme testleri, yavas depolama, tekrarli olcumler)
#   ONLY=test3,bench ./run_all.sh    # yalniz secili adimlar: unit,e2e,example,test3,slow,mem,bench
#   MODE=full taskset -c 0,2,4,6 bash run_all.sh   # belirli cekirdeklere sabitle. DIKKAT: ortam degiskeni taskset'ten ONCE yazilir
#                                    # ('taskset -c 0-3 MODE=full ...' CALISMAZ: MODE=full bir program sanilir).
#                                    # Cekirdekleri FARKLI FIZIKSEL cekirdeklerden sec (SMT kardesleri L1'i paylasir): lscpu -e
#   EXTRA_LIBS=-latomic ./run_all.sh # 32-bit ARM'da 64-bit atomic link hatasi olursa
#
# Sonuc: results/<host>-<zaman>/summary.txt (bunu paylas), env.txt, logs/*.log
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${MODE:-quick}"
CXX="${CXX:-g++}"
ONLY="${ONLY:-}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="${OUT:-$ROOT/results/$(hostname)-$STAMP}"
mkdir -p "$OUT/bin" "$OUT/logs" "$OUT/data"
export PROFILER_TEST_DIR="$OUT/data"          # test dosyalari buraya yazilir (O_DIRECT destekleyen bir FS olsun)
SUM="$OUT/summary.txt"; : > "$SUM"
CXXF="-std=c++17 -Wall -Wextra -pthread -I$ROOT/include ${EXTRA_CXXFLAGS:-}"
LIBS="${EXTRA_LIBS:-}"
PASS=0; FAIL=0; SKIP=0
NPROC="${NPROC_OVERRIDE:-$(nproc 2>/dev/null || echo 1)}"   # NPROC_OVERRIDE yalniz betigin kendi dogrulamasi icin

say()  { echo "$*" | tee -a "$SUM"; }
want() { [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }
full() { [ "$MODE" = "full" ]; }

build() {   # build <ikili> <kaynak> <ek bayraklar...>
  local name=$1 src=$2; shift 2
  if $CXX $CXXF "$@" "$ROOT/$src" -o "$OUT/bin/$name" $LIBS > "$OUT/logs/build_$name.log" 2>&1; then return 0; fi
  say "  [DERLEME HATASI] $name  (bkz. logs/build_$name.log)"; FAIL=$((FAIL+1)); return 1
}
run() {     # run <etiket> <zaman_asimi_sn> <komut...>   (ortam degiskeni icin: env A=b komut)
  local label=$1 to=$2; shift 2
  local log="$OUT/logs/$(echo "$label" | tr -c 'A-Za-z0-9._-' '_').log"
  timeout "$to" "$@" > "$log" 2>&1; local rc=$?
  if   [ $rc -eq 0 ];   then say "  [GECTI]      $label";                          PASS=$((PASS+1))
  elif [ $rc -eq 124 ]; then say "  [ZAMAN ASIMI ${to}sn] $label";                 FAIL=$((FAIL+1))
  else                       say "  [BASARISIZ exit=$rc] $label  (bkz. $log)";     FAIL=$((FAIL+1)); fi
  LASTLOG="$log"; return $rc
}
keys() {    # keys <log> <regex> : onemli satirlari ozete ekle
  grep -E "$2" "$1" 2>/dev/null | sed 's/^/        /' | tee -a "$SUM"
}
skip() { say "  [ATLANDI]    $*"; SKIP=$((SKIP+1)); }

probe_san() {   # bu makinede sanitizer derlenip CALISIYOR mu?
  echo 'int main(){return 0;}' > "$OUT/data/probe.cpp"
  if $CXX -std=c++17 $2 -pthread "$OUT/data/probe.cpp" -o "$OUT/bin/probe_$1" > /dev/null 2>&1 && "$OUT/bin/probe_$1" > /dev/null 2>&1; then echo 1; else echo 0; fi
}

# ------------------------------------------------------------------ ortam bilgisi
{
  echo "tarih: $(date -u +%FT%TZ)"
  echo "kernel: $(uname -srmo)"
  echo "nproc: $NPROC"
  echo "cpu: $(grep -m1 -E 'model name|Model|Hardware' /proc/cpuinfo 2>/dev/null)"
  command -v lscpu > /dev/null && lscpu 2>/dev/null | grep -E "^(Architecture|CPU\(s\)|Thread|Core|Socket|Model name|Vendor|Hypervisor)" 
  echo "sanallastirma: $(systemd-detect-virt 2>/dev/null || echo bilinmiyor)"
  echo "clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo bilinmiyor)"
  echo "cpu governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo bilinmiyor)"
  echo "derleyici: $($CXX --version 2>/dev/null | head -1)"
  echo "glibc: $(ldd --version 2>/dev/null | head -1)"
  echo "dosya sistemi (test dizini): $(df -T "$OUT/data" 2>/dev/null | tail -1)"
  echo "blok boyutu (statvfs): $(stat -f -c '%S' "$OUT/data" 2>/dev/null)"
  echo "taskset: $(taskset -p $$ 2>/dev/null || echo yok)"
  echo "izinli cpu listesi: $(grep -m1 Cpus_allowed_list /proc/self/status 2>/dev/null | awk '{print $2}')"
  command -v lscpu > /dev/null && { echo "--- lscpu -e (CPU / CORE / SOCKET / ...):"; lscpu -e 2>/dev/null; }
} > "$OUT/env.txt" 2>&1
HAVE_ASAN=$(probe_san asan "-fsanitize=address,leak")
HAVE_TSAN=$(probe_san tsan "-fsanitize=thread")
HAVE_UBSAN=$(probe_san ubsan "-fsanitize=undefined")
{ echo "sanitizer: asan+lsan=$HAVE_ASAN tsan=$HAVE_TSAN ubsan=$HAVE_UBSAN"; } >> "$OUT/env.txt"
say "=== Vigil: MODE=$MODE  nproc=$NPROC  cikti=$OUT ==="
say "$(sed -n '1,4p' "$OUT/env.txt" | tr '\n' ' ')"
say "sanitizer destegi: asan+lsan=$HAVE_ASAN tsan=$HAVE_TSAN ubsan=$HAVE_UBSAN (0 ise o adimlar ATLANIR)"

# --- Olcumun GECERLILIGINI bozabilecek ortam uyarilari ---
ALLOWED="${ALLOWED_OVERRIDE:-$(grep -m1 Cpus_allowed_list /proc/self/status 2>/dev/null | awk '{print $2}')}"   # *_OVERRIDE yalniz betigin kendi dogrulamasi icin
TOPO="${TOPO_ROOT:-/sys/devices/system/cpu}"
if [ -n "$ALLOWED" ]; then
  cpus=(); IFS=',' read -ra parts <<< "$ALLOWED"
  for p in "${parts[@]}"; do if [[ $p == *-* ]]; then for ((c=${p%-*}; c<=${p#*-}; c++)); do cpus+=($c); done; else cpus+=($p); fi; done
  declare -A seen_core; have_topo=1
  for c in "${cpus[@]}"; do
    pk="$(cat "$TOPO/cpu$c/topology/physical_package_id" 2>/dev/null)"; co="$(cat "$TOPO/cpu$c/topology/core_id" 2>/dev/null)"
    if [ -z "$co" ]; then have_topo=0; break; fi
    seen_core["$pk-$co"]=1
  done
  if [ "$have_topo" = 1 ] && [ "${#seen_core[@]}" -lt "${#cpus[@]}" ]; then
    say "UYARI: izinli ${#cpus[@]} CPU yalnizca ${#seen_core[@]} FIZIKSEL cekirdege dagiliyor (SMT kardesleri L1 onbellegi PAYLASIR)."
    say "       Cekirdekler-arasi onbellek/cekisme sorusu icin FARKLI fiziksel cekirdekleri sec (lscpu -e ile CORE sutunu farkli olanlar)."
  fi
fi
FSTYPE="$(df -T "$OUT/data" 2>/dev/null | awk 'NR==2 {print $2}')"
case "$FSTYPE" in
  tmpfs|ramfs|overlay|overlayfs|9p|drvfs|nfs*|cifs|fuse*|vboxsf|virtiofs)
    say "UYARI: cikti dizini '$FSTYPE' uzerinde. 'slow' ve O_DIRECT sonuclari GERCEK SD/eMMC/SSD hakkinda bir sey soylemez."
    say "       Hedef depolamaya isaret et:  OUT=/hedef/depolama/uzerinde/results bash run_all.sh" ;;
esac
[ "$NPROC" -lt 2 ] && say "UYARI: nproc=$NPROC -- cekismeli (contended) olcum ve cok-cekirdek davranisi BU MAKINEDE GECERLI OLMAZ."

SAN_FLAGS() { case $1 in asan) echo "-fsanitize=address,leak";; tsan) echo "-fsanitize=thread";; ubsan) echo "-fsanitize=undefined";; esac; }
HAVE() { case $1 in asan) echo $HAVE_ASAN;; tsan) echo $HAVE_TSAN;; ubsan) echo $HAVE_UBSAN;; esac; }
SANF="-O1 -g -fno-omit-frame-pointer"

# ------------------------------------------------------------------ 1) birim / anlam testleri
if want unit; then
  say ""; say "--- unit: yazici, kismi flush, makro anlami ---"
  build unit_writer  tests/writer_unit_test.cpp -O2 && run "unit: yazici (O_DIRECT + buffered)" 300 "$OUT/bin/unit_writer"
  build unit_partial tests/partial_flush_test.cpp -O2 && run "unit: kismi flush / crash-ani disk durumu" 300 "$OUT/bin/unit_partial"
  build sem4096 tests/macro_semantics_test.cpp -O2 && run "unit: makro anlami + KIMLIK (ring 4096)" 120 "$OUT/bin/sem4096"
  build unit_html tests/html_report_test.cpp -O2 && run "unit: HTML rapor (kacis, rozet, siralama, cevrimdisi)" 60 "$OUT/bin/unit_html"
  build unit_session tests/session_test.cpp -O2 && { run "unit: ProfilerSession + son tahliye (yasayan thread'in ring'i dahil)" 300 "$OUT/bin/unit_session"; keys "$LASTLOG" "O_DIRECT|buffered"; }
  build unit_sites tests/site_registry_test.cpp -O2 && { run "unit: site tablosu (esz. kayit, tasma, tanim gidis-donusu)" 300 "$OUT/bin/unit_sites"; keys "$LASTLOG" "BILGI|tasma|tanim gidis"; }
  build race_demo tests/site_race_demo.cpp -O1 -g && { run "unit: site ilk-gecis yarisi (BIZIM desen tek id vermeli)" 300 "$OUT/bin/race_demo"; keys "$LASTLOG" "NAIF|BIZIM"; }
  if full; then
    for sz in 1024 64; do build sem$sz tests/macro_semantics_test.cpp -O2 -DPROFILER_RING_SIZE=$sz && run "unit: makro anlami (ring $sz)" 120 "$OUT/bin/sem$sz"; done
    for s in asan tsan ubsan; do
      if [ "$(HAVE $s)" = 1 ]; then
        build sem_$s tests/macro_semantics_test.cpp $SANF $(SAN_FLAGS $s) && run "unit: makro anlami ($s)" 300 "$OUT/bin/sem_$s"
        build us_$s  tests/site_registry_test.cpp   $SANF $(SAN_FLAGS $s) && run "unit: site tablosu ($s)" 600 "$OUT/bin/us_$s"
        build ss_$s  tests/session_test.cpp         $SANF $(SAN_FLAGS $s) && run "unit: ProfilerSession ($s)" 600 "$OUT/bin/ss_$s"
        build hr_$s  tests/html_report_test.cpp     $SANF $(SAN_FLAGS $s) && run "unit: HTML rapor ($s)" 300 "$OUT/bin/hr_$s"
        build uw_$s  tests/writer_unit_test.cpp     $SANF $(SAN_FLAGS $s) && run "unit: yazici ($s)" 300 "$OUT/bin/uw_$s"
        build up_$s  tests/partial_flush_test.cpp   $SANF $(SAN_FLAGS $s) && run "unit: kismi flush ($s)" 300 "$OUT/bin/up_$s"
      else skip "unit ($s) -- bu makinede $s calismiyor"; fi
    done
    if [ "$HAVE_TSAN" = 1 ]; then
      build race_tsan tests/site_race_demo.cpp $SANF -fsanitize=thread && {
        run "unit: site ilk-gecis yarisi, TSan, BIZIM desen (0 uyari beklenir)" 300 "$OUT/bin/race_tsan" ours
        "$OUT/bin/race_tsan" naive > "$OUT/logs/race_tsan_naive.log" 2>&1
        say "  [BILGI]      site yarisi, TSan, ONERILEN (atomik-olmayan static) desen: $(grep -c 'WARNING: ThreadSanitizer' "$OUT/logs/race_tsan_naive.log") veri yarisi uyarisi (>0 beklenir: bu desen HATALIDIR)"
      }
    fi
  fi
fi

# ------------------------------------------------------------------ 2) uctan uca
if want e2e; then
  say ""; say "--- e2e: makrolar -> ring -> registry -> flush -> O_DIRECT dosya -> geri-okuma dogrulamasi ---"
  build e2e tests/e2e_durable.cpp -O2 && run "e2e: duz (O_DIRECT, gec kayit yolu)" 600 "$OUT/bin/e2e"
  [ -n "${LASTLOG:-}" ] && keys "$LASTLOG" "I/O modu|Yazici istatistikleri|tanimli site|kilit  |asim   |yetim_olay|TUM INVARIANT"
  build decode tools/profiler_decode.cpp -O2 && {
    run "e2e: profiler_decode dosyayi cozer (yetim olay=0 ise exit 0)" 120 "$OUT/bin/decode" "$PROFILER_TEST_DIR/profiler_output_durable.bin"
    keys "$LASTLOG" "bicim surumu|KILIT|DEADLINE|queue_lock|frame_even|e2e_durable.cpp"
  }
  if full; then
    for s in asan tsan ubsan; do
      if [ "$(HAVE $s)" = 1 ]; then build e2e_$s tests/e2e_durable.cpp $SANF $(SAN_FLAGS $s) && run "e2e: $s" 900 "$OUT/bin/e2e_$s"
      else skip "e2e ($s)"; fi
    done
    for pm in 0 100 20; do
      for kt in 4000 7000 12000; do
        label="e2e: SIGKILL kismi_flush=${pm}ms kill=${kt}ms"
        log="$OUT/logs/$(echo "$label" | tr -c 'A-Za-z0-9._-' '_').log"
        env PROFILER_PARTIAL_MS=$pm timeout 120 "$OUT/bin/e2e" --crash-test $kt > "$log" 2>&1; rc=$?
        case $rc in
          0)   say "  [GECTI]      $label (SIGKILL ile oldu, veri dogrulandi)";;
          3)   say "  [BILGI]      $label -- is yuku SIGKILL'den ONCE kendi bitti (bu donanim/yuk icin kill_ms cok uzun; bu HATA DEGIL, kayip=0 dogrulandi, bkz. log)";;
          124) say "  [ZAMAN ASIMI 120sn] $label"; FAIL=$((FAIL+1));;
          *)   say "  [BASARISIZ exit=$rc] $label  (bkz. $log)"; FAIL=$((FAIL+1));;
        esac
        [ $rc -eq 0 -o $rc -eq 3 ] && PASS=$((PASS+1))
        keys "$log" "KAYIP"
      done
    done
    if gcc -shared -fPIC -O2 "$ROOT/tools/odirect_shim.c" -o "$OUT/bin/odirect_shim.so" -ldl > "$OUT/logs/build_shim.log" 2>&1; then
      for m in open_einval write_einval; do
        run "e2e: O_DIRECT arizasi simulasyonu ($m) -> buffered fallback" 600 env SHIM_ODIRECT=$m LD_PRELOAD="$OUT/bin/odirect_shim.so" "$OUT/bin/e2e"
        keys "$LASTLOG" "I/O modu"
      done
    else skip "shim derlenemedi (fallback testleri)"; fi
  fi
fi

# ------------------------------------------------------------------ 2b) README hizli baslangic ornegi
if want example; then
  say ""; say "--- example: README'deki hizli baslangic ornegi derlenir, calistirilir, cozulur ---"
  build quickstart examples/quickstart.cpp -O2 && build decode tools/profiler_decode.cpp -O2 && {
    run "example: quickstart calisir" 120 "$OUT/bin/quickstart" "$OUT/data/quickstart.bin"
    run "example: profiler_decode ciktisi (queue_lock=8000, yetim=0)" 60 "$OUT/bin/decode" "$OUT/data/quickstart.bin"
    keys "$LASTLOG" "bicim surumu|queue_lock|frame_work"
    if grep -qE "queue_lock +8000 " "$LASTLOG"; then say "  [GECTI]      example: queue_lock tam 8000 olay"; PASS=$((PASS+1)); else say "  [BASARISIZ]  example: queue_lock 8000 degil"; FAIL=$((FAIL+1)); fi
    run "example: --html raporu uretir" 60 "$OUT/bin/decode" "$OUT/data/quickstart.bin" --html "$OUT/data/report.html"
    if [ -s "$OUT/data/report.html" ] && grep -q "queue_lock" "$OUT/data/report.html" && ! grep -qE "http://|https://|cdn\." "$OUT/data/report.html"; then
      say "  [GECTI]      example: HTML rapor dolu, veriyi iceriyor, harici kaynak yok -> $OUT/data/report.html"; PASS=$((PASS+1))
    else
      say "  [BASARISIZ]  example: HTML rapor eksik/bos/harici kaynak iceriyor"; FAIL=$((FAIL+1))
    fi
  }
fi

# ------------------------------------------------------------------ 3) Test 3: thread churn
if want test3; then
  say ""; say "--- test3: thread doğum/ölüm stresi (watchdog'lu). fixed=duzeltilmis tabanca, buggy=ORIJINAL hatali kod ---"
  build t3 tests/test3_churn.cpp -O2 -DENABLE_CHAOS_TESTING
  ok=0; for i in 1 2 3; do T3_START_GUN=fixed T3_ITERS=200 T3_STALL_MS=15000 timeout 120 "$OUT/bin/t3" > "$OUT/logs/t3_fixed_plain_$i.log" 2>&1 && ok=$((ok+1)); done
  if [ $ok -eq 3 ]; then say "  [GECTI]      test3 duz derleme, fixed, 100x200 (3/3)"; PASS=$((PASS+1)); else say "  [BASARISIZ]  test3 duz derleme, fixed: $ok/3 (bkz. logs/t3_fixed_plain_*.log)"; FAIL=$((FAIL+1)); fi
  if [ "$HAVE_TSAN" = 1 ]; then
    build t3_tsan tests/test3_churn.cpp $SANF -fsanitize=thread -DENABLE_CHAOS_TESTING
    n=$([ "$MODE" = full ] && echo 20 || echo 3); ok=0
    for i in $(seq 1 $n); do
      it=$([ $((i % 2)) = 0 ] && echo 200 || echo 150)
      T3_START_GUN=fixed T3_ITERS=$it T3_STALL_MS=15000 timeout 120 "$OUT/bin/t3_tsan" > "$OUT/logs/t3_fixed_tsan_$i.log" 2>&1 && ok=$((ok+1))
    done
    if [ $ok -eq $n ]; then say "  [GECTI]      test3 TSan, fixed, 100x150/200 ($ok/$n)"; PASS=$((PASS+1)); else say "  [BASARISIZ]  test3 TSan, fixed: $ok/$n (bkz. logs/t3_fixed_tsan_*.log)"; FAIL=$((FAIL+1)); fi
    if full; then   # ORIJINAL hatali kodla karsilastirma: BILGI amacli, asilmasi BEKLENEN (FAIL sayilmaz)
      h=0; for i in $(seq 1 10); do
        T3_START_GUN=buggy T3_ITERS=150 T3_STALL_MS=8000 timeout 60 "$OUT/bin/t3_tsan" > "$OUT/logs/t3_buggy_tsan_$i.log" 2>&1; [ $? -eq 3 ] && h=$((h+1))
      done
      say "  [BILGI]      test3 TSan, buggy(orijinal), 100x150: $h/10 kosu ASILDI. Hata ARALIKLI: bizim 1-cekirdek sandbox'imizda ~%24/kosu (10/42) olculdu;"
      say "               0 cikmasi MUMKUNDUR ve hatanin yoklugu anlamina GELMEZ. Asilma varsa logs/t3_buggy_tsan_*.log'ta kayip-uyandirma imzasi aranir."
      grep -l "KAYIP UYANDIRMA" "$OUT"/logs/t3_buggy_tsan_*.log 2>/dev/null | wc -l | sed 's/^/        imzali log sayisi: /' | tee -a "$SUM"
    fi
  else skip "test3 TSan -- bu makinede tsan calismiyor"; fi
fi

# ------------------------------------------------------------------ 4) yavas depolama
if want slow; then
  say ""; say "--- slow: depolama yavaslayinca worker gecikmesi ve dusen olaylar ---"
  build slow tests/slow_sd_test.cpp -O2 && {
    run "slow: hizli depolama (taban cizgisi)" 300 env SLOW_PREREGISTER=1 SLOW_DURATION_MS=8000 "$OUT/bin/slow"
    keys "$LASTLOG" "push_event gecikmesi|HESAP|RING_BOYUTU"
    if full && gcc -shared -fPIC -O2 "$ROOT/tools/odirect_shim.c" -o "$OUT/bin/odirect_shim.so" -ldl > /dev/null 2>&1; then
      run "slow: surekli yavas (pwrite 100ms, fdatasync 200ms)" 300 env SLOW_PREREGISTER=1 SHIM_WRITE_DELAY_US=100000 SHIM_SYNC_DELAY_US=200000 LD_PRELOAD="$OUT/bin/odirect_shim.so" "$OUT/bin/slow"
      keys "$LASTLOG" "push_event gecikmesi|flush thread|HESAP"
      run "slow: tek seferlik 3sn takilma (t=2sn)" 300 env SLOW_PREREGISTER=1 SHIM_STALL_AT_MS=2000 SHIM_STALL_MS=3000 LD_PRELOAD="$OUT/bin/odirect_shim.so" "$OUT/bin/slow"
      keys "$LASTLOG" "gercek hiz|push_event gecikmesi|flush thread|HESAP"
    fi
  }
fi

# ------------------------------------------------------------------ 5) bellek
if want mem; then
  say ""; say "--- mem: canli thread basina ring bellegi; thread cikinca serbest kaliyor mu ---"
  for sz in 4096 1024; do
    build mem$sz tests/ring_memory_test.cpp -O2 -DPROFILER_RING_SIZE=$sz && { run "mem: ring $sz" 300 "$OUT/bin/mem$sz"; keys "$LASTLOG" "sizeof|thread basina EK|serbest birakilan"; }
  done
fi

# ------------------------------------------------------------------ 6) PROFILE_LOCK_SCOPE maliyeti
if want bench; then
  say ""; say "--- bench: PROFILE_LOCK_SCOPE maliyeti (SANITIZER'SIZ -O2; makine bosta olmali) ---"
  build bench bench/lock_scope_bench.cpp -O2 && {
    R=$([ "$MODE" = full ] && echo "${BENCH_ROUNDS:-100}" || echo "${BENCH_ROUNDS:-30}")
    reps=$([ "$MODE" = full ] && echo 3 || echo 1)
    for i in $(seq 1 $reps); do
      run "bench: cekismesiz tek thread (kosu $i)" 900 env BENCH_ROUNDS=$R BENCH_OUT="$OUT/data/bench.bin" "$OUT/bin/bench"
      keys "$LASTLOG" "bare unique_lock|PROFILE_LOCK_SCOPE \(BASLIK\)|ESKI makro|makro - bare  \(esli fark, temiz\)|BASLIK makro - ESKI|AYRISTIRMA|flush thread CPU|makro\(dolu\) - bare"
    done
    if [ "$NPROC" -ge 2 ]; then
      Ts="2 $( [ "$NPROC" -ge 4 ] && echo 4)"; full && [ "$NPROC" -gt 4 ] && Ts="$Ts $NPROC"
      for T in $Ts; do
        run "bench: CEKISMELI $T thread" 900 env BENCH_THREADS=$T BENCH_ROUNDS=15 BENCH_ROUNDS_CONTENDED=${BENCH_ROUNDS_CONTENDED:-60} BENCH_OUT="$OUT/data/bench.bin" "$OUT/bin/bench"
        keys "$LASTLOG" "FAZ 2|bare  :|makro :|makro - bare \(esli, tur|NOT:"
      done
    else skip "cekismeli bench -- nproc=1 (gercek cekisme olusamaz)"; fi
  }
fi

say ""
say "=== OZET: GECTI=$PASS  BASARISIZ=$FAIL  ATLANDI=$SKIP   (ayrintilar: $OUT) ==="
say "Bu dosyayi ($SUM) ve $OUT/env.txt'yi paylas. Basarisiz adimlar icin ilgili logs/*.log dosyasini da ekle."
[ $FAIL -eq 0 ]
