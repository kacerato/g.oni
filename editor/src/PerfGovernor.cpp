/// Implementação do governor (ver PerfGovernor.hpp).

#include "eng/editor/PerfGovernor.hpp"

#include <cmath>

namespace eng::editor {

namespace {

/// A escada (índice 0 = High). Levers documentados no header — B6 aplica
/// renderScale e maxLights no caminho atual; post/bloom/luz-textura são
/// levers REAIS a partir da P4.7.1 (o preset já os carrega).
constexpr PerfPreset kLadder[3] = {
    PerfPreset{1.00f, 512u, true, true, 8u},   // High
    PerfPreset{0.85f, 256u, true, true, 4u},   // Med
    PerfPreset{0.70f, 128u, false, true, 2u},  // Low
};
constexpr std::uint32_t kLadderSize =
    sizeof(kLadder) / sizeof(kLadder[0]);

} // namespace

PerfPreset PerfGovernor::preset() const noexcept
{
    return kLadder[preset_];
}

void PerfGovernor::stepDown() noexcept
{
    if (preset_ + 1 < kLadderSize) {
        ++preset_;
    }
}

void PerfGovernor::stepUp() noexcept
{
    if (preset_ > 0) {
        --preset_;
    }
}

void PerfGovernor::reset() noexcept
{
    emaMs_ = 0.f;
    emaSeeded_ = false;
    thermal_ = ThermalLevel::Unknown;
    preset_ = 0;
    badStreak_ = 0;
    goodStreak_ = 0;
}

void PerfGovernor::onFrame(float frameMs, ThermalLevel thermal)
{
    // Térmico: sempre o mais recente; severo escala NA HORA (uma vez
    // crítico, não há "bom frame" que compense — o chip está quente).
    thermal_ = thermal;
    if (thermal == ThermalLevel::Severe || thermal == ThermalLevel::Critical
        || thermal == ThermalLevel::Emergency
        || thermal == ThermalLevel::Shutdown) {
        preset_ = kLadderSize - 1; // Low imediato
        badStreak_ = 0;
        goodStreak_ = 0;
        // SEM return: o frame ainda alimenta o EMA (métrica honesta).
    }

    if (!std::isfinite(frameMs) || frameMs <= 0.f) {
        return; // pausa/surface morta — nem bom nem ruim
    }

    // EMA simples (janela kEmaWindow — 1/n, n limitado).
    emaMs_ = emaSeeded_ ? emaMs_ + (frameMs - emaMs_)
                              / static_cast<float>(kEmaWindow)
                        : frameMs;
    emaSeeded_ = true;

    // Máquina de histerese: streaks CONSECUTIVOS nas duas direções.
    if (emaMs_ > kBadMs) {
        ++badStreak_;
        goodStreak_ = 0;
        if (badStreak_ >= kBadFrames) {
            stepDown();
            badStreak_ = 0; // desceu — recomeça a observação (não desce
                            // vários degraus de uma vez: o efeito do
                            // preset novo precisa de janela própria)
        }
        return;
    }
    if (emaMs_ < kGoodMs) {
        // Térmico ainda quente NUNCA sobe (mesmo com frame bom —
        // voltar com o chip quente é o que causa oscilação).
        if (thermal_ != ThermalLevel::Unknown
            && static_cast<std::int8_t>(thermal_)
                   >= static_cast<std::int8_t>(ThermalLevel::Moderate)) {
            goodStreak_ = 0;
            return;
        }
        ++goodStreak_;
        badStreak_ = 0;
        if (goodStreak_ >= kGoodFrames) {
            stepUp();
            goodStreak_ = 0;
        }
        return;
    }
    // Zona neutra (entre os alvos): mantém o degrau — nem streak conta.
    badStreak_ = 0;
    goodStreak_ = 0;
}

} // namespace eng::editor
