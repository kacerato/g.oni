#!/bin/bash
# FASE 1 (P3.1) — Instalação completa do Android SDK no ambiente Linux
# Componentes alinhados ao que o CI Android já usa (ci-android.yml):
#   ndk;27.0.12077973 cmake;3.31.6 platforms;android-34 build-tools;34.0.0
# + platform-tools (adb), emulator e system image arm64-v8a (APK é arm64-only).
set -euo pipefail
SDK=/home/z/android-sdk
SM=$SDK/cmdline-tools/latest/bin/sdkmanager

yes | "$SM" --licenses > /dev/null 2>&1 || true

"$SM" --install \
  "platform-tools" \
  "emulator" \
  "platforms;android-34" \
  "build-tools;34.0.0" \
  "cmake;3.31.6" \
  "system-images;android-30;google_apis;arm64-v8a" \
  2>&1 | grep -v "^\[" || true

echo "=== INSTALADO ==="
ls "$SDK"
