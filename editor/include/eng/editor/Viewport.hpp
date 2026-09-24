#pragma once

/// eng::editor::Viewport — câmera 2D, hit-test e lista de quads do editor
///.
///
/// Modelo de coordenadas:
///   - MUNDO: unidades arbitrárias, Y para cima, X para direita;
///   - TELA: pixels, origem no canto superior esquerdo, Y para baixo
///     (convenção Android — missão §8.8);
///   - `screen = center + (world - cameraPos) * zoom`, com flip de Y.
///
/// Quads: cada nó vira um quad centralizado na posição-mundo derivada do
/// `computeWorldMatrix` do nó (hierarquia composta), tamanho = escala local
/// (clamp mínimo), rotação = ângulo no plano XY. CORRETO > COMPLEXO:
/// é um marcador visual de entidade, não um renderer de jogo.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "eng/ecs/Ecs.hpp"
#include "eng/math/Mat4.hpp"
#include "eng/scene/Scene.hpp"

namespace eng::editor {

/// Um quad desenhável no viewport (dados, não comandos de GPU).
struct EntityQuad {
    eng::ecs::Entity entity{};
    float worldX{0.f};
    float worldY{0.f};
    float sizeX{1.f};              ///< em unidades de mundo (da escala)
    float sizeY{1.f};
    float rotation{0.f};           ///< radianos no plano XY
    std::uint32_t tint{0u};        ///< hue determinístico por entidade
    bool selected{false};

    // --- sprite (evolução P0-3) — preenchido quando o nó tem SpriteData ---
    /// Nó TEM SpriteData (com ou sem textura). Sem textura → o renderer
    /// desenha o PLACEHOLDER xadrez (P1.10: claramente identificado,
    /// não confundir com sprite renderizado/hue de entidade crua).
    bool isSprite{false};
    /// Nome do asset de textura (vazio = quad de cor, caminho antigo).
    std::string textureAsset{};
    /// Região UV do sprite (respeita flip no renderer).
    float u0{0.f};
    float v0{0.f};
    float u1{1.f};
    float v1{1.f};
    /// Tint multiplicativo RGBA (1,1,1,1 = sem tint).
    float tintR{1.f};
    float tintG{1.f};
    float tintB{1.f};
    float tintA{1.f};
    bool flipX{false};
    bool flipY{false};
    /// Ordem de desenho (maior = frente — o renderer ordena sprites por isto).
    float sort{0.f};
    /// Pixels por unidade de mundo do sprite (do SpriteData).
    float spritePpu{1.f};
    /// Pivot do sprite [0..1] (0.5,0.5 = centrado).
    float pivotX{0.5f};
    float pivotY{0.5f};
    /// Dimensões em PIXELS da textura resolvida (0 = desconhecida — o
    /// hit-test usa o tamanho de sprite SÓ quando ambas > 0; o documento
    /// preenche via TextureCache::imageInfo antes do tap, o renderer via
    /// a textura GPU que subiu). Tamanho mundial do sprite =
    /// escala × (região em px / ppu) — o hit box tem de casar com o
    /// desenhado, senão o autor toca na imagem e "não seleciona nada".
    std::uint32_t textureWidthPx{0u};
    std::uint32_t textureHeightPx{0u};
    /// Material do sprite (P3 §3): nome do asset materials/<n>.mat.json;
    /// VAZIO = material default (shader "lit", tint neutro). O renderer
    /// resolve (shader/tint multiplicativo) via cache do host.
    std::string materialAsset{};
    /// Shader do material RESOLVIDO pelo documento (P3 §3 — EditorDocument::
    /// resolveMaterials): "lit" (default) ou "unlit". O tint do material
    /// já está multiplicado em tintR/G/B/A.
    std::string materialShader{"lit"};
    /// Camada da ENTIDADE (LayerMember; "GAME" default — P3 §5): agrupa o
    /// draw no conjunto de luzes da MESMA camada (mask real da Light2D).
    std::string layer{"GAME"};

