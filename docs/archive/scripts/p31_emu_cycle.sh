#!/bin/bash
# =============================================================================
# P3.1 — FASE 3: Ciclo de validação do APK no emulador (cold boot TCG ~215s)
#   boot → install → launch → observe → collect → verdict — UMA chamada.
# Orçamento p/ chamada de 590s: boot 300 + install 90 + launch 30 + observe N
# + coleta 60. Saída: /home/z/my-project/download/p31/cycle_<stamp>/
# =============================================================================
set -uo pipefail
export LD_PRELOAD=/home/z/my-project/scripts/mmap_norescue.so
export ANDROID_AVD_HOME=/tmp/my-project/avd
EMU_BIN=/home/z/android-sdk/emulator/emulator
ADB_BIN=/home/z/android-sdk/platform-tools/adb
ADB="timeout 30 $ADB_BIN"
STAMP=$(date +%Y%m%d_%H%M%S)
OUT=/home/z/my-project/download/p31/cycle_${STAMP}
mkdir -p "$OUT"
LOG=$OUT/cycle.log
exec > >(tee -a "$LOG") 2>&1

RAW_ARG="${1:?uso: p31_emu_cycle.sh <apk> [observe] [backend] [editor|runtime|settings]}"
CYCLE_MEM=${CYCLE_MEM:-1024}
ACTIVITY=${4:-editor}   # editor=EditorActivity | runtime=GoniActivity | settings=Settings
if [[ "$RAW_ARG" == "control" ]]; then
  APK=""
  ACTIVITY="${ACTIVITY:-settings}"
else
  APK=$(realpath "$RAW_ARG")
fi
OBSERVE=${2:-60}
BACKEND_EXTRA=${3:-}

echo "[cycle] apk=$APK observe=${OBSERVE}s backend=${BACKEND_EXTRA:-auto}"

# --- 1. Cold boot (snapshot restore não funciona sob TCG — documentado) ------
"$EMU_BIN" -avd goni_p31 -no-window -no-accel -gpu swiftshader_indirect \
           -no-audio -no-boot-anim -memory $CYCLE_MEM -cores ${CYCLE_CORES:-2} -no-snapshot \
           -port 5554 > "$OUT/emulator.log" 2>&1 &
EMU_PID=$!
echo "[cycle] emulator PID=$EMU_PID (cold boot TCG)"

$ADB_BIN start-server > /dev/null 2>&1 || true
DEADLINE=$((SECONDS + 300))
BOOTED=0; BOOT_T=0
while (( SECONDS < DEADLINE )); do
  kill -0 "$EMU_PID" 2>/dev/null || { echo "[cycle] EMULATOR MORREU no boot"; break; }
  BC=$($ADB -s emulator-5554 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
  [[ "$BC" == "1" ]] && { BOOTED=1; BOOT_T=$SECONDS; break; }
  sleep 4
done
echo "[cycle] boot_concluido=$BOOTED em ${BOOT_T}s"
[[ "$BOOTED" != "1" ]] && { kill -9 "$EMU_PID" 2>/dev/null; exit 2; }

# --- 1b. Estabilização opcional (default OFF):
# EVIDÊNCIA P3.1: o guest ATD/TCG morre sozinho ~55-120s pós-boot (control
# test com Settings morreu igual) — networkstack "bg anr" → system_server
# "Lost network stack". A janela ÚTIL é logo após boot: testar AGORA e
# coletar ANTES da morte ambiental. CYCLE_SETTLE=1 religa a espera.
if [[ "${CYCLE_SETTLE:-0}" == "1" ]]; then
  $ADB -s emulator-5554 shell cmd connectivity airplane-mode enable >/dev/null 2>&1 || true
  sleep 45
fi

# --- 2. Instalar o MESMO APK do build --------------------------------------
if [[ -z "$APK" ]]; then
  echo "[cycle] modo control: sem install"
else
  for ATTEMPT in 1 2 3; do
    timeout 90 $ADB_BIN -s emulator-5554 install -r "$APK" > "$OUT/install.txt" 2>&1
    INSTALL_RC=$?
    echo "[cycle] install tentativa $ATTEMPT rc=$INSTALL_RC: $(tail -1 "$OUT/install.txt")"
    [[ $INSTALL_RC -eq 0 ]] && break
    sleep 3
  done
  [[ $INSTALL_RC -ne 0 ]] && { kill -9 "$EMU_PID" 2>/dev/null; exit 3; }
fi

# --- 3. Launch do editor (launcher activity) -------------------------------
$ADB -s emulator-5554 logcat -c 2>/dev/null || true
case "$ACTIVITY" in
  runtime)
    # Demo mínima (MESMO APK): janela nativa + backend + shaders + 1º frame
    # sem o custo de UI do editor — valida o caminho de render no Android.
    TARGET=com.goni.runtime/.GoniActivity ;;
  settings)
    TARGET=com.android.settings/.Settings ;;
  *)
    TARGET=com.goni.runtime/.EditorActivity ;;
