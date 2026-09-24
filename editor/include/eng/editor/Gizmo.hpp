#pragma once

/// eng::editor::TransformGizmo — gizmo 2D de transformação.
///
/// Camada de DADOS/LÓGICA PURA (sem GPU, sem documento): recebe o
/// Viewport (conversões tela↔mundo) + os bounds da entidade
/// selecionada e devolve (a) hit-test dos handles, (b) transform alvo
/// durante o drag, (c) geometria de desenho (quads/segmentos em MUNDO)
/// para o ViewportRenderer.
///
/// Ferramentas: Select/Move/Rotate/Scale vivem no
/// EditorDocument; o gizmo só desenha/age nas três de transformação.
///
/// Convenções:
///   - posição/escala em unidades de MUNDO; rotação em GRAUS (a mesma
///     convenção Euler do EditorDocument::TransformDesc — Inspector em
///     graus, cena em Quat);
///   - handles têm tamanho CONSTANTE EM TELA (px convertidos a mundo
///     pelo zoom da câmera EM FOCO no momento da operação);
///   - arraste devolve o TRANSFORM ALVO (estado absoluto), não delta —
///     o chamador aplica. Uma fonte de verdade: o ECS.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "eng/editor/Viewport.hpp"

namespace eng::editor {

/// Ferramenta ativa do editor. Pan/zoom continuam gestos de
/// navegação SEMPRE disponíveis (drag em espaço vazio / pinch) — não
/// são ferramentas de autoria.
enum class EditorTool : std::uint8_t {
    Select = 0, ///< tap seleciona; drag = pan
    Move,        ///< gizmo de movimento (eixo X, eixo Y, centro)
    Rotate,      ///< gizmo de rotação (anel + handle)
    Scale        ///< gizmo de escala (4 cantos)
};

/// Handle do gizmo — alvo do toque/drag.
enum class GizmoHandle : std::uint8_t {
    None = 0,
    MoveCenter,  ///< move livre (X+Y)
    MoveAxisX,   ///< constrain ao eixo X do mundo
    MoveAxisY,   ///< constrain ao eixo Y do mundo
    RotateRing,  ///< rotação (ângulo pointer↔pivot)
    ScaleNE,     ///< canto nordeste do bounds
    ScaleNW,
    ScaleSE,
    ScaleSW,
    ScaleEdgeE,  ///< P4.1 (D4): aresta LESTE — escala só no eixo X
    ScaleEdgeW,  ///< aresta OESTE — escala só no eixo X
    ScaleEdgeN,  ///< aresta NORTE — escala só no eixo Y
    ScaleEdgeS   ///< aresta SUL — escala só no eixo Y
};

/// Bounds da entidade selecionada no plano do MUNDO — layout do gizmo
/// e clamps de escala usam EXATAMENTE o tamanho desenhado (posição,
/// rotação, escala, textura e ppu — P1.2), nunca um tamanho arbitrário.
///
/// Dois pontos de referência distintos, ambos derivados do
/// estado ATUAL da entidade (nenhum dado temporário):
///   - worldX/worldY: CENTRO VISUAL (com offset de pivot do sprite) —
///     pivô do ROTATE, cantos do SCALE e desenho do gizmo;
///   - originX/originY: ORIGEM DO NÓ (translation do world matrix) —
///     alvo do MOVE (a posição que o Transform guarda).
struct GizmoBounds {
    float worldX{0.f};   ///< centro visual (posição + offset de pivot)
    float worldY{0.f};
    float originX{0.f};  ///< origem do NÓ em mundo (sem pivot — MOVE)
    float originY{0.f};
    float halfW{0.5f};   ///< MEIA-largura desenhada (mundo)
    float halfH{0.5f};
    float rotation{0.f}; ///< radianos no plano XY
    bool valid{false};   ///< false → gizmo não desagina nem acerta
};

/// Estado TRS que o gizmo lê/escreve (graus — convenção do Inspector).
/// PosX/posY são a posição de MUNDO da ORIGEM do nó — o DOCUMENTO
/// converte o delta de mundo para o espaço LOCAL do pai (filhos de pais
/// rotacionados/escalados movem no eixo de TELA certo).
struct GizmoTransform {
    float posX{0.f};
    float posY{0.f};
    float rotationDeg{0.f};
    float scaleX{1.f};
    float scaleY{1.f};
};

/// Quad preenchido do gizmo, em MUNDO (renderer converte p/ clip).
struct GizmoQuad {
    float worldX{0.f};
    float worldY{0.f};
    float halfW{1.f};
    float halfH{1.f};
    float rotation{0.f};
    float r{1.f};
    float g{1.f};
    float b{1.f};
};

/// Triângulo preenchido do gizmo, em
/// MUNDO. Aponta para o +X LOCAL da rotação (radianos): halfW =
/// comprimento centro→ápice, halfH = meia-base. Substitui os "quadrados
/// girados" que o round 6 leu como cubo.
struct GizmoTriangle {
    float worldX{0.f};
    float worldY{0.f};
    float halfW{1.f};
    float halfH{1.f};
    float rotation{0.f};
    float r{1.f};
    float g{1.f};
    float b{1.f};
};

/// Segmento do gizmo (eixos/anel), em MUNDO.
struct GizmoSegment {
    float x0{0.f};
    float y0{0.f};
    float x1{0.f};
    float y1{0.f};
    float r{1.f};
    float g{1.f};
    float b{1.f};
};

/// Pacote de desenho do gizmo (quads + triângulos + segmentos em MUNDO)
/// — o documento produz, o renderer consome.
struct GizmoDrawData {
    std::vector<GizmoQuad> quads;
    std::vector<GizmoTriangle> triangles;
    std::vector<GizmoSegment> segments;
};

class TransformGizmo final {
public:
    /// Dimensões de UI em DP:
    /// constantes em ZOOM e convertidas a px da surface pela densidade do
    /// viewport (uiScale). Regras da missão P4.1:
    ///   - handle visual 28–40 px independentes de zoom;
    ///   - alvo de toque ≥ 48 dp (raio 24 dp → diâmetro 48 dp);
    ///   - anel de rotação ≥ 64 px de raio (era minúsculo — D3).
    static constexpr float kHandleDp = 32.f;   ///< lado do handle visual
    static constexpr float kArrowHeadDp = 40.f; ///< ponta de seta (MOVE)
    static constexpr float kEdgeDp = 24.f;     ///< marca de aresta (SCALE)
    static constexpr float kEdgeArrowDp = 28.f; ///< seta de aresta (SCALE, P4.6/L3)
    static constexpr float kAxisDp = 96.f;     ///< comprimento do eixo
    static constexpr float kRingPadDp = 26.f;  ///< folga do anel p/ fora
    static constexpr float kRingMinDp = 64.f;  ///< raio mínimo do anel
    static constexpr float kHitDp = 24.f;      ///< raio de acerto (48dp ⌀)
    // Setas REAIS + anti-sobreposição:
    static constexpr float kHeadTriLenDp = 16.f; ///< comprimento do triângulo (12–16dp)
    static constexpr float kHeadTriBaseDp = 14.f; ///< base do triângulo
    static constexpr float kShaftDp = 2.f;       ///< espessura da haste
    static constexpr float kHaloDp = 1.f;        ///< halo/rim de 1dp
    static constexpr float kMinCornerCenterDp = 52.f; ///< cantos: distância mínima do centro
    static constexpr float kMinEdgeCenterDp = 44.f;   ///< arestas: distância mínima do centro
    /// Snap de rotação: 15° com ímã de 4° (opcional, previsível).
    static constexpr float kRotateSnapStepDeg = 15.f;
    static constexpr float kRotateSnapPullDeg = 4.f;
    /// Escala — impedir valores inválidos.
    static constexpr float kScaleMin = 0.01f;
    static constexpr float kScaleMax = 100.f;

