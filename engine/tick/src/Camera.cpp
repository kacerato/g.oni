#include "eng/tick/Camera.hpp"

/// CameraData/resolveActiveCamera/CameraTickSystem — implementação
/// (evolução P0-5; ADR-051).

#include "eng/log/Macros.hpp"
#include "eng/math/Mat4.hpp"
#include "eng/scene/Name.hpp"

#include <algorithm>
#include <cmath>

ENG_LOG_CATEGORY("tick");

namespace eng::tick {

ActiveCamera resolveActiveCamera(const eng::scene::Scene& scene)
{
    // cada<CameraData> itera o pool na ORDEM DE INSERÇÃO —
    // índice de criação = ordem estável e determinística. Dados COPIADOS
    // por valor (ActiveCamera não retém ponteiro de pool — ver Camera.hpp).
    //
    // PosX/posY são OFFSETS a
    // partir da posição-MUNDO da ENTIDADE (a câmera é acoplada ao
    // Transform — mover a entidade no editor move a vista no Play).
    // Entidades na origem (todas as cenas pré-P2) preservam o
    // comportamento absoluto antigo: offset 0 + entidade em (0,0).
    ActiveCamera resolved;
    scene.world().each<CameraData>(
        [&](eng::ecs::Entity e, const CameraData& camera) {
            if (!resolved.has && camera.active) {
                resolved.entity = e;
                resolved.data = camera;
                const eng::math::Mat4 world = scene.computeWorldMatrix(e);
                resolved.data.posX += world.at(3, 0);
                resolved.data.posY += world.at(3, 1);
                resolved.has = true;
            }
        });
    return resolved;
}

std::size_t activeCameraCount(const eng::scene::Scene& scene)
{
    std::size_t count = 0;
    scene.world().each<CameraData>(
        [&](eng::ecs::Entity /*e*/, const CameraData& camera) {
            if (camera.active) {
                ++count;
            }
        });
    return count;
}

void CameraTickSystem::tick(eng::scene::Scene& scene, float dt)
{
    active_ = resolveActiveCamera(scene);
    const std::size_t actives = activeCameraCount(scene);
    if (actives > 1) {
        ENG_WARN(
            "CameraTick: {} câmeras ativas na cena — a primeira em ordem "
            "de criação vence (desative as demais)",
            actives);
    }
    if (!active_.has) {
        hasSmoothed_ = false;
        return;
    }

    // --- P4.7.0 Bloco 4: DEADZONE → SMOOTHING → CLAMP pós-zoom ----------
    const CameraData& data = active_.data;

    // Desejado = posição base resolvida (offsets + entidade).
    float desiredX = active_.data.posX;
    float desiredY = active_.data.posY;

    // FOLLOW (por NOME — primeira entidade com Name igual): substitui o
    // desejado pelo centro do alvo.
    eng::ecs::Entity followTarget{0xFFFFFFFFu, 0xFFFFFFFFu};
    if (!data.followName.empty()) {
        scene.world().each<eng::scene::Name>(
            [&](eng::ecs::Entity e, const eng::scene::Name& name) {
                if (followTarget.index == 0xFFFFFFFFu
                    && name.value == data.followName) {
                    const eng::math::Mat4 world = scene.computeWorldMatrix(e);
                    desiredX = world.at(3, 0);
                    desiredY = world.at(3, 1);
                    followTarget = e;
                }
            });
    }

    // Suavização: re-inicia quando a câmera ativa muda (ou 1º tick).
    if (!hasSmoothed_ || smoothedEntity_.index != active_.entity.index
        || smoothedEntity_.generation != active_.entity.generation) {
        hasSmoothed_ = true;
        smoothedX_ = desiredX;
        smoothedY_ = desiredY;
        smoothedEntity_ = active_.entity;
    }

    // DEADZONE (só com follow): o alvo pode andar dentro do retângulo
    // centrado na câmera sem movê-la; fora, a câmera anda o EXCESSO.
    if (followTarget.index != 0xFFFFFFFFu
        && (data.deadzoneW > 0.f || data.deadzoneH > 0.f)) {
        const float deltaX = desiredX - smoothedX_;
        const float deltaY = desiredY - smoothedY_;
        const float halfW = data.deadzoneW * 0.5f;
        const float halfH = data.deadzoneH * 0.5f;
        const float excessX =
            std::abs(deltaX) <= halfW ? 0.f : deltaX - std::copysign(halfW, deltaX);
        const float excessY =
            std::abs(deltaY) <= halfH ? 0.f : deltaY - std::copysign(halfH, deltaY);
        desiredX = smoothedX_ + excessX;
        desiredY = smoothedY_ + excessY;
    }

    // SMOOTHING exponencial (constante de tempo em segundos; 0 = snap).
    if (data.smoothingTime > 0.f && dt > 0.f) {
        const float alpha = 1.f - std::exp(-dt / data.smoothingTime);
        smoothedX_ += (desiredX - smoothedX_) * alpha;
        smoothedY_ += (desiredY - smoothedY_) * alpha;
    } else {
        smoothedX_ = desiredX;
        smoothedY_ = desiredY;
    }

    // CLAMP pós-zoom: o retângulo VISÍVEL (vista/zoom) fica dentro dos
    // limites. Sem hint de vista (viewW/viewH = 0) é no-op — documentado.
    if (data.limitsEnabled && viewW_ > 0.f && viewH_ > 0.f
        && data.zoom > 0.f) {
        const float halfVisibleW = viewW_ / (2.f * data.zoom);
        const float halfVisibleH = viewH_ / (2.f * data.zoom);
        const float minX = data.limitMinX + halfVisibleW;
        const float maxX = data.limitMaxX - halfVisibleW;
        const float minY = data.limitMinY + halfVisibleH;
        const float maxY = data.limitMaxY - halfVisibleH;
        if (minX <= maxX) {
            smoothedX_ = std::clamp(smoothedX_, minX, maxX);
        } else {
            smoothedX_ = (minX + maxX) * 0.5f; // limites estreitos: centro
        }
        if (minY <= maxY) {
            smoothedY_ = std::clamp(smoothedY_, minY, maxY);
        } else {
            smoothedY_ = (minY + maxY) * 0.5f;
        }
    }

    // O cache recebe a posição FINAL (o editor consome isto).
    active_.data.posX = smoothedX_;
    active_.data.posY = smoothedY_;
}

}  // namespace eng::tick