esac
echo "[cycle] launch: $TARGET (backend=${BACKEND_EXTRA:-auto})"
if [[ -n "$BACKEND_EXTRA" && "$ACTIVITY" != "settings" ]]; then
  $ADB -s emulator-5554 shell am start -W -n "$TARGET" \
      --es backend "$BACKEND_EXTRA" > "$OUT/am_start.txt" 2>&1
else
  $ADB -s emulator-5554 shell am start -W -n "$TARGET" \
      > "$OUT/am_start.txt" 2>&1
fi
echo "[cycle] am_start: $(rg -o 'Status: .*|TotalTime: .*' "$OUT/am_start.txt" | tr '\n' ' ')"
LAUNCH_T=$SECONDS

# --- 4. Observação: processo vivo? crash? progresso dos estágios? -----------
DEAD=0
WATCHPROC=com.goni.runtime
[[ "$ACTIVITY" == "settings" ]] && WATCHPROC=com.android.settings
POLL_N=0
while (( SECONDS - LAUNCH_T < OBSERVE )); do
  PID=$($ADB -s emulator-5554 shell pidof $WATCHPROC 2>/dev/null | tr -d '\r')
  if [[ -z "$PID" ]]; then
    sleep 3
    PID2=$($ADB -s emulator-5554 shell pidof $WATCHPROC 2>/dev/null | tr -d '\r')
    [[ -z "$PID2" ]] && { DEAD=1; break; }
  fi
  # Progresso dos estágios DURANTE a observação (evidência sobrevive a
  # timeouts da chamada): pull incremental do goni_startup.log a cada ~15s.
  if (( (SECONDS - LAUNCH_T) / 15 > POLL_N )); then
    POLL_N=$(( (SECONDS - LAUNCH_T) / 15 ))
    echo "--- pull ${POLL_N} (t+$((SECONDS - LAUNCH_T))s) ---" >> "$OUT/goni_startup_progress.txt"
    timeout 15 $ADB_BIN -s emulator-5554 shell "run-as $WATCHPROC cat files/goni_startup.log 2>/dev/null" >> "$OUT/goni_startup_progress.txt" 2>/dev/null || true
  fi
  sleep 3
done
ALIVE_T=$((SECONDS - LAUNCH_T))
PROC="ALIVE"
[[ $DEAD -eq 1 ]] && PROC="DEAD_AFTER_${ALIVE_T}s"
echo "[cycle] processo: $PROC (observado ${ALIVE_T}s)"