    // --- métricas em px da SURFACE (dp × densidade — constantes em zoom) ----

    /// Lado visual do handle em px (clamp 28–40 px pela regra da missão).
    [[nodiscard]] static float handlePx(float uiScale) noexcept
    {
        return std::clamp(kHandleDp * uiScale, 28.f, 40.f);
    }
    /// Comprimento TOTAL do triângulo de seta (P4.7.0 B2) em px —
    /// halfW do GizmoTriangle é a METADE disto (centro→ápice).
    [[nodiscard]] static float headTriLenPx(float uiScale) noexcept
    {
        return std::clamp(kHeadTriLenDp * uiScale, 14.f, 20.f);
    }
    /// Base do triângulo de seta em px (meia-base = halfH).
    [[nodiscard]] static float headTriBasePx(float uiScale) noexcept
    {
        return std::clamp(kHeadTriBaseDp * uiScale, 12.f, 18.f);
    }
    /// Halo/rim dos handles em px (P4.7.0 B2: 1dp mínimo).
    [[nodiscard]] static float haloPx(float uiScale) noexcept
    {
        return std::max(kHaloDp * uiScale, 1.f);
    }
    /// Lado visual da ponta de seta (MOVE) em px.
    [[nodiscard]] static float arrowHeadPx(float uiScale) noexcept
    {
        return std::clamp(kArrowHeadDp * uiScale, 34.f, 48.f);
    }
    /// Lado visual da marca de aresta (SCALE) em px.
    [[nodiscard]] static float edgePx(float uiScale) noexcept
    {
        return std::clamp(kEdgeDp * uiScale, 20.f, 30.f);
    }
    /// Lado visual da seta de aresta (SCALE — P4.6/L3) em px.
    [[nodiscard]] static float edgeArrowPx(float uiScale) noexcept
    {
        return std::clamp(kEdgeArrowDp * uiScale, 22.f, 34.f);
    }
    /// Comprimento do eixo (MOVE) em px.
    [[nodiscard]] static float axisPx(float uiScale) noexcept
    {
        return kAxisDp * uiScale;
    }
    /// Raio de acerto em px (diâmetro = 48 dp — alvo de dedo).
    [[nodiscard]] static float hitPx(float uiScale) noexcept
    {
        return kHitDp * uiScale;
    }
    /// Raio do anel de rotação em px: max(borda dos bounds, 64 px).
    [[nodiscard]] static float ringRadiusPx(float boundsRadiusPx,
                                             float uiScale) noexcept
    {
        return std::max(boundsRadiusPx + kRingPadDp * uiScale,
                        kRingMinDp * uiScale);
    }

