#!/bin/bash
# =============================================================================
# P3.2 — Verificação do EXPORT AUTOMÁTICO de diagnósticos no emulador.
#
# Complementa (não substitui) o ciclo p31_emu_cycle.sh: valida que o próprio
# G.ONI copia goni_startup.log/goni_crash.log para Download/GONI/ via
# MediaStore (acessível ao usuário sem run-as/adb — o cenário REAL do
# Realme C33, onde Termux/run-as não tem permissão).
#
# Etapas: cold boot → espera do volume de storage (lição do ATD: o volume
# 'external_primary' só fica pronto ~1-2min pós-boot; o app tenta de novo a
# cada estágio, mas o teste espera o volume ANTES de lançar) → install →
# launch → (1) linha no MediaStore com dono com.goni.runtime → (2) conteúdo
# REAL no backing store /data/media/0/Download/GONI/ com estágios → (3)
# SEM duplicatas (uma linha por arquivo — bug do v1 corrigido) → (4) crash
# INJETADO no arquivo privado → force-stop → relaunch → goni_crash.log
# exportado na execução SEGUINTE → teardown graceful.
#
# NOTA de visibilidade: neste ATD o shell não atravessa o FUSE de /sdcard
# (Permission denied — particularidade da imagem), então a verificação usa
# o MediaStore (content query — o que o gerenciador de arquivos/MTP do
# usuário usa) + o backing store via 'adb root' (bytes reais no disco).
#
# Uso: p32_verify_export.sh <apk> [wait_seconds]
# Saída: /home/z/my-project/download/p32/export_<stamp>/ (+ verdict.txt)
# =============================================================================
set -uo pipefail
export LD_PRELOAD=/home/z/my-project/scripts/mmap_norescue.so
export ANDROID_AVD_HOME=/tmp/my-project/avd
EMU_BIN=/home/z/android-sdk/emulator/emulator
ADB_BIN=/home/z/android-sdk/platform-tools/adb
ADB="timeout 30 $ADB_BIN"
STAMP=$(date +%Y%m%d_%H%M%S)
OUT=/home/z/my-project/download/p32/export_${STAMP}
mkdir -p "$OUT"
LOG=$OUT/export.log
exec > >(tee -a "$LOG") 2>&1

APK=$(realpath "${1:?uso: p32_verify_export.sh <apk> [wait_seconds]}")
WAIT=${2:-90}
PKG=com.goni.runtime
ACT=com.goni.runtime/.EditorActivity
MS_URI=content://media/external/downloads
BACKING=/data/media/0/Download/GONI

echo "[p32] apk=$APK wait=${WAIT}s"

# --- 1. Cold boot (mesma configuração estável do P3.1 — docs §3) -----------
"$EMU_BIN" -avd goni_p31 -no-window -no-accel -gpu swiftshader_indirect \
           -no-audio -no-boot-anim -memory 2048 -cores 4 -no-snapshot \
           -port 5554 > "$OUT/emulator.log" 2>&1 &
EMU_PID=$!
echo "[p32] emulator PID=$EMU_PID (cold boot TCG)"