# --- 5. Coleta de evidência (best-effort, cada passo com timeout) -----------
$ADB -s emulator-5554 logcat -d -t 8000 > "$OUT/logcat_full.txt" 2>/dev/null || true
$ADB -s emulator-5554 logcat -d -b crash > "$OUT/logcat_crash_buffer.txt" 2>/dev/null || true
rg -a "GONI" "$OUT/logcat_full.txt" > "$OUT/logcat_goni.txt" 2>/dev/null || true
(rg -ai "FATAL EXCEPTION|AndroidRuntime|SIGSEGV|SIGABRT|SIGBUS|SIGILL|SIGFPE|Fatal signal|UnsatisfiedLinkError|tombstone|abort" "$OUT/logcat_full.txt" || true) > "$OUT/logcat_crash_lines.txt"
timeout 20 $ADB_BIN -s emulator-5554 exec-out screencap -p > "$OUT/screen.png" 2>/dev/null || true
$ADB -s emulator-5554 shell "run-as com.goni.runtime ls -laR files/ 2>/dev/null" > "$OUT/app_files.txt" 2>/dev/null || true
$ADB -s emulator-5554 shell "run-as com.goni.runtime cat files/goni_startup.log 2>/dev/null" > "$OUT/goni_startup.log" 2>/dev/null || true
$ADB -s emulator-5554 shell "run-as com.goni.runtime cat files/goni_crash.log 2>/dev/null" > "$OUT/goni_crash.log" 2>/dev/null || true

# --- 6. Verdict --------------------------------------------------------------
# Morte AMBIENTAL (system_server do ATD/TCG morreu — DeadSystemException)
# ≠ crash PRÓPRIO do app. O diagnóstico do GONI (stages + crash log) é a
# fonte da verdade: STARTUP_COMPLETE alcançado = app funcional até a morte
# do sistema hóspede.
GONI_NATIVE_CRASH=0
if rg -aq "Fatal signal.*\(com\.goni\.runtime\)|SIGSEGV.*goni|SIGABRT.*goni" "$OUT/logcat_full.txt" 2>/dev/null; then
  GONI_NATIVE_CRASH=1
fi
ENV_SYSTEM_DEATH=0
rg -aq "DeadSystemException|Lost network stack|FATAL EXCEPTION IN SYSTEM PROCESS" "$OUT/logcat_full.txt" 2>/dev/null && ENV_SYSTEM_DEATH=1
VERDICT=PASS
[[ $DEAD -eq 1 ]] && VERDICT=FAIL_PROCESS_DEAD
[[ $ENV_SYSTEM_DEATH -eq 1 ]] && VERDICT="ENV_SYSTEM_DEATH_${VERDICT}"
[[ $GONI_NATIVE_CRASH -eq 1 ]] && VERDICT=FAIL_CRASH_GONI
LAST_STAGE="NONE"
if rg -aq "STARTUP_COMPLETE" "$OUT/logcat_goni.txt" 2>/dev/null; then
  LAST_STAGE=STARTUP_COMPLETE
else
  LS=$(rg -ao "\[GONI\]\[STARTUP\] [A-Z_]+" "$OUT/logcat_goni.txt" 2>/dev/null | tail -1 | sed 's/.*STARTUP\] //')
  [[ -n "$LS" ]] && LAST_STAGE="$LS"
fi
{
  echo "verdict: $VERDICT"
  echo "process: $PROC"
  echo "observed_seconds: $ALIVE_T"
  echo "boot_seconds: $BOOT_T"
  echo "last_startup_stage: $LAST_STAGE"
  echo "watch_proc: ${WATCHPROC:-com.goni.runtime}"
echo "apk: $APK"
  if [[ -f "$APK" ]]; then
    echo "apk_sha256: $(sha256sum "$APK" | cut -d' ' -f1)"
  else
    echo "apk_sha256: (persistido no AVD: modo $APK)"
  fi
  echo "date: $STAMP"
  echo "backend: ${BACKEND_EXTRA:-auto}"
} | tee "$OUT/verdict.txt"

# --- 7. Encerrar (GRACEFUL: SIGKILL no qemu perde o flush do userdata!) -----
timeout 20 $ADB_BIN -s emulator-5554 emu kill 2>/dev/null
for I in $(seq 1 15); do
  kill -0 "$EMU_PID" 2>/dev/null || break
  sleep 1
done
kill "$EMU_PID" 2>/dev/null; sleep 2; kill -9 "$EMU_PID" 2>/dev/null
echo "[cycle] evidências: $OUT"
exit 0
