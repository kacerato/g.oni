#include "eng/editor/Viewport.hpp"
#include "eng/editor/TextData.hpp"
#include "eng/ui/Ui.hpp"

#include "eng/render/Light2D.hpp"

/// Viewport — câmera 2D, conversões, quads e hit-test.
///
/// Ver header para o modelo de coordenadas e decisões (auditoria D3).

#include <algorithm>
#include <cmath>

#include "eng/editor/SpriteData.hpp"
#include "eng/particles/Particles.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/scene/Name.hpp"
#include "eng/tick/Camera.hpp"

namespace eng::editor {

namespace {

/// Hue determinístico por entidade (index) — visualmente distinguível sem
/// semântica falsa (missão §8.6: marcador, não classificador).
[[nodiscard]] std::uint32_t hueOf(eng::ecs::Entity entity) noexcept
{
    return (entity.index * 47u + 13u) % 360u;
}

}  // namespace

// =============================================================================
// Conversões tela ↔ mundo (P0-5: passam pela câmera EM FOCO — a de jogo
// quando ativa em Play, senão a do editor — ADR-051)
// =============================================================================

float Viewport::worldToScreenX(float wx) const noexcept
{
    const Camera2D& camera = effectiveCamera();
    return screenW_ * 0.5f + (wx - camera.posX) * camera.zoom;
}

float Viewport::worldToScreenY(float wy) const noexcept
{
    const Camera2D& camera = effectiveCamera();
    return screenH_ * 0.5f - (wy - camera.posY) * camera.zoom;
}

float Viewport::screenToWorldX(float sx) const noexcept
{
    const Camera2D& camera = effectiveCamera();
    return (sx - screenW_ * 0.5f) / camera.zoom + camera.posX;
}

float Viewport::screenToWorldY(float sy) const noexcept
{
    const Camera2D& camera = effectiveCamera();
    return camera.posY - (sy - screenH_ * 0.5f) / camera.zoom;
}

std::pair<float, float> Viewport::worldToScreen(float wx,
                                                float wy) const noexcept
{
    // Rotação da VISTA: com rotation == 0, caminho reto (idêntico ao
    // pré-P4.7). Contrato testado: a 90°, +X do mundo aparece para CIMA.
    const Camera2D& camera = effectiveCamera();
    if (camera.rotation == 0.f) {
        return {worldToScreenX(wx), worldToScreenY(wy)};
    }
    const float cx0 = worldToScreenX(wx);
    const float cy0 = worldToScreenY(wy);
    const float cosR = std::cos(camera.rotation);
    const float sinR = std::sin(camera.rotation);
    const float ox = cx0 - screenW_ * 0.5f;
    const float oy = cy0 - screenH_ * 0.5f;
    return {screenW_ * 0.5f + ox * cosR + oy * sinR,
            screenH_ * 0.5f - ox * sinR + oy * cosR};
}

std::pair<float, float> Viewport::screenToWorld(float sx,
                                                float sy) const noexcept
{
    const Camera2D& camera = effectiveCamera();
    if (camera.rotation == 0.f) {
        return {screenToWorldX(sx), screenToWorldY(sy)};
    }
    // Inversa: des-faz a rotação do offset de tela e volta ao mundo.
    const float dx = sx - screenW_ * 0.5f;
    const float dy = sy - screenH_ * 0.5f;
    const float cosR = std::cos(camera.rotation);
    const float sinR = std::sin(camera.rotation);
    const float ox = dx * cosR - dy * sinR;
    const float oy = dx * sinR + dy * cosR;
    const float cx0 = screenW_ * 0.5f + ox;
    const float cy0 = screenH_ * 0.5f + oy;
    return {screenToWorldX(cx0), screenToWorldY(cy0)};
}

void Viewport::pan(float screenDx, float screenDy) noexcept
{
    if (gameCameraActive()) {
        return; // ADR-051: a câmera em foco é do JOGO — pan é no-op
    }
    camera_.posX -= screenDx / camera_.zoom;
    camera_.posY += screenDy / camera_.zoom;
}

void Viewport::zoomAt(float factor, float screenFocusX, float screenFocusY) noexcept
{
    if (gameCameraActive()) {
        return; // ADR-051: zoom é no-op sob câmera de jogo
    }
    if (factor <= 0.f || !std::isfinite(factor)) {
        return;
    }
    // Mundo sob o foco ANTES do zoom — ele permanece fixo na tela.
    const float focusWX = screenToWorldX(screenFocusX);
    const float focusWY = screenToWorldY(screenFocusY);

    camera_.zoom = std::clamp(camera_.zoom * factor, kMinZoom, kMaxZoom);

    camera_.posX = focusWX - (screenFocusX - screenW_ * 0.5f) / camera_.zoom;
    camera_.posY = focusWY + (screenFocusY - screenH_ * 0.5f) / camera_.zoom;
}

void Viewport::setScreenSize(float width, float height) noexcept
{
    screenW_ = width > 0.f && std::isfinite(width) ? width : 1.f;
    screenH_ = height > 0.f && std::isfinite(height) ? height : 1.f;
}

void Viewport::setUiScale(float scale) noexcept
{
    // Faixa honesta: densidades de device (mdpi 1.0 … xxhdpi 3.0+). Valor
    // lixo → mantém 1.0 (nunca degenera os alvos de toque).
    uiScale_ = std::isfinite(scale) && scale >= 0.5f && scale <= 4.f
                   ? scale
                   : 1.f;
}

// =============================================================================
// Quads e hit-test
// =============================================================================

std::vector<EntityQuad> Viewport::buildQuads(
    const eng::scene::Scene& scene,
    const std::optional<eng::ecs::Entity>& selection) const
{
    std::vector<EntityQuad> quads;
    buildQuadsInto(quads, scene, selection, nullptr, nullptr);
    return quads;
}

std::vector<EntityQuad> Viewport::buildQuads(
    const eng::scene::Scene& scene,
    const std::optional<eng::ecs::Entity>& selection,
    const CullRect& cull, std::uint32_t* culledOut) const
{
    std::vector<EntityQuad> quads;
    buildQuadsInto(quads, scene, selection, &cull, culledOut);
    return quads;
}

Viewport::CullRect Viewport::worldViewRect() const noexcept
{
    // AABB da VISTA (câmera de jogo quando ativa — P0-5): centro + tela/
    // zoom. Rotação expande o AABB pelos cantos (half' = half*|cos| +
    // half*|sin| cruzado — cobre TUDO que a vista rotacionada enxerga).
    const Camera2D& cam = effectiveCamera();
    const float zoom = cam.zoom > 0.f ? cam.zoom : 48.f;
    const float halfW = screenW_ / zoom * 0.5f;
    const float halfH = screenH_ / zoom * 0.5f;
    const float cosR = std::cos(cam.rotation);
    const float sinR = std::sin(cam.rotation);
    const float halfX = halfW * std::abs(cosR) + halfH * std::abs(sinR);
    const float halfY = halfW * std::abs(sinR) + halfH * std::abs(cosR);
    CullRect rect;
    rect.minX = cam.posX - halfX;
    rect.maxX = cam.posX + halfX;
    rect.minY = cam.posY - halfY;
    rect.maxY = cam.posY + halfY;
    rect.margin = 0.f;
    return rect;
}

void Viewport::buildQuadsInto(std::vector<EntityQuad>& out,
                              const eng::scene::Scene& scene,
                              const std::optional<eng::ecs::Entity>& selection,
                              const CullRect* cull,
                              std::uint32_t* culledOut) const
{
    if (culledOut != nullptr) {
        *culledOut = 0;
    }
    // Pooling (B6): clear() preserva a capacidade já conquistada.
    std::vector<EntityQuad>& quads = out;
    quads.clear();
    quads.reserve(scene.nodeCount());

    // Caminhada depth-first estável (mesma ordem do hierarchySnapshot — a
    // ORDEM DE DESENHO; hit-test varre de trás para frente).
    const auto visit = [&](auto&& self, eng::ecs::Entity node, int depth) -> void {
        // Camadas: entidades em camada sem participação
        // de render NÃO geram quad (filhos continuam sendo visitados — a
        // camada é por entidade, não herdada).
        // Moldes: invisíveis no Play; esmaecidos na edição.
        const bool ghost =
            editing_ && scene.isTemplated(node) &&
            scene.layerParticipates(node, eng::scene::LayerStage::Render);
        if (!ghost &&
            !scene.participatesIn(node, eng::scene::LayerStage::Render)) {
            scene.eachChild(node, [&](eng::ecs::Entity child) {
                self(self, child, depth + 1);
            });
            return;
        }
        const eng::math::Mat4 world = scene.computeWorldMatrix(node);

        EntityQuad quad;
        quad.entity = node;
        // Column-major: translação vive na 4ª COLUNA — at(3, row).
        quad.worldX = world.at(3, 0);
        quad.worldY = world.at(3, 1);
        // Escala = comprimento das colunas da base.
        quad.sizeX = std::sqrt(world.at(0, 0) * world.at(0, 0) +
                              world.at(0, 1) * world.at(0, 1) +
                              world.at(0, 2) * world.at(0, 2));
        quad.sizeY = std::sqrt(world.at(1, 0) * world.at(1, 0) +
                              world.at(1, 1) * world.at(1, 1) +
                              world.at(1, 2) * world.at(1, 2));
        quad.rotation = std::atan2(world.at(0, 1), world.at(0, 0));
        quad.tint = hueOf(node);
        quad.selected = selection.has_value() && *selection == node;

        // CULLING por retângulo de vista (Play) — quad
        // FORA do rect (AABB do quad girado + margem) NÃO entra; os
        // FILHOS continuam sendo visitados (filho em vista desenha — a
        // hierarquia nunca poda). Contagem exporta a métrica do round 7.
        if (cull != nullptr) {
            const float cosR = std::abs(std::cos(quad.rotation));
            const float sinR = std::abs(std::sin(quad.rotation));
            const float halfX =
                (cosR * quad.sizeX + sinR * quad.sizeY) * 0.5f;
            const float halfY =
                (sinR * quad.sizeX + cosR * quad.sizeY) * 0.5f;
            if (!cull->overlaps(halfX, halfY, quad.worldX, quad.worldY)) {
                if (culledOut != nullptr) {
                    ++*culledOut;
                }
                scene.eachChild(node, [&](eng::ecs::Entity child) {
                    self(self, child, depth + 1);
                });
                return;
            }
        }
        // Camada da entidade: agrupa o draw no conjunto de luzes
        // da camada (Light2D.layer). Sem LayerMember = GAME.
        if (const auto* member =
                scene.world().get<eng::scene::LayerMember>(node)) {
            quad.layer = member->layer;
        }

        // Sprite (evolução P0-3): SpriteData REAL substitui o marcador hue.
        // Tamanho do sprite em mundo = escala local × (região em PIXELS do
        // arquivo / pixelsPerUnit) — os pixels da textura vêm do cache do
        // HOST (aqui é camada de dados, sem renderer); sem metadados, a
        // região UV × 1 unidade de mundo serve de estimativa e o renderer
        // corrige na escala final.
        if (const auto* sprite = scene.world().get<eng::editor::SpriteData>(node)) {
            quad.isSprite = true;
            quad.textureAsset = sprite->textureAsset;
            quad.u0 = sprite->u0;
            quad.v0 = sprite->v0;
            quad.u1 = sprite->u1;
            quad.v1 = sprite->v1;
            quad.tintR = sprite->tintR;
            quad.tintG = sprite->tintG;
            quad.tintB = sprite->tintB;
            quad.tintA = sprite->opacity;
            quad.flipX = sprite->flipX;
            quad.flipY = sprite->flipY;
            quad.sort = sprite->sort;
            quad.spritePpu = sprite->pixelsPerUnit > 0.f ? sprite->pixelsPerUnit
                                                         : 1.f;
            quad.pivotX = sprite->pivotX;
            quad.pivotY = sprite->pivotY;
            quad.materialAsset = sprite->materialAsset;
        }

        // Collider (RECOVERY §10): geometria nas MESMAS convenções do
        // PhysicsWorld::worldShapeOf — centrado no nó, raio/halfExtents
        // escalados pela norma das colunas do world matrix (sizeX/sizeY
        // do quad JÁ são essas normas). O runtime usa exatamente isto;
        // agora o autor VÊ o mesmo shape que a física usará.
        if (const auto* collider =
                scene.world().get<eng::physics::Collider>(node)) {
            quad.hasCollider = true;
            quad.colliderIsSphere =
                collider->shape == eng::physics::ColliderShape::Sphere;
            quad.colliderTrigger = collider->isTrigger;
            if (quad.colliderIsSphere) {
                quad.colliderHalfX = collider->radius * quad.sizeX;
                quad.colliderHalfY = quad.colliderHalfX;
            } else {
                quad.colliderHalfX = collider->halfExtents.x * quad.sizeX;
                quad.colliderHalfY = collider->halfExtents.y * quad.sizeY;
            }
        }

        // Câmera de jogo (P2 §11): retângulo de VISTA da CameraData —
        // mesma fórmula do viewport (tela/zoom), centrada na entidade +
        // offset (a MESMA semântica de resolveActiveCamera no Play).
        if (const auto* camera =
                scene.world().get<eng::tick::CameraData>(node)) {
            quad.hasCamera = true;
            quad.cameraActive = camera->active;
            quad.cameraCenterX = quad.worldX + camera->posX;
            quad.cameraCenterY = quad.worldY + camera->posY;
            float zoom = camera->zoom > 0.f ? camera->zoom : 48.f;
            if (camera->viewHeight > 0.f && screenH_ > 1.f) {
                zoom = screenH_ / camera->viewHeight;
            }
            quad.cameraHalfW =
                static_cast<float>(screenW_) / zoom * 0.5f;
            quad.cameraHalfH =
                static_cast<float>(screenH_) / zoom * 0.5f;
            // P4.7.0 B4: rotação da vista + retângulo de limites.
            quad.cameraRotation =
                camera->rotationDeg * (3.14159265358979323846f / 180.f);
            quad.cameraLimits = camera->limitsEnabled;
            quad.cameraLimitMinX = camera->limitMinX;
            quad.cameraLimitMinY = camera->limitMinY;
            quad.cameraLimitMaxX = camera->limitMaxX;
            quad.cameraLimitMaxY = camera->limitMaxY;
        }

        // Emissor de partículas (P2 §10): marcador editável — quad +
        // seta na DIREÇÃO de emissão (direção do emitter girada pela
        // rotação do nó, escalada pela norma das colunas).
        if (const auto* emitter =
                scene.world().get<eng::particles::ParticleEmitter>(node)) {
            quad.hasEmitter = true;
            const float len = std::sqrt(emitter->direction.x *
                                            emitter->direction.x +
                                        emitter->direction.y *
                                            emitter->direction.y);
            if (len > 1e-6f) {
                const float dx = emitter->direction.x / len;
                const float dy = emitter->direction.y / len;
                const float cosR = std::cos(quad.rotation);
                const float sinR = std::sin(quad.rotation);
                quad.emitterDirX = dx * cosR - dy * sinR;
                quad.emitterDirY = dx * sinR + dy * cosR;
            }
            quad.emitterSize = std::max(0.35f, quad.sizeX * 0.5f);
        }
        // Luz 2D: dados para o bloco PerFrame (posicao = worldX/Y
        // do no - fonte unica de verdade). Desligada nao entra.
        if (const auto* light = scene.world().get<eng::render::Light2D>(node)) {
            if (light->enabled) {
                quad.hasLight = true;
                quad.lightIntensity = light->intensity;
                quad.lightRadius = light->radius;
                quad.lightFalloff = light->falloff;
                quad.lightColorR = light->colorR;
                quad.lightColorG = light->colorG;
                quad.lightColorB = light->colorB;
                quad.lightLayer = light->layer;
            }
        }

        if (ghost) {
            quad.tintA *= 0.35f;
        }
        quads.push_back(quad);
        (void)depth; // profundidade não muda o quad — reserva de API futura

        if (const auto* text = scene.world().get<eng::editor::TextData>(node)) {
            appendTextQuads(quads, node, *text, quad.worldX, quad.worldY,
                            ghost ? 0.35f : 1.f);
        }

        scene.eachChild(node, [&](eng::ecs::Entity child) {
            self(self, child, depth + 1);
        });
    };

    // Raízes: iteração sobre filhos "da cena" (pai = kNoEntity) — World não
    // expõe enumeração de pools; percorre via Hierarchy? Não: a Scene não
    // lista raízes diretamente — usa-se o world com cada nó criado. Solução:
    // percorrer TODOS os nós vivos e filtrar raízes (pai == kNoEntity) em
    // ordem estável por índice.
    // Texto preso à tela: na edição aparece dentro do quadro da câmera de
    // jogo (a mesma posição que terá no Play).
    textFrame_ = TextFrame{};
    if (editing_) {
        scene.world().each<eng::tick::CameraData>(
            [&](eng::ecs::Entity e, const eng::tick::CameraData& cam) {
                if (textFrame_.valid || !cam.active || scene.isTemplated(e)) {
                    return;
                }
                const eng::math::Mat4 world = scene.computeWorldMatrix(e);
                float zoom = cam.zoom > 0.f ? cam.zoom : 48.f;
                if (cam.viewHeight > 0.f && screenH_ > 1.f) {
                    zoom = screenH_ / cam.viewHeight;
                }
                textFrame_.valid = true;
                textFrame_.cx = world.at(3, 0) + cam.posX;
                textFrame_.cy = world.at(3, 1) + cam.posY;
                textFrame_.halfW = screenW_ / zoom * 0.5f;
                textFrame_.halfH = screenH_ / zoom * 0.5f;
            });
    }

    std::vector<eng::ecs::Entity> roots;
    scene.world().each<eng::scene::Hierarchy>(
        [&](eng::ecs::Entity node, const eng::scene::Hierarchy& hierarchy) {
            if (hierarchy.parent == eng::scene::kNoEntity && scene.isNode(node)) {
                roots.push_back(node);
            }
        });
    std::sort(roots.begin(), roots.end(),
              [](eng::ecs::Entity a, eng::ecs::Entity b) {
                  return a.index < b.index;
              });
    for (const eng::ecs::Entity root : roots) {
        visit(visit, root, 0);
    }
}

void Viewport::appendTextQuads(std::vector<EntityQuad>& quads,
                               eng::ecs::Entity node, const TextData& text,
                               float worldX, float worldY,
                               float alphaScale) const
{
    if (text.text.empty() || text.size <= 0.f) {
        return;
    }
    // Fonte 5×7: avanço de 6 colunas, linha de 9 (7 + espaço).
    float px = text.size / 7.f;
    float originX = worldX;
    float originY = worldY;
    if (text.screenSpace) {
        if (textFrame_.valid) {
            originX = textFrame_.cx - textFrame_.halfW +
                      text.screenX * 2.f * textFrame_.halfW;
            originY = textFrame_.cy + textFrame_.halfH -
                      text.screenY * 2.f * textFrame_.halfH;
        } else {
            originX = screenToWorldX(text.screenX * screenW_);
            originY = screenToWorldY(text.screenY * screenH_);
        }
    }
    // Quebra em linhas para alinhar cada uma.
    std::vector<std::string_view> lines;
    {
        std::string_view all{text.text};
        std::size_t start = 0;
        while (true) {
            const auto nl = all.find('\n', start);
            lines.push_back(all.substr(start, nl == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : nl - start));
            if (nl == std::string_view::npos) {
                break;
            }
            start = nl + 1;
        }
    }
    // Texto preso à tela nunca sai pelas bordas: em telas mais estreitas
    // (ou frases longas) a letra encolhe até caber em 92% da largura.
    if (text.screenSpace) {
        const float available =
            textFrame_.valid ? 2.f * textFrame_.halfW
                             : std::abs(screenToWorldX(screenW_) - screenToWorldX(0.f));
        std::size_t longest = 0;
        for (const std::string_view line : lines) {
            longest = std::max(longest, line.size());
        }
        const float widest = longest == 0 ? 0.f : (static_cast<float>(longest) * 6.f - 1.f) * px;
        if (available > 0.f && widest > available * 0.92f) {
            px *= available * 0.92f / widest;
        }
    }
    const float lineH = 9.f * px;
    const float blockH = static_cast<float>(lines.size()) * lineH - 2.f * px;
    float lineTop = originY + blockH * 0.5f;  // bloco centrado no ponto
    for (const std::string_view line : lines) {
        const float width =
            line.empty() ? 0.f : (static_cast<float>(line.size()) * 6.f - 1.f) * px;
        float left = originX - width * 0.5f;
        if (text.align == TextAlign::Left) {
            left = originX;
        } else if (text.align == TextAlign::Right) {
            left = originX - width;
        }
        for (std::size_t i = 0; i < line.size(); ++i) {
            char ch = line[i];
            if (ch >= 'a' && ch <= 'z') {
                // A fonte 5×7 só tem maiúsculas legíveis.
                ch = static_cast<char>(ch - 'a' + 'A');
            }
            const eng::ui::FontGlyph* glyph = eng::ui::fontGlyph(ch);
            if (glyph == nullptr) {
                continue;
            }
            const float glyphLeft = left + static_cast<float>(i) * 6.f * px;
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (!glyph->pixels[row][col]) {
                        continue;
                    }
                    EntityQuad q;
                    q.entity = node;
                    q.textPixel = true;
                    q.isSprite = true;
                    q.worldX = glyphLeft + (static_cast<float>(col) + 0.5f) * px;
                    q.worldY = lineTop - (static_cast<float>(row) + 0.5f) * px;
                    // Sobreposição mínima evita frestas entre pixels.
                    q.sizeX = px * 1.02f;
                    q.sizeY = px * 1.02f;
                    q.tintR = text.colorR;
                    q.tintG = text.colorG;
                    q.tintB = text.colorB;
                    q.tintA = text.opacity * alphaScale;
                    q.sort = text.sort;
                    q.spritePpu = 1.f;
                    q.materialShader = "unlit";
                    quads.push_back(std::move(q));
                }
            }
        }
        lineTop -= lineH;
    }
}