    // --- collider (RECOVERY §10) — preenchido quando o nó tem Collider ---
    /// O AUTOR precisa VER o shape de colisão que está editando: o quad
    /// carrega a geometria (nas MESMAS convenções do PhysicsWorld —
    /// centrado no nó, escalado pelas colunas do world matrix) e o
    /// renderer desenha o contorno por cima da cena.
    bool hasCollider{false};
    /// Meia-largura/altura em MUNDO (esfera: halfX == halfY == raio).
    float colliderHalfX{0.5f};
    float colliderHalfY{0.5f};
    /// Esfera → contorno octogonal; box → retângulo na rotação do nó.
    bool colliderIsSphere{false};
    /// Trigger → contorno âmbar (contato SEM resolução — §7.2 da física).
    bool colliderTrigger{false};

    // --- câmera de jogo (P2 §11 — "visualizar área da câmera") -------------
    /// O nó tem CameraData: o renderer desenha o RETÂNGULO DE VISTA
    /// (o que a câmera veria no Play com a tela ATUAL do editor —
    /// mesma fórmula do viewport: tela/zoom).
    bool hasCamera{false};
    bool cameraActive{true};
    float cameraCenterX{0.f};  ///< centro da vista em MUNDO (entidade+offset)
    float cameraCenterY{0.f};
    float cameraHalfW{1.f};    ///< meia-largura da vista em MUNDO
    float cameraHalfH{1.f};
    // P4.7.0 B4: rotação da vista + limites do mundo (moldura extra).
    float cameraRotation{0.f}; ///< radianos (mesmo campo do Camera2D)
    bool cameraLimits{false};
    float cameraLimitMinX{0.f};
    float cameraLimitMinY{0.f};
    float cameraLimitMaxX{0.f};
    float cameraLimitMaxY{0.f};

    // --- emissor de partículas (P2 §10 — representação editável) ----------
    /// O nó tem ParticleEmitter: marcador no viewport (quad + seta de
    /// direção) — o autor VÊ onde/em-que-direção o emissor dispara.
    bool hasEmitter{false};
    float emitterDirX{0.f};    ///< direção NORMALIZADA em mundo
    float emitterDirY{1.f};
    float emitterSize{0.5f};   ///< meia-aresta do marcador (mundo)

    // --- luz 2D (P3 §5) — preenchido quando o nó tem Light2D ativa --------
    /// A luz contribui para o bloco PerFrame dos sprites da MESMA camada
    /// (Light2D.layer). enabled=false NÃO entra (custo zero).
    bool hasLight{false};
    float lightIntensity{1.f};
    float lightRadius{4.f};      ///< unidades de mundo
    float lightFalloff{1.5f};    ///< expoente da atenuação
    float lightColorR{1.f};
    float lightColorG{0.93f};
    float lightColorB{0.78f};
    /// Camada que a luz ilumina (Light2D.layer — "GAME" ilumina todos sem
    /// LayerMember próprio).
    std::string lightLayer{"GAME"};
};

/// Quad de PARTÍCULA viva (marcador de gameplay — FASE 10). Auditoria
/// final: docs prometiam "o viewport desenha partículas como quads
/// (mesmo pipeline pos+cor)" e nada lia a ParticlePool — agora é real.
struct ParticleQuad {
    float worldX{0.f};
    float worldY{0.f};
    float size{0.08f};             ///< tamanho da partícula (mundo)
    float rotation{0.f};          ///< radianos no plano XY
};

class Viewport final {
public:
    /// Câmera 2D do editor (pan + zoom — §8.6).
    struct Camera2D {
        float posX{0.f};
        float posY{0.f};
        float zoom{48.f};          ///< pixels por unidade de mundo
        /// P4.7.0 B4: rotação da VISTA em radianos CCW (0 = sem rotação;
        /// a câmera do EDITOR nunca rota — só a de jogo, via CameraData).
        float rotation{0.f};
    };

