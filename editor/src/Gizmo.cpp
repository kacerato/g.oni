#include "eng/editor/Gizmo.hpp"

/// TransformGizmo — implementação (P1; P4.1 re-trabalho de usabilidade).
///
/// Hit-test + matemática de drag + layout de desenho. Ver header para
/// as decisões (handles constantes em TELA escalados pela densidade;
/// drag devolve estado ALVO; bounds = tamanho desenhado da entidade).
///
/// Alvos de toque em dp (≥48dp de diâmetro), anel de
/// rotação com raio mínimo de 64 px, MOVE com 4 setas + quadrado
/// central, ROTATE com handle visível + ponta de seta no anel, SCALE
/// com 4 cantos + 4 marcas de aresta (escala de UM eixo).

#include <algorithm>
#include <cmath>
#include <utility>

namespace eng::editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;

[[nodiscard]] float pxToWorld(const Viewport& viewport, float px) noexcept
{
    const float zoom = viewport.effectiveCamera().zoom;
    return zoom > 0.f ? px / zoom : 0.f;
}

/// Distância em TELA entre (screenX,screenY) e um ponto de MUNDO.
[[nodiscard]] float screenDistanceTo(const Viewport& viewport, float worldX,
                                     float worldY, float screenX,
                                     float screenY) noexcept
{
    const float dx = viewport.worldToScreenX(worldX) - screenX;
    const float dy = viewport.worldToScreenY(worldY) - screenY;
    return std::sqrt(dx * dx + dy * dy);
}

/// Distância em TELA de (screenX,screenY) ao SEGMENTO de mundo a—b
/// (P4.2/B-D: as HASTES das setas de movimento viram alvo — tocar na
/// haste em vez da pontinha não cai mais no fallback de scroll).
[[nodiscard]] float screenDistanceToSegment(const Viewport& viewport,
                                            float aWorldX, float aWorldY,
                                            float bWorldX, float bWorldY,
                                            float screenX,
                                            float screenY) noexcept
{
    const float ax = viewport.worldToScreenX(aWorldX);
    const float ay = viewport.worldToScreenY(aWorldY);
    const float bx = viewport.worldToScreenX(bWorldX);
    const float by = viewport.worldToScreenY(bWorldY);
    const float abx = bx - ax;
    const float aby = by - ay;
    const float apx = screenX - ax;
    const float apy = screenY - ay;
    const float abLen2 = abx * abx + aby * aby;
    float t = abLen2 > 0.f
                  ? (apx * abx + apy * aby) / abLen2
                  : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const float dx = apx - abx * t;
    const float dy = apy - aby * t;
    return std::sqrt(dx * dx + dy * dy);
}

/// Ponto do anel de rotação no ângulo atual da entidade (o handle nasce
/// "amarrado" à rotação — girar é pegar e balançar, sem salto).
[[nodiscard]] std::pair<float, float> ringHandleWorld(const GizmoBounds& b,
                                                     float radiusWorld) noexcept
{
    const float x = b.worldX + radiusWorld * std::cos(b.rotation);
    const float y = b.worldY + radiusWorld * std::sin(b.rotation);
    return {x, y};
}

/// Cantos do bounds GIRADOS pela rotação da entidade (P1.5: os handles
/// de escala seguem o retângulo real desenhado, não o AABB).
struct CornerPoints {
    std::pair<float, float> ne, nw, se, sw;
};

[[nodiscard]] CornerPoints cornersOf(const GizmoBounds& b) noexcept
{
    const float c = std::cos(b.rotation);
    const float s = std::sin(b.rotation);
    auto corner = [&](float lx, float ly) {
        return std::pair<float, float>{
            b.worldX + lx * c - ly * s, b.worldY + lx * s + ly * c};
    };
    return CornerPoints{corner(b.halfW, b.halfH), corner(-b.halfW, b.halfH),
                        corner(b.halfW, -b.halfH), corner(-b.halfW, -b.halfH)};
}

/// Centro das ARESTAS do bounds na rotação da entidade (P4.1/D4 — os
/// novos handles de escala de um eixo: E(+halfW,0) W(-halfW,0)
/// N(0,+halfH) S(0,-halfH)).
struct EdgePoints {
    std::pair<float, float> e, w, n, s;
};

