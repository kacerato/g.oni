#!/bin/bash
# =============================================================================
# P3.1 FASE 2 — Bootstrap do emulador: boot completo + snapshot "boot_p31"
# (uma vez; ciclos seguintes carregam o snapshot — boot em segundos)
# Tudo em UMA chamada foreground; log em arquivo p/ sobreviver a timeout.
# =============================================================================
set -uo pipefail
export LD_PRELOAD=/home/z/my-project/scripts/mmap_norescue.so
export ANDROID_AVD_HOME=/tmp/my-project/avd
EMU=/home/z/android-sdk/emulator/emulator
ADB="timeout 30 /home/z/android-sdk/platform-tools/adb"
LOG=/home/z/my-project/download/p31/bootstrap.log
mkdir -p /home/z/my-project/download/p31
exec > >(tee -a "$LOG") 2>&1

echo "[boot] iniciando em $(date +%H:%M:%S)"
"$EMU" -avd goni_p31 -no-window -no-accel -gpu swiftshader_indirect \
       -no-audio -no-boot-anim -memory 1024 -cores 2 -no-snapshot-load \
       -port 5554 > /home/z/my-project/download/p31/boot_emulator.log 2>&1 &
EMU_PID=$!
echo "[boot] emulator PID=$EMU_PID"

DEADLINE=$((SECONDS + 480))
BOOTED=0
while (( SECONDS < DEADLINE )); do
  kill -0 "$EMU_PID" 2>/dev/null || { echo "[boot] emulator MORREU"; break; }
  BC=$($ADB -s emulator-5554 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
  [[ "$BC" == "1" ]] && { BOOTED=1; break; }
  sleep 5
done
echo "[boot] boot_concluido=$BOOTED em ${SECONDS}s ($(date +%H:%M:%S))"

if [[ "$BOOTED" == "1" ]]; then
  echo "[boot] salvando snapshot boot_p31..."
  timeout 60 /home/z/android-sdk/platform-tools/adb -s emulator-5554 emu avd snapshot save boot_p31 2>&1 | tail -2
  sleep 15
  echo "[boot] snapshot salvo (verificar abaixo)"
  ls -la /tmp/my-project/avd/goni_p31.avd/snapshots/default_boot/ 2>/dev/null | head -8
fi

echo "[boot] encerrando emulator"
kill "$EMU_PID" 2>/dev/null; sleep 3; kill -9 "$EMU_PID" 2>/dev/null
timeout 10 /home/z/android-sdk/platform-tools/adb -s emulator-5554 emu kill 2>/dev/null
sleep 2
echo "[boot] fim: BOOTED=$BOOTED"