    /// Retângulo de CULLING em MUNDO (AABB — a rotação da
    /// vista já vem expandida pelos cantos). `margin` cobre sprites que
    /// renderizam MAIORES que a escala da entidade (região px/ppu não é
    /// conhecida na camada de dados): quem toca o rect expandido DESENHA
    /// (culling conservador — nunca some sprite visível por margem curta).
    struct CullRect {
        float minX{0.f};
        float minY{0.f};
        float maxX{0.f};
        float maxY{0.f};
        float margin{0.f};

        [[nodiscard]] bool overlaps(float halfX, float halfY, float x,
                                    float y) const noexcept
        {
            return x + halfX + margin >= minX && x - halfX - margin <= maxX
                   && y + halfY + margin >= minY
                   && y - halfY - margin <= maxY;
        }
    };

    /// AABB da VISTA em MUNDO (câmera de jogo quando ativa, senão a do
    /// editor) — rotação expandida pelos cantos (half' = half*|cos| +
    /// half*|sin| cruzado). Margem NÃO incluída (o chamador soma).
    [[nodiscard]] CullRect worldViewRect() const noexcept;

    // --- conversões --------------------------------------------------------

    [[nodiscard]] float worldToScreenX(float wx) const noexcept;
    [[nodiscard]] float worldToScreenY(float wy) const noexcept;
    [[nodiscard]] float screenToWorldX(float sx) const noexcept;
    [[nodiscard]] float screenToWorldY(float sy) const noexcept;

    /// P4.7.0 B4: conversão PAR — a rotação da VISTA precisa do ponto
    /// completo (as funções single-eixo acima assumem rotation == 0 e
    /// preservam o caminho reto pré-P4.7; com câmera rotacionada, use
    /// SEMPRE o par). Contrato: a 90°, o +X do mundo aparece PARA CIMA.
    [[nodiscard]] std::pair<float, float> worldToScreen(float wx,
                                                        float wy) const noexcept;
    [[nodiscard]] std::pair<float, float> screenToWorld(float sx,
                                                        float sy) const noexcept;

    // --- navegação (gestos — §8.6/§8.8) -------------------------------------

    /// Pan por delta de TELA (pixels). NO-OP quando a câmera de JOGO
    /// está ativa (P0-5: em Play com câmera na cena, mexer na câmera do
    /// editor por trás seria debug mentiroso — a câmera é do jogo).
    void pan(float screenDx, float screenDy) noexcept;

    /// Zoom centrado num foco de TELA (pinch). Fator > 1 = aproximar.
    /// Mesma política de `pan` sob câmera de jogo.
    void zoomAt(float factor, float screenFocusX, float screenFocusY) noexcept;

    // --- geometria do viewport ----------------------------------------------

    void setScreenSize(float width, float height) noexcept;
    [[nodiscard]] float screenWidth() const noexcept { return screenW_; }
    [[nodiscard]] float screenHeight() const noexcept { return screenH_; }
    [[nodiscard]] const Camera2D& camera() const noexcept { return camera_; }
    [[nodiscard]] Camera2D& camera() noexcept { return camera_; }

    /// Densidade do device (dp → px da surface). 1.0 no
    /// Linux/testes; a Activity instala `resources.displayMetrics.density`.
    /// Alvos de toque e handles do gizmo ESCALAM por isto (48 dp = 48×s px
    /// de alvo) — os 13 px do P1 eram intocáveis no dedo (defeito D3/D4).
    void setUiScale(float scale) noexcept;
    [[nodiscard]] float uiScale() const noexcept { return uiScale_; }

    // --- câmera de jogo (evolução P0-5, ADR-051) ------------------------------