[[nodiscard]] EdgePoints edgesOf(const GizmoBounds& b) noexcept
{
    const float c = std::cos(b.rotation);
    const float s = std::sin(b.rotation);
    auto point = [&](float lx, float ly) {
        return std::pair<float, float>{
            b.worldX + lx * c - ly * s, b.worldY + lx * s + ly * c};
    };
    return EdgePoints{point(b.halfW, 0.f), point(-b.halfW, 0.f),
                      point(0.f, b.halfH), point(0.f, -b.halfH)};
}

/// Snap de rotação: múltiplos de kRotateSnapStepDeg com ímã de
/// kRotateSnapPullDeg.
[[nodiscard]] float snappedDegrees(float degrees) noexcept
{
    const float step = TransformGizmo::kRotateSnapStepDeg;
    const float pull = TransformGizmo::kRotateSnapPullDeg;
    const float nearest = std::round(degrees / step) * step;
    return (std::abs(degrees - nearest) <= pull) ? nearest : degrees;
}

/// Clamp de escala — impedir valores inválidos.
[[nodiscard]] float clampScale(float value) noexcept
{
    if (!std::isfinite(value)) {
        return 1.f;
    }
    return std::clamp(value, TransformGizmo::kScaleMin, TransformGizmo::kScaleMax);
}

/// true quando o handle é um dos cantos do SCALE.
[[nodiscard]] bool isCornerHandle(GizmoHandle h) noexcept
{
    return h == GizmoHandle::ScaleNE || h == GizmoHandle::ScaleNW ||
           h == GizmoHandle::ScaleSE || h == GizmoHandle::ScaleSW;
}

/// true quando o handle é uma aresta do SCALE (um eixo só).
[[nodiscard]] bool isEdgeHandle(GizmoHandle h) noexcept
{
    return h == GizmoHandle::ScaleEdgeE || h == GizmoHandle::ScaleEdgeW ||
           h == GizmoHandle::ScaleEdgeN || h == GizmoHandle::ScaleEdgeS;
}

}  // namespace

// =============================================================================
// Hit-test
// =============================================================================