std::vector<ParticleQuad> Viewport::buildParticleQuads(
    const eng::scene::Scene& scene) const
{
    // Auditoria final (drift D6 da FASE 10): o viewport prometia desenhar
    // partículas como quads — nada lia a ParticlePool. Uma por partícula
    // VIVA (pool é runtime-only; em Play o clone tem as pools ativas).
    // Camadas: pool de camada sem render NÃO desenha.
    std::vector<ParticleQuad> quads;
    scene.world().each<eng::particles::ParticlePool>(
        [&](eng::ecs::Entity emitter,
            const eng::particles::ParticlePool& pool) {
            if (!scene.participatesIn(emitter,
                                      eng::scene::LayerStage::Render)) {
                return;
            }
            quads.reserve(quads.size() + pool.particles.size());
            for (const auto& particle : pool.particles) {
                ParticleQuad quad;
                quad.worldX = particle.position.x;
                quad.worldY = particle.position.y;
                quad.size = particle.size;
                quad.rotation = particle.rotation;
                quads.push_back(quad);
            }
        });
    return quads;
}

std::optional<eng::ecs::Entity> Viewport::hitTest(
    const std::vector<EntityQuad>& quads, float screenX, float screenY,
    float touchRadius) const noexcept
{
    // Top-most = último desenhado (frente). Raio generoso p/ dedo.
    // Usa a câmera EM FOCO (de jogo quando ativa) — o toque segue
    // a câmera que o usuário está vendo.
    const Camera2D& camera = effectiveCamera();
    for (auto it = quads.rbegin(); it != quads.rend(); ++it) {
        // Tamanho tocável = tamanho DESENHADO (RECOVERY P0): sprite
        // texturizado usa escala × (região em px / ppu) — a MESMA fórmula
        // do renderer; o hit box tem de casar com o que o autor VÊ, senão
        // ele toca na imagem e "não seleciona nada". Sem dimensões
        // resolvidas, cai no caminho da escala local (quad de cor).
        float worldHalfW = it->sizeX * 0.5f;
        float worldHalfH = it->sizeY * 0.5f;
        if (!it->textureAsset.empty() && it->textureWidthPx > 0u &&
            it->textureHeightPx > 0u) {
            const float regionPx =
                static_cast<float>(it->textureWidthPx) * (it->u1 - it->u0);
            const float regionPy =
                static_cast<float>(it->textureHeightPx) * (it->v1 - it->v0);
            const float ppu = it->spritePpu > 0.f ? it->spritePpu : 1.f;
            worldHalfW = it->sizeX * regionPx / ppu * 0.5f;
            worldHalfH = it->sizeY * regionPy / ppu * 0.5f;
        }
        const float centerSX = worldToScreenX(it->worldX);
        const float centerSY = worldToScreenY(it->worldY);
        const float halfPX =
            std::max(worldHalfW * camera.zoom, kMinQuadPixels * 0.5f);
        const float halfPY =
            std::max(worldHalfH * camera.zoom, kMinQuadPixels * 0.5f);
        const float dx = std::abs(screenX - centerSX);
        const float dy = std::abs(screenY - centerSY);
        if (dx <= halfPX + touchRadius && dy <= halfPY + touchRadius) {
            return it->entity;
        }
    }
    return std::nullopt;
}

std::size_t Viewport::quadCount(const std::vector<EntityQuad>& quads) const noexcept
{
    return quads.size();
}

}  // namespace eng::editor