    // --- hit-test -------------------------------------------------------------

    /// Handle sob o toque (px de tela). Invalid bounds / tool sem gizmo
    /// → None. Handles têm precedência sobre o corpo da entidade — a
    /// Activity consulta isto ANTES do viewportTap.
    [[nodiscard]] GizmoHandle hitTest(const Viewport& viewport,
                                     EditorTool tool,
                                     const GizmoBounds& bounds,
                                     float screenX, float screenY) const;

    // --- drag ------------------------------------------------------

    /// Captura o estado inicial (transform + ponto de agarre). O drag é
    /// uma operação ATÔMICA: begin → dragTo* → endDrag.
    void beginDrag(GizmoHandle handle, const GizmoTransform& startTransform,
                   const Viewport& viewport, const GizmoBounds& bounds,
                   float screenX, float screenY);

    /// Transform ALVO para a posição do pointer. Sem drag ativo →
    /// devolve o transform inicial inalterado. NÃO-const: a
    /// rotação ACUMULA o ângulo por evento (ver accumulatedRotationDeg_)
    /// — o drag é a única fonte do estado do gesto.
    [[nodiscard]] GizmoTransform dragTo(const Viewport& viewport,
                                        const GizmoBounds& bounds,
                                        float screenX, float screenY);

    void endDrag() noexcept
    {
        active_ = GizmoHandle::None;
        // O acumulador de rotação morre com o drag — nenhum
        // estado de gesto atravessa re-armo (regra P4.1/D1 mantida).
        accumulatedRotationDeg_ = 0.f;
        lastAngleRad_ = 0.f;
    }
    [[nodiscard]] GizmoHandle activeHandle() const noexcept
    {
        return active_;
    }
    [[nodiscard]] bool dragging() const noexcept
    {
        return active_ != GizmoHandle::None;
    }