$ADB_BIN start-server > /dev/null 2>&1 || true
DEADLINE=$((SECONDS + 300))
BOOTED=0
while (( SECONDS < DEADLINE )); do
  kill -0 "$EMU_PID" 2>/dev/null || { echo "[p32] EMULATOR MORREU no boot"; break; }
  BC=$($ADB -s emulator-5554 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
  [[ "$BC" == "1" ]] && { BOOTED=1; break; }
  sleep 4
done
echo "[p32] boot_concluido=$BOOTED em ${SECONDS}s"
[[ "$BOOTED" != "1" ]] && { kill -9 "$EMU_PID" 2>/dev/null; exit 2; }

# --- 1b. Volume de storage pronto (external_primary) ------------------------
# Evidência da 1ª execução: mirrors falhavam com "Volume external_primary
# not found" nos primeiros segundos. O app se autocura (cada estágio tenta
# de novo), mas o teste não deve competir com essa corrida.
VOL_OK=0
for I in $(seq 1 30); do
  V=$($ADB -s emulator-5554 shell "content query --uri $MS_URI --projection _id 2>/dev/null" | tr -d '\r')
  [[ -n "$V" ]] && { VOL_OK=1; break; }
  sleep 4
done
echo "[p32] volume_media_pronto=$VOL_OK após $((I*4))s"
[[ "$VOL_OK" != "1" ]] && { kill -9 "$EMU_PID" 2>/dev/null; exit 4; }

# --- 2. Instalar + launch ----------------------------------------------------
# O userdata do AVD persiste entre ciclos (-no-snapshot preserva o qcow2):
# desinstala versões antigas (assinatura de outro debug.keystore pós-reset
# do host) e limpa duplicatas GONI de execuções de teste anteriores.
$ADB -s emulator-5554 uninstall $PKG > /dev/null 2>&1 || true
$ADB -s emulator-5554 shell "content delete --uri $MS_URI --where \"_data LIKE '%GONI%'\"" > /dev/null 2>&1 || true
for ATTEMPT in 1 2 3; do
  timeout 90 $ADB_BIN -s emulator-5554 install "$APK" > "$OUT/install.txt" 2>&1 \
    && break
  sleep 3
done
grep -q "Success" "$OUT/install.txt" || { echo "[p32] INSTALL FALHOU"; kill -9 "$EMU_PID"; exit 3; }
echo "[p32] install ok"

$ADB -s emulator-5554 logcat -c 2>/dev/null || true
$ADB -s emulator-5554 shell am start -W -n "$ACT" > "$OUT/am_start.txt" 2>&1 || true
echo "[p32] launch: $(rg -o 'Status: .*' "$OUT/am_start.txt" | head -1)"
LAUNCH_T=$SECONDS

# --- 3. TESTE 1: linha no MediaStore com dono do app + estágios -------------
# O que o gerenciador de arquivos/MTP do usuário vê (índice MediaStore).
PUB_OK=0
STAGE_OK=0
while (( SECONDS - LAUNCH_T < WAIT )); do
  ROWS=$($ADB -s emulator-5554 shell "content query --uri $MS_URI --projection _display_name:owner_package_name 2>/dev/null" | tr -d '\r')
  if echo "$ROWS" | rg -q "goni_startup\.log.*com\.goni\.runtime"; then
    PUB_OK=1
    # Conteúdo REAL no backing store (bytes no disco) via adb root
    $ADB_BIN -s emulator-5554 root > /dev/null 2>&1 || true
    sleep 2
    CONTENT=$($ADB -s emulator-5554 shell "cat $BACKING/goni_startup.log 2>/dev/null" | tr -d '\r')
    STAGE_N=$(echo "$CONTENT" | rg -c "STARTUP_" || echo 0)
    echo "[p32] t+$((SECONDS - LAUNCH_T))s: backing goni_startup.log com ${STAGE_N} estágios"
    (( STAGE_N >= 5 )) && { STAGE_OK=1; break; }
  fi
  sleep 5
done
MS_ROWS=$($ADB -s emulator-5554 shell "content query --uri $MS_URI --projection _display_name:owner_package_name 2>/dev/null" | tr -d '\r')
echo "$MS_ROWS" | rg "goni_startup" > "$OUT/mediastore_rows.txt" 2>/dev/null || true
$ADB -s emulator-5554 shell "cat $BACKING/goni_startup.log 2>/dev/null" > "$OUT/backing_startup.log" 2>/dev/null || true
$ADB -s emulator-5554 shell "run-as $PKG cat files/goni_startup.log 2>/dev/null" > "$OUT/private_startup.log" 2>/dev/null || true
echo "[p32] teste1_mediastore=$PUB_OK teste1_estagios_backing=$STAGE_OK"

# --- 5. TESTE 3: crash da execução anterior é exportado no próximo início -----
# O crash handler grava APENAS no privado (signal-safety). Simulamos o
# registro de um crash REAL (formato do handler) e verificamos que o
# próximo onCreate o copia para Download/GONI/goni_crash.log.
$ADB -s emulator-5554 shell "run-as $PKG sh -c 'printf \"[crash] signal=SIGSEGV(11) code=1 addr=0x10 stage=P32_INJECTED_TEST pc=0x0\n\" >> files/goni_crash.log'" 2>/dev/null
$ADB -s emulator-5554 shell am force-stop $PKG 2>/dev/null
sleep 3
$ADB -s emulator-5554 shell am start -W -n "$ACT" > "$OUT/am_start2.txt" 2>&1 || true
RELAUNCH_T=$SECONDS
CRASH_OK=0
while (( SECONDS - RELAUNCH_T < 90 )); do
  CLS=$($ADB -s emulator-5554 shell "cat $BACKING/goni_crash.log 2>/dev/null" | tr -d '\r')
  if [[ "$CLS" == *P32_INJECTED_TEST* ]]; then
    CRASH_OK=1
    break
  fi
  sleep 4
done
$ADB -s emulator-5554 shell "cat $BACKING/goni_crash.log 2>/dev/null" > "$OUT/backing_crash.log" 2>/dev/null || true
echo "[p32] teste3_crash_exportado_proxima_execucao=$CRASH_OK"

# --- 4b. TESTE 2 (APÓS relaunch): SEM duplicatas no backing store ------------
# A contagem precisa ser APÓS o relaunch: o bug do v2 só se manifestava na
# 2ª execução (a linha do processo anterior não era achada → nascia
# "goni_startup (1).log"). Contar só na 1ª execução NÃO esconde o bug.
sleep 15  # espelhos do relaunch assentarem
DUPES=$($ADB -s emulator-5554 shell "ls $BACKING/ 2>/dev/null" | tr -d '\r' | grep -c "goni_startup" || echo 0)
DUP_OK=0
(( DUPES == 1 )) && DUP_OK=1
echo "[p32] teste2_sem_duplicatas=$DUP_OK (arquivos goni_startup: $DUPES)"
$ADB -s emulator-5554 shell "ls -la $BACKING/ 2>/dev/null" > "$OUT/backing_ls.txt" 2>/dev/null || true

# --- 6. Evidência complementar ------------------------------------------------
$ADB -s emulator-5554 logcat -d 2>/dev/null | rg "GONI" > "$OUT/logcat_goni.txt" || true
$ADB -s emulator-5554 logcat -d -b crash 2>/dev/null > "$OUT/logcat_crash.txt" || true
PID_FINAL=$($ADB -s emulator-5554 shell pidof $PKG 2>/dev/null | tr -d '\r')
PROC="ALIVE"
[[ -z "$PID_FINAL" ]] && PROC="DEAD"
MIRROR_N=$(rg -c "\[MIRROR\]" "$OUT/logcat_goni.txt" 2>/dev/null || echo 0)
MIRROR_ERR=$(rg -c "mirror:.*falhou" "$OUT/logcat_goni.txt" 2>/dev/null || echo 0)
echo "[p32] processo_final=$PROC pid=${PID_FINAL:-none} mirrors=$MIRROR_N erros_mirror=$MIRROR_ERR"
# backing store listing completo p/ inspeção
$ADB -s emulator-5554 shell "ls -la $BACKING/ 2>/dev/null" > "$OUT/backing_ls.txt" 2>/dev/null || true

# --- 7. Verdict ---------------------------------------------------------------
VERDICT=PASS
[[ $PUB_OK -ne 1 ]] && VERDICT=FAIL_MEDIASTORE_ROW_MISSING
[[ $STAGE_OK -ne 1 ]] && VERDICT=FAIL_BACKING_NO_STAGES
[[ $DUP_OK -ne 1 ]] && VERDICT=FAIL_DUPLICATE_FILES
[[ $CRASH_OK -ne 1 ]] && VERDICT=FAIL_CRASH_NOT_EXPORTED
[[ "$PROC" == "DEAD" ]] && VERDICT=FAIL_PROCESS_DEAD
{
  echo "verdict: $VERDICT"
  echo "mediastore_row: $PUB_OK"
  echo "backing_stages: $STAGE_OK"
  echo "no_duplicates: $DUP_OK (files=$DUPES)"
  echo "crash_export_next_launch: $CRASH_OK"
  echo "process: $PROC"
  echo "mirror_logcat_lines: $MIRROR_N"
  echo "mirror_errors: $MIRROR_ERR"
  echo "apk: $APK"
  echo "apk_sha256: $(sha256sum "$APK" | cut -d' ' -f1)"
  echo "date: $STAMP"
} | tee "$OUT/verdict.txt"

# --- 8. Teardown GRACEFUL (flush do userdata) ---------------------------------
timeout 20 $ADB_BIN -s emulator-5554 emu kill 2>/dev/null
for I in $(seq 1 15); do
  kill -0 "$EMU_PID" 2>/dev/null || break
  sleep 1
done
kill "$EMU_PID" 2>/dev/null; sleep 2; kill -9 "$EMU_PID" 2>/dev/null
echo "[p32] evidências: $OUT"
[[ "$VERDICT" == "PASS" ]] && exit 0 || exit 1
