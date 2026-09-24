#!/usr/bin/env bash
# TSan harness P3.5 — watchdog + espelho assíncrono + worker de áudio.
# Requer o build linux-debug já configurado (libs estáticas + deps).
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=build/tsan
OUT=/tmp/tsan_p35_harness

g++ -std=c++20 -g -fsanitize=thread \
  -Ieditor/include \
  -Iengine/niscript/include -Iengine/core/include -Iengine/math/include \
  -Iengine/ecs/include -Iengine/reflect/include -Iengine/tick/include \
  -Iengine/scene/include -Iengine/physics/include -Iengine/animation/include \
  -Iengine/particles/include -Iengine/serial/include -Iengine/project/include \
  -Iengine/fs/include -Iengine/assets/include -Iengine/image/include \
  -Iengine/input/include -Iengine/audio/include -Iengine/render/include \
  -Iengine/rhi/include -Iengine/log/include \
  -isystem "$BUILD/_deps/nlohmann_json-src/include" \
  scripts/tsan_p35_harness.cpp -o "$OUT" \
  -Wl,--start-group \
  $BUILD/editor/libeng_editor.a \
  $BUILD/engine/niscript/libeng_niscript.a \
  $BUILD/engine/scene/libeng_scene.a \
  $BUILD/engine/physics/libeng_physics.a \
  $BUILD/engine/animation/libeng_animation.a \
  $BUILD/engine/particles/libeng_particles.a \
  $BUILD/engine/tick/libeng_tick.a \
  $BUILD/engine/assets/libeng_assets.a \
  $BUILD/engine/serial/libeng_serial.a \
  $BUILD/engine/project/libeng_project.a \
  $BUILD/engine/platform/libeng_platform.a \
  $BUILD/engine/audio/libeng_audio.a \
  $BUILD/engine/render/libeng_render.a \
  $BUILD/engine/rhi/libeng_rhi.a \
  $BUILD/engine/rhi/backends/vulkan/libeng_rhi_vulkan.a \
  $BUILD/engine/rhi/backends/gles/libeng_rhi_gles.a \
  $BUILD/engine/input/libeng_input.a \
  $BUILD/engine/image/libeng_image.a \
  $BUILD/engine/fs/libeng_fs.a \
  $BUILD/engine/reflect/libeng_reflect.a \
  $BUILD/engine/ecs/libeng_ecs.a \
  $BUILD/engine/events/libeng_events.a \
  $BUILD/engine/jobs/libeng_jobs.a \
  $BUILD/engine/math/libeng_math.a \
  $BUILD/engine/core/libeng_core.a \
  $BUILD/engine/log/libeng_log.a \
  $BUILD/engine/mem/libeng_mem.a \
  -Wl,--end-group \
  -lpthread -ldl

rm -rf .tsan_p35_ws || true
echo "=== executando sob TSan ==="
"$OUT"