    // --- desenho ----------------------------------------------------------------

    /// Transição entre ferramentas — POP de 120ms (ease-out
    /// cúbico; 0.88 → 1.0). Função PURA do tempo decorrido (testável sem
    /// clock); o renderer só multiplica os halfes dos handles.
    static constexpr float kToolTransitionMs = 120.f;
    [[nodiscard]] static float transitionScale(
        float elapsedMs) noexcept
    {
        if (!(elapsedMs >= 0.f) || elapsedMs >= kToolTransitionMs) {
            return 1.f;
        }
        const float t = elapsedMs / kToolTransitionMs;
        const float eased = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
        return 0.88f + 0.12f * eased;
    }

    /// Geometria da ferramenta (em MUNDO) para o renderer. Vazia quando
    /// a ferramenta não tem gizmo (Select) ou bounds inválido.
    [[nodiscard]] std::vector<GizmoQuad> layoutQuads(
        const Viewport& viewport, EditorTool tool,
        const GizmoBounds& bounds) const;
    [[nodiscard]] std::vector<GizmoTriangle> layoutTriangles(
        const Viewport& viewport, EditorTool tool,
        const GizmoBounds& bounds) const;
    [[nodiscard]] std::vector<GizmoSegment> layoutSegments(
        const Viewport& viewport, EditorTool tool,
        const GizmoBounds& bounds) const;

    /// Posições dos handles de SCALE com o CLAMP
    /// anti-sobreposição: bounds pequenos colapsariam a esquina dos
    /// cantos sobre o centro (round 6). Cantos/arestas são EMPURRADOS
    /// para fora até a distância mínima do centro (kMinCornerCenterDp /
    /// kMinEdgeCenterDp). Hit-test e desenho usam ESTA MESMA fonte — o
    /// toque sempre coincide com o visual. Drag continua 1:1 (o drag
    /// segue o POINTER, não a posição do handle).
    struct HandlePoints {
        std::pair<float, float> ne, nw, se, sw; ///< cantos
        std::pair<float, float> e, w, n, s;     ///< arestas
    };
    [[nodiscard]] HandlePoints scaleHandlePoints(
        const Viewport& viewport, const GizmoBounds& bounds) const;

    /// Cores canônicas (X vermelho, Y verde, centro amarelo, rotação
    /// ciano, escala âmbar — mesmas em hit-test, drag e desenho).
    static constexpr float kXAxisR = 0.92f, kXAxisG = 0.33f, kXAxisB = 0.28f;
    static constexpr float kYAxisR = 0.36f, kYAxisG = 0.80f, kYAxisB = 0.40f;
    static constexpr float kCenterR = 0.96f, kCenterG = 0.82f, kCenterB = 0.30f;
    static constexpr float kRotateR = 0.32f, kRotateG = 0.78f, kRotateB = 0.92f;
    static constexpr float kScaleR = 0.96f, kScaleG = 0.62f, kScaleB = 0.28f;

private:
    GizmoHandle active_ = GizmoHandle::None;
    GizmoTransform start_{};
    float grabWorldX_ = 0.f;   ///< ponto de agarre em MUNDO (move)
    float grabWorldY_ = 0.f;
    float startAngleRad_ = 0.f; ///< ângulo pointer↔pivot no begin (rotate)
    /// O ANGULO TOTAL era
    /// normalizado contra o ponto de agarre fixo — dedo além de 180°
    /// flipava o sinal e a entidade girava PARA TRÁS. Agora cada evento
    /// contribui com o DELTA curto (sempre <180°) acumulado aqui; a soma
    /// dá quantas voltas o dedo der.
    float accumulatedRotationDeg_ = 0.f;
    float lastAngleRad_ = 0.f;  ///< ângulo do ÚLTIMO evento (delta curto)
    float startPointerLocalX_ = 1.f; ///< pointer no frame LOCAL do nó (scale)
    float startPointerLocalY_ = 1.f;
};

}  // namespace eng::editor
