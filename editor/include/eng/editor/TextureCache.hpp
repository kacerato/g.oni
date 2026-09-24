#pragma once

/// eng::editor::TextureCache — nome do asset de textura → textura GPU REAL
/// (evolução P0-2/P0-3).
///
/// Fluxo: AssetBrowser cataloga bytes em assets/textures/<nome>.<ext> →
/// este cache resolve pelo NOME (AssetBrowser::resolve dentro do projeto),
/// lê via eng::fs, DECODIFICA via eng::image (PNG/JPEG → RGBA8) e sobe para
/// a GPU via RHI (createTexture + createSampler). O upload acontece UMA
/// vez por nome; destruição/reimport invalida a entrada.
///
/// Propriedade: o cache NÃO conhece projeto — o chamador (EditorHost)
/// injeta o AssetBrowser vivo do documento (troca de projeto = clear()).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "eng/rhi/Renderer.hpp"

namespace eng::editor {

class AssetBrowser;  // AssetBrowser.hpp

class TextureCache final {
public:
    TextureCache() = default;
    ~TextureCache() = default;

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    /// Entrada de GPU (handles do RHI — válidos enquanto o Renderer viver).
    struct GpuTexture {
        eng::rhi::TextureHandle texture{};
        eng::rhi::SamplerHandle sampler{};
        std::uint32_t width{0};
        std::uint32_t height{0};
        bool alpha{false};
    };

    /// Resolve `name` (asset de textura do projeto) e devolve a entrada de
    /// GPU — criando-a na primeira chamada. Falhas (asset ausente, decode
    /// quebrado, upload falhou) retornam nullo SEM cache negativo (próxima
    /// chamada tenta de novo — erros são logados uma vez por tentativa).
    [[nodiscard]] const GpuTexture* acquire(
        const AssetBrowser& assets, eng::rhi::Renderer& renderer,
        std::string_view name);

    /// Invalida TUDO (troca de projeto, reimport, destruição do renderer).
    /// Destroi os recursos GPU via renderer ANTES de esvaziar.
    /// Textura branca 2×2 para sprites de cor sólida e texto (tint = cor).
    [[nodiscard]] const GpuTexture* acquireSolid(eng::rhi::Renderer& renderer);

    void clear(eng::rhi::Renderer& renderer);

    /// Descarta as entradas SEM destruir recursos GPU (renderer já morto —
    /// ex.: destruição do ViewportRenderer; os handles morrem com ele).
    void discardAll() noexcept { entries_.clear(); }

    /// Invalida uma entrada (reimport do asset).
    void invalidate(eng::rhi::Renderer& renderer, std::string_view name);

    /// Metadados sem GPU: dimensões/alfa decodificados (para o browser
    /// listar "name\tWxH" — decode em cache, SEM upload).
    struct ImageInfo {
        std::uint32_t width{0};
        std::uint32_t height{0};
        bool alpha{false};
        bool valid{false};
    };
    [[nodiscard]] ImageInfo imageInfo(const AssetBrowser& assets,
                                      std::string_view name);

private:
    struct Entry {
        GpuTexture gpu{};
        ImageInfo info{};
        /// false = nunca decodificado com sucesso (para retry).
        bool alive{false};
    };

    std::unordered_map<std::string, Entry> entries_{};
};

}  // namespace eng::editor
