/// TSan harness P3.5 — watchdog + espelho assíncrono + worker de áudio.
///
/// O linux-debug roda ASan/UBSan; o TSan ganha harnesses dedicados porque
/// os presets não o incluem (mesmo padrão dos jobs P3.2 e do CallbackGate
/// P3.4 — scripts/tsan_p34_harness.cpp).
///
/// Cenários (todos devem terminar SEM data race e SEM deadlock):
///  1. marks de N threads × espelho coalescido com callback lento —
///     a fila mutex+cv não pode expor race nem perder pedido final;
///  2. watchdog: arm/heartbeat/evaluate concorrentes + SIGUSR1 real
///     (o handler roda na thread pokeada — cross-check do sentinel);
///  3. CallbackGate (cobertura de regressão do P3.4 no mesmo binário);
///  4. EditorHost: onResume/onPause/destroy em ciclos rápidos — o
///     worker de áudio (NullBackend no Linux) nasce/morre sob TSan.
///
/// Compilar/rodar (ver docs/p35-hang-audio.md §Validação):
///   g++ -std=c++20 -g -fsanitize=thread -I<includes...> \
///       scripts/tsan_p35_harness.cpp <libs...> -lpthread -ldl
///   ./tsan_p35_harness
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "eng/audio/AudioAdapt.hpp"
#include "eng/editor/Diagnostics.hpp"
#include "eng/editor/EditorHost.hpp"

namespace wd = eng::editor::diag::watchdog;

namespace {

int g_failures = 0;

/// Fails fast com mensagem (harness: exit != 0).
void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("[FAIL] %s\n", what);
        ++g_failures;
    } else {
        std::printf("[ok]   %s\n", what);
    }
}

/// Cenário 1 — marks de N threads + espelho coalescido com callback lento.
void scenarioMirrorStress()
{
    std::printf("--- cenário 1: mirror async (marks × N threads) ---\n");
    std::atomic<int> calls{0};
    eng::editor::diag::setMirrorCallback(
        [](void* ud) {
            auto* c = static_cast<std::atomic<int>*>(ud);
            c->fetch_add(1);
            // callback proposadamente LENTO (MediaStore simulado).
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        },
        &calls);

    constexpr int kThreads = 6;
    constexpr int kMarksEach = 80;
    std::vector<std::thread> markers;
    for (int t = 0; t < kThreads; ++t) {
        markers.emplace_back([] {
            for (int i = 0; i < kMarksEach; ++i) {
                eng::editor::diag::mark("TSAN_P35_MIRROR", "ok", "x");
            }
        });
    }
    for (std::thread& m : markers) {
        m.join();
    }
    const bool idle = eng::editor::diag::waitMirrorIdle(10000);
    check(idle, "fila do espelho drena (10 s)");
    check(calls.load() >= 1, "ao menos um lote coalescido atendido");
    check(calls.load() <= kThreads * kMarksEach,
          "nenhuma explosão de callbacks (coalesce real)");
    eng::editor::diag::setMirrorCallback(nullptr, nullptr);
}

/// Cenário 2 — watchdog: pinger + heartbeats + evaluates concorrentes.
void scenarioWatchdog()
{
    std::printf("--- cenário 2: watchdog (arm/heartbeat/evaluate) ---\n");
    wd::arm(/*thresholdMs=*/120, /*graceMs=*/0);
    std::atomic<bool> stop{false};
    std::atomic<int> fired{0};
    std::atomic<int> beats{0};

    // "main thread" responde aos pings (heartbeats periódicos).
    std::thread beater{[&] {
        while (!stop.load()) {
            wd::heartbeat();
            beats.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }};
    // dois "pingers" avaliam concorrentemente.
    std::vector<std::thread> pingers;
    for (int p = 0; p < 2; ++p) {
        pingers.emplace_back([&] {
            while (!stop.load()) {
                if (wd::evaluate()) {
                    fired.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop.store(true);
    beater.join();
    for (std::thread& p : pingers) {
        p.join();
    }
    check(fired.load() <= 2,
          "poke no máx. 1 por episódio (heartbeats ativos)");
    // Sem heartbeat por 300 ms → evaluate deve disparar.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const bool poked = wd::evaluate();
    check(poked, "evaluate dispara após expiração sem heartbeat");
}

/// Cenário 3 — CallbackGate (regressão P3.4).
void scenarioGate()
{
    std::printf("--- cenário 3: CallbackGate (regressão P3.4) ---\n");
    eng::audio::CallbackGate gate;
    gate.reopen();
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> delivered{0};
    std::vector<std::thread> callbacks;
    for (int t = 0; t < 3; ++t) {
        callbacks.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                if (gate.tryEnter()) {
                    delivered.fetch_add(1);
                    gate.exit();
                }
            }
        });
    }
    for (int cycle = 0; cycle < 50; ++cycle) {
        gate.close();
        check(gate.waitDrained(150), "gate drena em 150 ms");
        gate.reopen();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    for (std::thread& c : callbacks) {
        c.join();
    }
    std::printf("    entregas: %llu\n",
                static_cast<unsigned long long>(delivered.load()));
}

/// Cenário 4 — EditorHost: ciclos rápidos de resume/pause + destruição.
void scenarioHostAudio()
{
    std::printf("--- cenário 4: EditorHost (worker de áudio) ---\n");
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto created = eng::editor::EditorHost::create("gles", ".tsan_p35_ws");
        if (created.isError()) {
            check(false, "EditorHost::create");
            return;
        }
        std::unique_ptr<eng::editor::EditorHost> host{created.value()};
        host->onResume();   // spawn do worker + retry (NullBackend: ok)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        host->onPause();    // época++ aborta o ciclo
        host->onResume();   // novo pedido
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // destrutor: cancel + join + stop (o caminho do teardown).
    }
    check(true, "5 ciclos resume/pause/destroy sem deadlock");
}

}  // namespace

int main()
{
    scenarioMirrorStress();
    scenarioWatchdog();
    scenarioGate();
    scenarioHostAudio();
    std::printf("\n%s\n", g_failures == 0
                               ? "TSAN P3.5: TODOS os cenários OK"
                               : "TSAN P3.5: FALHAS");
    return g_failures == 0 ? 0 : 1;
}