    /// Define a câmera de JOGO usada nas conversões (nullptr = desliga).
    /// TODAS as conversões world↔screen E o hit-test passam a usá-la — o
    /// render, o toque e o arraste seguem a câmera do jogo de graça.
    /// O DONO do objeto apontado é o chamador (o documento guarda o
    /// cache do frame). Pan/zoom do editor ficam no-op enquanto ativa.
    void setGameCamera(const Camera2D* camera) noexcept { gameCamera_ = camera; }
    [[nodiscard]] bool gameCameraActive() const noexcept
    {
        return gameCamera_ != nullptr;
    }
    /// Câmera em foco: a de jogo quando ativa, senão a do editor.
    [[nodiscard]] const Camera2D& effectiveCamera() const noexcept
    {
        return gameCamera_ != nullptr ? *gameCamera_ : camera_;
    }

    /// Zoom clampado a limites utilizáveis (evita degenerar com pinch).
    static constexpr float kMinZoom = 8.f;
    static constexpr float kMaxZoom = 512.f;

    /// P4.7.0 B6: margem do culling (render e logic LOD) em unidades de
    /// MUNDO — sprites que renderizam maiores que a escala da entidade
    /// (região px/ppu não é conhecida na camada de dados) somem nas
    /// bordas sem ela. Conservador e documentado.
    static constexpr float kCullMarginWorld = 8.f;

    // --- conteúdo -----------------------------------------------------------

    /// Quads de TODOS os nós da cena, em ordem depth-first estável (a ordem
    /// de desenho; o hit-test percorre de trás para frente).
    [[nodiscard]] std::vector<EntityQuad> buildQuads(
        const eng::scene::Scene& scene,
        const std::optional<eng::ecs::Entity>& selection) const;

    /// Overload com CULLING — quads FORA do rect (com
    /// margem) não entram na lista; `culledOut` (opcional) recebe quantos
    /// foram cortados (métrica do round 7: cull ≈ 180 com 200 entidades,
    /// 180 off-screen). Pais culled CONTINUAM visitando filhos (filho em
    /// vista desenha mesmo com pai fora — hierarquia não é poda).
    [[nodiscard]] std::vector<EntityQuad> buildQuads(
        const eng::scene::Scene& scene,
        const std::optional<eng::ecs::Entity>& selection,
        const CullRect& cull, std::uint32_t* culledOut) const;

    /// Preenche `out` — o host reutiliza o
    /// buffer entre frames (clear() interno preserva capacidade; o frame
    /// quente não realoca). Cull opcional (null = sem culling).
    void buildQuadsInto(std::vector<EntityQuad>& out,
                        const eng::scene::Scene& scene,
                        const std::optional<eng::ecs::Entity>& selection,
                        const CullRect* cull,
                        std::uint32_t* culledOut) const;

    /// Quads de TODAS as partículas vivas (uma por Particle de cada
    /// ParticlePool/emitter da cena). Desenhados POR CIMA das entidades —
    /// marcadores de gameplay, não selecionáveis.
    [[nodiscard]] std::vector<ParticleQuad> buildParticleQuads(
        const eng::scene::Scene& scene) const;

    /// Hit-test em coordenadas de TELA. Raio de tolerância em pixels
    /// (alvo de toque generoso — touch UX §8.8).
    [[nodiscard]] std::optional<eng::ecs::Entity> hitTest(
        const std::vector<EntityQuad>& quads, float screenX, float screenY,
        float touchRadius) const noexcept;

    /// Tamanho mínimo do quad em pixels (entidades pequenas continuam
    /// visíveis/toveicáveis em zoom baixo).
    static constexpr float kMinQuadPixels = 22.f;
    static constexpr float kQuadHalfWorld = 0.5f; ///< meia-largura padrão

    /// Quantos quads seriam desenhados (grade + entidades) — usado nos
    /// testes como prova de conteúdo sem GPU.
    [[nodiscard]] std::size_t quadCount(
        const std::vector<EntityQuad>& quads) const noexcept;

private:
    Camera2D camera_{};
    const Camera2D* gameCamera_ = nullptr;  ///< câmera de jogo
    float screenW_{1.f};
    float screenH_{1.f};
    float uiScale_{1.f};  ///< densidade do device
};

} // namespace eng::editor
