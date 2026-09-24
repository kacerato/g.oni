#pragma once

/// eng::render::RenderTypes — draw data 2D GENÉRICA.
///
/// A "render world" do frame: uma lista de dados de desenho que NÃO
/// depende do EditorDocument — o editor a PREENCHE a partir dos seus
/// componentes (EntityQuads → itens) e o ViewportRenderer a CONSOME.
/// A forma é extensível por design: itens futuros (3D: mesh/câmera
/// perspectiva/luz 3D) entram como NOVOS campos/variantes SEM alterar os
/// consumidores existentes (2D continua um subconjunto).
///
/// Regra de honestidade: todo campo aqui é DE FATO consumido pelo
/// renderer 2D atual — nada de "reservado para o futuro" que nunca é lido.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "eng/render/FrameParams.hpp"

namespace eng::render {

/// Câmera 2D do frame (mesma convenção do viewport do editor: mundo em
/// unidades, zoom = pixels por unidade; sprite batcher converte p/ clip).
struct Camera2DState {
    float posX{0.f};
    float posY{0.f};
    float zoom{48.f};
    float screenW{1.f};  // pixels (clip do batcher = 2*px/w - 1)
    float screenH{1.f};
};

/// Um sprite a desenhar (dados — nenhum handle de GPU aqui; a textura é
/// resolvida por NOME no cache do host que possui o renderer).
struct SpriteDrawItem {
    float worldX{0.f};
    float worldY{0.f};
    float rotation{0.f};       ///< radianos (plano XY)
    float scaleX{1.f};          ///< norma das colunas do world matrix
    float scaleY{1.f};
    float u0{0.f};             ///< região UV (flip aplicado pelo consumer)
    float v0{0.f};
    float u1{1.f};
    float v1{1.f};
    float tintR{1.f};          ///< tint multiplicativo (sprite × material)
    float tintG{1.f};
    float tintB{1.f};
    float tintA{1.f};
    float sort{0.f};           ///< maior = frente (ordenação estável)
    float pivotX{0.5f};        ///< âncora local [0..1]²
    float pivotY{0.5f};
    bool flipX{false};
    bool flipY{false};
    float spritePpu{1.f};      ///< pixels por unidade de mundo
    /// Textura por NOME de asset (vazio = sem textura → caminho de cor).
    std::string texture{};
    /// Nome do shader do material ("unlit" | "lit" — ShaderLibrary).
    std::string shader{"lit"};
    /// Camada da ENTIDADE (LayerMember; "GAME" default) — agrupa o draw
    /// para o conjunto de luzes correto (mask real).
    std::string layer{"GAME"};
};

/// Item de partícula viva (marcador de gameplay — mesmo modelo do editor).
struct ParticleDrawItem {
    float worldX{0.f};
    float worldY{0.f};
    float size{0.08f};
    float rotation{0.f};
};

/// A render world de UM frame (2D). O consumidor desenha: sprites (por
/// shader, agrupados por camada+textura, com luzes filtradas por camada),
/// depois partículas.
struct DrawList {
    Camera2DState camera{};
    /// Ambiente do frame (rgb * intensidade) — sobe no bloco PerFrame.
    float ambientR{1.f};
    float ambientG{1.f};
    float ambientB{1.f};
    float ambientIntensity{1.f};

    std::vector<SpriteDrawItem> sprites{};
    std::vector<ParticleDrawItem> particles{};

    /// Luzes do frame (dados amigáveis; o consumer empacota em
    /// FrameUniforms por grupo de camada). Posição em MUNDO.
    struct LightItem {
        float worldX{0.f};
        float worldY{0.f};
        float radius{4.f};
        float intensity{1.f};
        float r{1.f};
        float g{1.f};
        float b{1.f};
        float falloff{1.5f};
        /// Camada que ilumina (Light2D.layer).
        std::string layer{"GAME"};
    };
    std::vector<LightItem> lights{};

    /// Conta as luzes válidas para UMA camada (o consumer usa isto para
    /// montar o bloco PerFrame do grupo).
    [[nodiscard]] std::size_t lightCountFor(
        const std::string& layer) const
    {
        std::size_t count = 0;
        for (const auto& light : lights) {
            if (light.layer == layer) {
                ++count;
            }
        }
        return count;
    }

    /// Empacota as luzes da camada no bloco std140 (até kMaxLights — as
    /// primeiras na ordem da lista; excedente é DESCARTADO com honestidade:
    /// o editor não promete mais de 8 luzes simultâneas por camada).
    [[nodiscard]] FrameUniforms packUniformsFor(
        const std::string& layer) const
    {
        FrameUniforms block{};
        block.ambient[0] = ambientR;
        block.ambient[1] = ambientG;
        block.ambient[2] = ambientB;
        block.ambient[3] = ambientIntensity;
        std::uint32_t count = 0;
        for (const auto& light : lights) {
            if (light.layer != layer || count >= kMaxLights) {
                continue;
            }
            block.setLight(count, light.worldX, light.worldY, light.radius,
                           light.intensity, light.r, light.g, light.b,
                           light.falloff);
            ++count;
        }
        block.setLightCount(count);
        return block;
    }
};

}  // namespace eng::render
