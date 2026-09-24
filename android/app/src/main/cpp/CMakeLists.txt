# =============================================================================
# libgoni.so — alvo nativo do APK.
#
# REUSA os targets do engine via add_subdirectory — NENHUMA fonte
# duplicada. Os MESMOS runtime/AndroidRuntime.cpp testados no Linux
# (android/runtime/CMakeLists.txt) compilam aqui para arm64-v8a.
#
# Fronteira: jni.h existe APENAS em GoniJni.cpp (missão §II.4); o engine
# (eng::*) segue 100% livre de JNI/Android headers exceto os backends sob
# __ANDROID__ (native_window — NDK API, não JNI).
# =============================================================================
cmake_minimum_required(VERSION 3.28)
project(goni LANGUAGES CXX)

# Mesmas políticas do build raiz do engine.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Raiz do repositório: android/app/src/main/cpp -> ../../../.. (4 níveis:
# cpp -> main -> src -> app -> android, +1 para a raiz = 5).
get_filename_component(GONI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

list(APPEND CMAKE_MODULE_PATH "${GONI_ROOT}/cmake")
include(EngineOptions)       # opções + eng_add_module
include(EngineWarnings)       # ADR-020 (zero warnings — §XXXVI)
include(EngineSanitizers)    # off por default no Android (§XXXV)
include(EngineDependencies)  # nlohmann (build); Catch2 pulado (tests OFF)

set(ENG_BUILD_TESTS OFF CACHE BOOL "Sem testes no APK" FORCE)

# Módulos do engine (mesmos CMakeLists do Linux — sem cópias).
add_subdirectory("${GONI_ROOT}/engine" engine)
# Núcleo C++ do editor — MESMA biblioteca testada no Linux
# (eng::editor + suite Catch2); fontes JAMAIS duplicadas.
add_subdirectory("${GONI_ROOT}/editor" editor)

add_library(goni SHARED
    EditorJni.cpp
)
target_link_libraries(goni PRIVATE
    eng::rhi
    eng::rhi::vulkan
    eng::rhi::gles
    eng::editor
    eng::log
    eng::core
    android   # ANativeWindow (runtime/editor)
    log)      # __android_log_print (LogcatSink)
