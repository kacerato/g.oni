#pragma once

/// eng::tick::CameraData — câmera de jogo 2D como CIDADÃ DA CENA
/// (evolução P0-5; ADR-051 — "CameraTick").
///
/// - Antes da P0-5 a câmera era estado do EDITOR (Viewport::Camera2D) —
///   um jogo exportado não tinha como definir a própria câmera. Agora a
///   câmera é um componente: o entity system resolve a ATIVA e o viewport
///   do editor a SEGUE em Play (hit-test e arraste continuam corretos —
///   todas as conversões world↔screen passam pela câmera em foco).
/// - Ortográfica 2D: posX/posY em unidades de mundo, zoom em
///   PIXELS POR UNIDADE (mesma semântica da câmera do editor — conversão
///   direta). Rotação/perspectiva ficam para o rework de câmera 3D:
///   campo sem consumidor seria API inerte (missão proíbe).
/// - Várias câmeras: a PRIMEIRA ativa em ordem estável (índice de criação)
///   vence; o CameraTickSystem AVISA ambiguidade (dados não ficam em
///   silêncio). `active=false` desliga sem remover.

#include <string>

#include "eng/ecs/Ecs.hpp"
#include "eng/scene/Scene.hpp"
#include "eng/tick/Tick.hpp"

namespace eng::tick {

/// Componente câmera de jogo (refletido → Inspector + persistência +
/// clone no Play — ADR-043).
struct CameraData {
    float posX{0.f};
    float posY{0.f};
    /// Pixels por unidade de mundo (idem Viewport::Camera2D::zoom).
    float zoom{48.f};
    /// Câmera desligada sem remover (a resolução ignora).
    bool active{true};

    // --- P4.7.0 Bloco 4 — câmera 2D completa (ADITIVOS no fim; defaults
    // = comportamento pré-P4.7 — cenas antigas decodificam sem pre-pass
    // de dados, migration idempotente por defaults) -------------------
    /// Rotação da VISTA em graus CCW (0 = sem rotação — compatível).
    float rotationDeg{0.f};
    /// Nome da entidade seguida (primeira correspondência de Name na
    /// cena; "" = câmera livre — comportamento pré-P4.7).
    std::string followName{};
    /// Zona morta do follow em unidades de mundo (0 = off): o alvo pode
    /// andar dentro do retângulo sem mover a câmera.
    float deadzoneW{0.f};
    float deadzoneH{0.f};
    /// Constante de tempo da suavização exponencial em SEGUNDOS (0 =
    /// snap — pré-P4.7). Maior = câmera mais lenta/pesada.
    float smoothingTime{0.f};
    /// Limites do MUNDO: o retângulo VISÍVEL (pós-zoom) fica dentro de
    /// [limitMin, limitMax]. false = sem limites (pré-P4.7).
    bool limitsEnabled{false};
    float limitMinX{0.f};
    float limitMinY{0.f};
    float limitMaxX{0.f};
    float limitMaxY{0.f};
};

/// Câmera ativa resolvida — ou nenhuma. Dados POR VALOR: ponteiros de
/// pool não sobrevivem a emplaces do mesmo tipo (dense array realoca,
/// ADR-024) — reter `const CameraData*` entre mutações da cena seria
/// armadilha de use-after-free.
struct ActiveCamera {
    eng::ecs::Entity entity{};
    CameraData data{};

    /// Havia uma câmera ativa? (o default de CameraData não responde
    /// isso — `has` é explícito.)
    bool has = false;

    [[nodiscard]] bool found() const noexcept { return has; }
};

/// Resolve a câmera ATIVA da cena: primeira CameraData ativa em ordem
/// estável (índice de criação — cada<CameraData> itera o pool na ordem de
/// inserção, ADR-024). Determinística. Sem câmera → {.has = false}.
[[nodiscard]] ActiveCamera resolveActiveCamera(
    const eng::scene::Scene& scene);

/// Contagem de câmeras ATIVAS (diagnóstico de ambiguidade — o tick avisa
/// quando > 1).
[[nodiscard]] std::size_t activeCameraCount(
    const eng::scene::Scene& scene);

/// CameraTick (fase PreRender): cacheia a câmera ativa do frame e AVISA
/// ambiguidade. Consumidores com scheduler consultam o cache; sem
/// scheduler, `resolveActiveCamera` direto.
///
/// Pipeline do follow: DEADZONE → SMOOTHING → CLAMP
/// pós-zoom (ordem fixa documentada). O resultado final (posição
/// suavizada/limitada) é ESCRITO no cache `activeCamera()` — o editor e
/// o runtime consomem a posição FINAL, nunca a base.
class CameraTickSystem final : public TickSystem {
public:
    [[nodiscard]] const char* name() const override { return "CameraTick"; }
    [[nodiscard]] Phase phase() const override { return Phase::PreRender; }

    void tick(eng::scene::Scene& scene, float dt) override;

    /// Tamanho da VISTA em px (o dono do scheduler informa por frame —
    /// o editor tem o viewport; sem hint, o clamp pós-zoom é no-op).
    void setViewSize(float width, float height) noexcept
    {
        viewW_ = width;
        viewH_ = height;
    }

    /// Câmera ativa do ÚLTIMO frame (antes do primeiro tick: vazia).
    [[nodiscard]] const ActiveCamera& activeCamera() const noexcept
    {
        return active_;
    }

private:
    ActiveCamera active_{};
    /// Estado da suavização (posição atual da câmera em mundo).
    bool hasSmoothed_ = false;
    float smoothedX_ = 0.f;
    float smoothedY_ = 0.f;
    eng::ecs::Entity smoothedEntity_{0xFFFFFFFFu, 0xFFFFFFFFu};
    float viewW_ = 0.f;
    float viewH_ = 0.f;
};

}  // namespace eng::tick

/// Reflexão (ADR-043: campos por caminho — Inspector/serializer).
ENG_REFLECT_BEGIN(eng::tick::CameraData)
    ENG_REFLECT_FIELD(posX)
    ENG_REFLECT_FIELD(posY)
    ENG_REFLECT_FIELD(zoom)
    ENG_REFLECT_FIELD(active)
    ENG_REFLECT_FIELD(rotationDeg)
    ENG_REFLECT_FIELD(followName)
    ENG_REFLECT_FIELD(deadzoneW)
    ENG_REFLECT_FIELD(deadzoneH)
    ENG_REFLECT_FIELD(smoothingTime)
    ENG_REFLECT_FIELD(limitsEnabled)
    ENG_REFLECT_FIELD(limitMinX)
    ENG_REFLECT_FIELD(limitMinY)
    ENG_REFLECT_FIELD(limitMaxX)
    ENG_REFLECT_FIELD(limitMaxY)
ENG_REFLECT_END()
