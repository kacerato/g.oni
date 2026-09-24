/// TSan harness do CallbackGate + LinearResampler (P3.4) — o mesmo
/// padrão do suite de jobs (FASE 2): o linux-debug roda ASan/UBSan, o
/// TSan ganha um harness dedicado porque os presets não o incluem.
///
/// Cenário: N "callbacks" (threads) disputam o gate enquanto o "dono"
/// alterna close/reopen — o protocolo de drenagem não pode expor data
/// race (TSan) nem deadlock (timeout do próprio harness via
/// waitDrained limitado).
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "eng/audio/AudioAdapt.hpp"

int main()
{
    using namespace eng::audio;

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> delivered{0};
    CallbackGate gate;
    gate.reopen();

    // "Thread de áudio": entra/sai continuamente (o callback real).
    std::vector<std::thread> audio;
    for (int t = 0; t < 3; ++t) {
        audio.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                if (gate.tryEnter()) {
                    ++delivered;
                    gate.exit();
                }
                std::this_thread::sleep_for(std::chrono::microseconds{50});
            }
        });
    }

    // "Thread do host": ciclos stop()/start() com drenagem.
    for (int cycle = 0; cycle < 50; ++cycle) {
        gate.close();
        if (!gate.waitDrained(200)) {
            std::fprintf(stderr, "FALHA: drenagem estourou no ciclo %d\n",
                         cycle);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::microseconds{100});
        gate.reopen();
    }
    stop.store(true);
    for (auto& t : audio) {
        t.join();
    }

    // Sanity do resampler no mesmo binário (sem race — só execução).
    LinearResampler r;
    if (!r.configure(48000, 44100, 2, 128)) {
        std::fprintf(stderr, "FALHA: configure\n");
        return 1;
    }
    std::vector<float> stage(300 * 2, 0.25f);
    std::vector<float> dst(128 * 2);
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t push = r.framesToPush(128);
        if (push > 0) {
            stage.assign(static_cast<std::size_t>(push) * 2, 0.25f);
            r.push(stage.data(), push);
        }
        r.produce(128, dst.data());
    }
    if (r.produced() != 100 * 128) {
        std::fprintf(stderr, "FALHA: produced=%llu\n",
                     static_cast<unsigned long long>(r.produced()));
        return 1;
    }

    std::printf("OK: gate drenou 50/50 ciclos, %llu entregas, resampler "
                "produziu %llu\n",
                static_cast<unsigned long long>(delivered.load()),
                static_cast<unsigned long long>(r.produced()));
    return 0;
}