GizmoHandle TransformGizmo::hitTest(const Viewport& viewport, EditorTool tool,
                                    const GizmoBounds& bounds, float screenX,
                                    float screenY) const
{
    if (!bounds.valid || dragging()) {
        return GizmoHandle::None;
    }
    // ScreenDistanceTo
    // devolve PX DE TELA; o raio de acerto é comparado EM PX (hitPx ×
    // densidade) — constante no zoom. O código antigo convertia o raio
    // px→mundo (pxToWorld) e comparava 12px ≤ 0.5unidades: em zoom baixo
    // o alvo tinha ~4px (impossível de acertar — D3/D4) e em zoom alto
    // ~256px (pegava handle sem querer). Os testes antigos só passavam
    // porque tocavam no CENTRO EXATO dos handles.
    const float scale = viewport.uiScale();
    const float hitScreenPx = hitPx(scale);

    if (tool == EditorTool::Move) {
        // O CENTRO é avaliado PRIMEIRO.
        // Com alvos em dp (48px de raio na densidade 2) a ponta interna da
        // haste fica a 24px do centro — eixos primeiro faziam o raio de
        // acerto da haste COBRIR o centro: agarrar o corpo da entidade
        // arrastava só o eixo X ("o dedo não segue" no device, os DOIS
        // eixos errados). Dentro do raio do centro, o centro vence; as
        // hastes (borda → ponta) e as cabeças continuam alvos de eixo.
        if (screenDistanceTo(viewport, bounds.worldX, bounds.worldY,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::MoveCenter;
        }
        // Alvos em DUAS direções por eixo (±X, ±Y) — a
        // seta existe nos dois lados e o toque nela arrasta o EIXO.
        // HASTE também acerta — o alvo deixou de ser só a
        // pontinha (head dot); no device o toque na haste caía no
        // fallback do onScroll (mover RELATIVO com slop) e a entidade
        // "não seguia o dedo".
        const float axisLen = pxToWorld(viewport, axisPx(scale));
        const float shaftStartX = std::max(bounds.halfW, 0.f);
        const float shaftStartY = std::max(bounds.halfH, 0.f);
        const bool onXShaft =
            screenDistanceToSegment(viewport,
                                    bounds.worldX + shaftStartX, bounds.worldY,
                                    bounds.worldX + axisLen, bounds.worldY,
                                    screenX, screenY) <= hitScreenPx ||
            screenDistanceToSegment(viewport,
                                    bounds.worldX - shaftStartX, bounds.worldY,
                                    bounds.worldX - axisLen, bounds.worldY,
                                    screenX, screenY) <= hitScreenPx;
        const bool onXHead =
            screenDistanceTo(viewport, bounds.worldX + axisLen,
                             bounds.worldY, screenX, screenY) <= hitScreenPx ||
            screenDistanceTo(viewport, bounds.worldX - axisLen,
                             bounds.worldY, screenX, screenY) <= hitScreenPx;
        if (onXHead || onXShaft) {
            return GizmoHandle::MoveAxisX;
        }
        const bool onYShaft =
            screenDistanceToSegment(viewport,
                                    bounds.worldX, bounds.worldY + shaftStartY,
                                    bounds.worldX, bounds.worldY + axisLen,
                                    screenX, screenY) <= hitScreenPx ||
            screenDistanceToSegment(viewport,
                                    bounds.worldX, bounds.worldY - shaftStartY,
                                    bounds.worldX, bounds.worldY - axisLen,
                                    screenX, screenY) <= hitScreenPx;
        const bool onYHead =
            screenDistanceTo(viewport, bounds.worldX,
                             bounds.worldY + axisLen, screenX,
                             screenY) <= hitScreenPx ||
            screenDistanceTo(viewport, bounds.worldX,
                             bounds.worldY - axisLen, screenX,
                             screenY) <= hitScreenPx;
        if (onYHead || onYShaft) {
            return GizmoHandle::MoveAxisY;
        }
        return GizmoHandle::None;
    }

    if (tool == EditorTool::Rotate) {
        // Raio do anel com MÍNIMO de 64 px em tela — em zoom
        // baixo ou entidade pequena o anel continua agarrável.
        // O anel INTEIRO é alvo — banda |dist − raio| ≤ hit
        // — em vez de só o dot do handle. No device, tocar no anel a
        // 90° do dot devolvia None e o gesto virava PAN da câmera
        // ("rotação inoperante por toque"); o dot continua coberto
        // (está SOBRE o anel).
        const float radiusPx =
            ringRadiusPx(std::max(bounds.halfW, bounds.halfH) *
                             viewport.effectiveCamera().zoom,
                         scale);
        const float dx = viewport.worldToScreenX(bounds.worldX) - screenX;
        const float dy = viewport.worldToScreenY(bounds.worldY) - screenY;
        const float distPx = std::sqrt(dx * dx + dy * dy);
        if (std::abs(distPx - radiusPx) <= hitScreenPx) {
            return GizmoHandle::RotateRing;
        }
        return GizmoHandle::None;
    }

    if (tool == EditorTool::Scale) {
        // MESMAS posições CLAMPADAS do desenho — o toque
        // sempre coincide com o handle desenhado (anti-sobreposição).
        // Cantos primeiro (escala XY), depois arestas (um eixo) — P4.1.
        const HandlePoints points = scaleHandlePoints(viewport, bounds);
        if (screenDistanceTo(viewport, points.ne.first, points.ne.second,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::ScaleNE;
        }
        if (screenDistanceTo(viewport, points.nw.first, points.nw.second,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::ScaleNW;
        }
        if (screenDistanceTo(viewport, points.se.first, points.se.second,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::ScaleSE;
        }
        if (screenDistanceTo(viewport, points.sw.first, points.sw.second,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::ScaleSW;
        }
        if (screenDistanceTo(viewport, points.e.first, points.e.second,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::ScaleEdgeE;
        }
        if (screenDistanceTo(viewport, points.w.first, points.w.second,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::ScaleEdgeW;
        }
        if (screenDistanceTo(viewport, points.n.first, points.n.second,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::ScaleEdgeN;
        }
        if (screenDistanceTo(viewport, points.s.first, points.s.second,
                             screenX, screenY) <= hitScreenPx) {
            return GizmoHandle::ScaleEdgeS;
        }
        return GizmoHandle::None;
    }

    return GizmoHandle::None;  // Select: sem gizmo
}

// =============================================================================
// Drag
// =============================================================================

void TransformGizmo::beginDrag(GizmoHandle handle,
                               const GizmoTransform& startTransform,
                               const Viewport& viewport,
                               const GizmoBounds& bounds, float screenX,
                               float screenY)
{
    active_ = GizmoHandle::None;
    if (handle == GizmoHandle::None || !bounds.valid) {
        return;
    }
    active_ = handle;
    start_ = startTransform;

    const float worldX = viewport.screenToWorldX(screenX);
    const float worldY = viewport.screenToWorldY(screenY);
    grabWorldX_ = worldX;
    grabWorldY_ = worldY;

    if (handle == GizmoHandle::RotateRing) {
        startAngleRad_ = std::atan2(worldY - bounds.worldY,
                                    worldX - bounds.worldX);
        // Acumulador por EVENTO — o primeiro delta é zero.
        lastAngleRad_ = startAngleRad_;
        accumulatedRotationDeg_ = 0.f;
    } else if (isCornerHandle(handle) || isEdgeHandle(handle)) {
        // Pointer no frame LOCAL do nó (desfaz a rotação) — o ratio
        // local é o que multiplica a escala (Godot-style: cantos
        // escalam X e Y; arestas P4.1 escalam UM eixo).
        const float c = std::cos(-bounds.rotation);
        const float s = std::sin(-bounds.rotation);
        const float dx = worldX - bounds.worldX;
        const float dy = worldY - bounds.worldY;
        startPointerLocalX_ = dx * c - dy * s;
        startPointerLocalY_ = dx * s + dy * c;
    }
}

GizmoTransform TransformGizmo::dragTo(const Viewport& viewport,
                                      const GizmoBounds& bounds, float screenX,
                                      float screenY)
{
    if (active_ == GizmoHandle::None) {
        return start_;
    }
    GizmoTransform result = start_;
    const float worldX = viewport.screenToWorldX(screenX);
    const float worldY = viewport.screenToWorldY(screenY);
    const float dxWorld = worldX - grabWorldX_;
    const float dyWorld = worldY - grabWorldY_;

    switch (active_) {
    case GizmoHandle::MoveCenter:
        result.posX = start_.posX + dxWorld;
        result.posY = start_.posY + dyWorld;
        break;
    case GizmoHandle::MoveAxisX:
        result.posX = start_.posX + dxWorld;  // Y travado
        break;
    case GizmoHandle::MoveAxisY:
        result.posY = start_.posY + dyWorld;  // X travado
        break;
    case GizmoHandle::RotateRing: {
        // O código
        // antigo normalizava o ÂNGULO TOTAL contra o ponto de agarre
        // fixo; dedo além de 180° flipava o sinal (ex.: +200° virava
        // −160°) e a entidade girava PARA TRÁS. Agora cada evento
        // contribui com o delta CURTO contra o ângulo do evento
        // ANTERIOR (sempre <180° — impossível flipar) e a soma
        // acumula: volta(s) completas somam, drag contínuo funciona.
        const float angle = std::atan2(worldY - bounds.worldY,
                                       worldX - bounds.worldX);
        float deltaDeg = (angle - lastAngleRad_) * 180.f / kPi;
        while (deltaDeg > 180.f) { deltaDeg -= 360.f; }
        while (deltaDeg < -180.f) { deltaDeg += 360.f; }
        lastAngleRad_ = angle;
        accumulatedRotationDeg_ += deltaDeg;
        result.rotationDeg =
            snappedDegrees(start_.rotationDeg + accumulatedRotationDeg_);
        break;
    }
    case GizmoHandle::ScaleNE:
    case GizmoHandle::ScaleNW:
    case GizmoHandle::ScaleSE:
    case GizmoHandle::ScaleSW: {
        const float c = std::cos(-bounds.rotation);
        const float s = std::sin(-bounds.rotation);
        const float dx = worldX - bounds.worldX;
        const float dy = worldY - bounds.worldY;
        const float localX = dx * c - dy * s;
        const float localY = dx * s + dy * c;
        if (std::abs(startPointerLocalX_) > 1e-4f) {
            result.scaleX = clampScale(start_.scaleX * localX /
                                       startPointerLocalX_);
        }
        if (std::abs(startPointerLocalY_) > 1e-4f) {
            result.scaleY = clampScale(start_.scaleY * localY /
                                       startPointerLocalY_);
        }
        break;
    }
    case GizmoHandle::ScaleEdgeE:
    case GizmoHandle::ScaleEdgeW: {
        // Aresta E/W — escala SÓ no eixo X (o Y fica intacto,
        // distorção controlada pelo autor).
        const float c = std::cos(-bounds.rotation);
        const float s = std::sin(-bounds.rotation);
        const float dx = worldX - bounds.worldX;
        const float dy = worldY - bounds.worldY;
        const float localX = dx * c - dy * s;
        if (std::abs(startPointerLocalX_) > 1e-4f) {
            result.scaleX = clampScale(start_.scaleX * localX /
                                       startPointerLocalX_);
        }
        break;
    }
    case GizmoHandle::ScaleEdgeN:
    case GizmoHandle::ScaleEdgeS: {
        // Aresta N/S — escala SÓ no eixo Y.
        const float c = std::cos(-bounds.rotation);
        const float s = std::sin(-bounds.rotation);
        const float dx = worldX - bounds.worldX;
        const float dy = worldY - bounds.worldY;
        const float localY = dx * s + dy * c;
        if (std::abs(startPointerLocalY_) > 1e-4f) {
            result.scaleY = clampScale(start_.scaleY * localY /
                                       startPointerLocalY_);
        }
        break;
    }
    case GizmoHandle::None:
    default:
        break;
    }
    return result;
}

// =============================================================================
// Layout de desenho
// =============================================================================

std::vector<GizmoQuad> TransformGizmo::layoutQuads(const Viewport& viewport,
                                                   EditorTool tool,
                                                   const GizmoBounds& bounds) const
{
    std::vector<GizmoQuad> quads;
    if (!bounds.valid || tool == EditorTool::Select) {
        return quads;
    }
    const float scale = viewport.uiScale();
    const float handleHalf = pxToWorld(viewport, handlePx(scale)) * 0.5f;

    if (tool == EditorTool::Move) {
        // P4.1 (D1/D2) + P4.7.0 B2: 4 SETAS (±X, ±Y) — hastes terminam
        // na BASE do triângulo — e centro DIAMANTE (quadrado a 45° —
        // affordance distinta dos handles quadrados do scale).
        quads.push_back({bounds.worldX, bounds.worldY, handleHalf,
                         handleHalf, kPi * 0.25f, kCenterR, kCenterG,
                         kCenterB});
        return quads;
    }

    if (tool == EditorTool::Rotate) {
        // P4.1 (D3) + P4.7.0 B2: anel ≥64 px; o HANDLE é TRIÂNGULO
        // tangente (em layoutTriangles) — aqui só o anel/spoke vivem em
        // segmentos; NENHUM quad central (o pivot é o próprio centro).
        return quads;
    }

    // Scale: 4 CANTOS (quadrados — escala XY) + 4
    // marcas de aresta nas posições CLAMPADAS (anti-sobreposição).
    const HandlePoints points = scaleHandlePoints(viewport, bounds);
    quads.push_back({points.ne.first, points.ne.second, handleHalf,
                     handleHalf, bounds.rotation, kScaleR, kScaleG, kScaleB});
    quads.push_back({points.nw.first, points.nw.second, handleHalf,
                     handleHalf, bounds.rotation, kScaleR, kScaleG, kScaleB});
    quads.push_back({points.se.first, points.se.second, handleHalf,
                     handleHalf, bounds.rotation, kScaleR, kScaleG, kScaleB});
    quads.push_back({points.sw.first, points.sw.second, handleHalf,
                     handleHalf, bounds.rotation, kScaleR, kScaleG, kScaleB});
    const float edgeHalf = pxToWorld(viewport, edgePx(scale)) * 0.5f;
    quads.push_back({points.e.first, points.e.second, edgeHalf, edgeHalf,
                     bounds.rotation, kScaleR, kScaleG, kScaleB});
    quads.push_back({points.w.first, points.w.second, edgeHalf, edgeHalf,
                     bounds.rotation, kScaleR, kScaleG, kScaleB});
    quads.push_back({points.n.first, points.n.second, edgeHalf, edgeHalf,
                     bounds.rotation, kScaleR, kScaleG, kScaleB});
    quads.push_back({points.s.first, points.s.second, edgeHalf, edgeHalf,
                     bounds.rotation, kScaleR, kScaleG, kScaleB});
    return quads;
}

TransformGizmo::HandlePoints TransformGizmo::scaleHandlePoints(
    const Viewport& viewport, const GizmoBounds& bounds) const
{
    // CLAMP anti-sobreposição. Um bounds pequeno (px)
    // colapsaria cantos/arestas num só blob sobre o centro — cada handle
    // é empurrado PARA FORA ao longo da sua direção local até a
    // distância mínima do centro (em PX de tela, constante no zoom).
    HandlePoints points;
    const CornerPoints corners = cornersOf(bounds);
    const EdgePoints edges = edgesOf(bounds);
    const float scale = viewport.uiScale();
    const float minCornerPx = kMinCornerCenterDp * scale;
    const float minEdgePx = kMinEdgeCenterDp * scale;

    auto pushOut = [&](std::pair<float, float> point,
                       float minDistPx) {
        const float dx = viewport.worldToScreenX(point.first)
                         - viewport.worldToScreenX(bounds.worldX);
        const float dy = viewport.worldToScreenY(point.second)
                         - viewport.worldToScreenY(bounds.worldY);
        // worldToScreenY INVERTE o Y — distância euclidiana é igual.
        const float distPx = std::sqrt(dx * dx + dy * dy);
        if (distPx >= minDistPx || distPx <= 0.f) {
            return point;
        }
        const float factor = minDistPx / distPx;
        // Escala o OFFSET de MUNDO (proporcional ao offset de px —
        // worldToScreen é afin, zoom positivo).
        const float offX = (point.first - bounds.worldX) * factor;
        const float offY = (point.second - bounds.worldY) * factor;
        return std::make_pair(bounds.worldX + offX,
                              bounds.worldY + offY);
    };

    points.ne = pushOut(corners.ne, minCornerPx);
    points.nw = pushOut(corners.nw, minCornerPx);
    points.se = pushOut(corners.se, minCornerPx);
    points.sw = pushOut(corners.sw, minCornerPx);
    points.e = pushOut(edges.e, minEdgePx);
    points.w = pushOut(edges.w, minEdgePx);
    points.n = pushOut(edges.n, minEdgePx);
    points.s = pushOut(edges.s, minEdgePx);
    return points;
}

std::vector<GizmoTriangle> TransformGizmo::layoutTriangles(
    const Viewport& viewport, EditorTool tool,
    const GizmoBounds& bounds) const
{
    std::vector<GizmoTriangle> triangles;
    if (!bounds.valid || tool == EditorTool::Select) {
        return triangles;
    }
    const float scale = viewport.uiScale();
    const float halfLen =
        pxToWorld(viewport, headTriLenPx(scale) * 0.5f);
    const float halfBase =
        pxToWorld(viewport, headTriBasePx(scale) * 0.5f);

    if (tool == EditorTool::Move) {
        // P4.7.0 B2: 4 pontas de seta TRIANGULARES apontando PARA FORA
        // (+X, −X, +Y, −Y — convenção world, Y para cima).
        constexpr float kRight = 0.f;
        constexpr float kUp = kPi * 0.5f;
        constexpr float kLeft = kPi;
        constexpr float kDown = kPi * 1.5f;
        const float axisLen = pxToWorld(viewport, axisPx(scale));
        triangles.push_back({bounds.worldX + axisLen, bounds.worldY,
                             halfLen, halfBase, kRight, kXAxisR, kXAxisG,
                             kXAxisB});
        triangles.push_back({bounds.worldX - axisLen, bounds.worldY,
                             halfLen, halfBase, kLeft, kXAxisR, kXAxisG,
                             kXAxisB});
        triangles.push_back({bounds.worldX, bounds.worldY + axisLen,
                             halfLen, halfBase, kUp, kYAxisR, kYAxisG,
                             kYAxisB});
        triangles.push_back({bounds.worldX, bounds.worldY - axisLen,
                             halfLen, halfBase, kDown, kYAxisR, kYAxisG,
                             kYAxisB});
        return triangles;
    }

    if (tool == EditorTool::Rotate) {
        // Handle TRIANGULAR no anel apontando na TANGENTE (direção de
        // crescimento do ângulo — affordance de "para onde gira").
        const float radiusPx =
            std::max(bounds.halfW, bounds.halfH) *
                viewport.effectiveCamera().zoom;
        const float radius =
            pxToWorld(viewport, ringRadiusPx(radiusPx, scale));
        const auto [hx, hy] = ringHandleWorld(bounds, radius);
        triangles.push_back({hx, hy, halfLen, halfBase,
                             bounds.rotation + kPi * 0.5f, kRotateR,
                             kRotateG, kRotateB});
        return triangles;
    }

    // Scale: setas de ARESTA apontando PARA FORA ao longo do eixo local
    // (E/W no +X local, N/S no +Y local) — nas posições CLAMPADAS.
    const HandlePoints points = scaleHandlePoints(viewport, bounds);
    const float dirX = std::cos(bounds.rotation);
    const float dirY = std::sin(bounds.rotation);
    const float perpX = -dirY;
    const float perpY = dirX;
    const float outward = edgePx(scale) * 0.5f
                          + headTriLenPx(scale) * 0.75f;
    const float outwardW =
        pxToWorld(viewport, outward);
    triangles.push_back(
        {points.e.first + dirX * outwardW,
         points.e.second + dirY * outwardW, halfLen, halfBase,
         bounds.rotation, kScaleR, kScaleG, kScaleB});
    triangles.push_back(
        {points.w.first - dirX * outwardW,
         points.w.second - dirY * outwardW, halfLen, halfBase,
         bounds.rotation + kPi, kScaleR, kScaleG, kScaleB});
    triangles.push_back(
        {points.n.first + perpX * outwardW,
         points.n.second + perpY * outwardW, halfLen, halfBase,
         bounds.rotation + kPi * 0.5f, kScaleR, kScaleG, kScaleB});
    triangles.push_back(
        {points.s.first - perpX * outwardW,
         points.s.second - perpY * outwardW, halfLen, halfBase,
         bounds.rotation + kPi * 1.5f, kScaleR, kScaleG, kScaleB});
    return triangles;
}

std::vector<GizmoSegment> TransformGizmo::layoutSegments(
    const Viewport& viewport, EditorTool tool,
    const GizmoBounds& bounds) const
{
    std::vector<GizmoSegment> segments;
    if (!bounds.valid || tool == EditorTool::Select) {
        return segments;
    }
    const float scale = viewport.uiScale();

    if (tool == EditorTool::Move) {
        // Hastes das 4 setas (partem da borda do bounds — não
        // cobrem a arte da entidade). P4.7.0 B2: terminam na BASE do
        // triângulo (a seta é o triângulo — nunca haste através dele).
        const float axisLen = pxToWorld(viewport, axisPx(scale));
        const float shaftGap =
            pxToWorld(viewport, headTriLenPx(scale) * 0.5f);
        const float startX = std::max(bounds.halfW, 0.f);
        const float startY = std::max(bounds.halfH, 0.f);
        segments.push_back({bounds.worldX + startX, bounds.worldY,
                            bounds.worldX + axisLen - shaftGap, bounds.worldY,
                            kXAxisR, kXAxisG, kXAxisB});
        segments.push_back({bounds.worldX - startX, bounds.worldY,
                            bounds.worldX - axisLen + shaftGap, bounds.worldY,
                            kXAxisR, kXAxisG, kXAxisB});
        segments.push_back({bounds.worldX, bounds.worldY + startY,
                            bounds.worldX, bounds.worldY + axisLen - shaftGap,
                            kYAxisR, kYAxisG, kYAxisB});
        segments.push_back({bounds.worldX, bounds.worldY - startY,
                            bounds.worldX, bounds.worldY - axisLen + shaftGap,
                            kYAxisR, kYAxisG, kYAxisB});
        return segments;
    }

    if (tool == EditorTool::Rotate) {
        // Anel 32 lados + SPOKE do centro ao handle. P4.7.0
        // B2: a ponta de seta é o HANDLE TRIANGULAR (layoutTriangles).
        // O autor VÊ de onde girar.
        const float radiusPx =
            std::max(bounds.halfW, bounds.halfH) *
                viewport.effectiveCamera().zoom;
        const float radius =
            pxToWorld(viewport, ringRadiusPx(radiusPx, scale));
        constexpr int kSides = 32;  // suave em zoom alto
        float prevX = bounds.worldX + radius * std::cos(bounds.rotation);
        float prevY = bounds.worldY + radius * std::sin(bounds.rotation);
        for (int i = 1; i <= kSides; ++i) {
            const float angle = bounds.rotation +
                                (2.f * kPi * static_cast<float>(i)) /
                                    static_cast<float>(kSides);
            const float nextX = bounds.worldX + radius * std::cos(angle);
            const float nextY = bounds.worldY + radius * std::sin(angle);
            segments.push_back({prevX, prevY, nextX, nextY, kRotateR,
                                kRotateG, kRotateB});
            prevX = nextX;
            prevY = nextY;
        }
        // Spoke radial (centro → handle).
        const auto [hx, hy] = ringHandleWorld(bounds, radius);
        segments.push_back({bounds.worldX, bounds.worldY, hx, hy, kRotateR,
                            kRotateG, kRotateB});
        // P4.7.0 B2: o CHEVRON saiu — o handle TRIÂNGULAR tangente
        // (layoutTriangles) é a própria ponta de seta da rotação.
        return segments;
    }

    // Scale: diagonais do centro aos CANTOS (guia) + arestas
    // do retângulo (o quad que o autor está escalando — os handles de
    // aresta ganham sentido visual).
    const CornerPoints corners = cornersOf(bounds);
    segments.push_back({bounds.worldX, bounds.worldY, corners.ne.first, corners.ne.second,
                        kScaleR, kScaleG, kScaleB});
    segments.push_back({bounds.worldX, bounds.worldY, corners.nw.first, corners.nw.second,
                        kScaleR, kScaleG, kScaleB});
    segments.push_back({bounds.worldX, bounds.worldY, corners.se.first, corners.se.second,
                        kScaleR, kScaleG, kScaleB});
    segments.push_back({bounds.worldX, bounds.worldY, corners.sw.first, corners.sw.second,
                        kScaleR, kScaleG, kScaleB});
    const EdgePoints edges = edgesOf(bounds);
    segments.push_back({corners.nw.first, corners.nw.second, edges.n.first,
                        edges.n.second, kScaleR, kScaleG, kScaleB});
    segments.push_back({edges.n.first, edges.n.second, corners.ne.first,
                        corners.ne.second, kScaleR, kScaleG, kScaleB});
    segments.push_back({corners.sw.first, corners.sw.second, edges.s.first,
                        edges.s.second, kScaleR, kScaleG, kScaleB});
    segments.push_back({edges.s.first, edges.s.second, corners.se.first,
                        corners.se.second, kScaleR, kScaleG, kScaleB});
    segments.push_back({corners.nw.first, corners.nw.second, edges.w.first,
                        edges.w.second, kScaleR, kScaleG, kScaleB});
    segments.push_back({edges.w.first, edges.w.second, corners.sw.first,
                        corners.sw.second, kScaleR, kScaleG, kScaleB});
    segments.push_back({corners.ne.first, corners.ne.second, edges.e.first,
                        edges.e.second, kScaleR, kScaleG, kScaleB});
    segments.push_back({edges.e.first, edges.e.second, corners.se.first,
                        corners.se.second, kScaleR, kScaleG, kScaleB});
    return segments;
}

}  // namespace eng::editor
