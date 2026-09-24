#include "eng/editor/TextureCache.hpp"

/// eng::editor::TextureCache — implementação (evolução P0-2/P0-3).

#include <cstring>
#include <vector>

#include "eng/editor/AssetBrowser.hpp"
#include "eng/image/Image.hpp"
#include "eng/log/Macros.hpp"

namespace eng::editor {

namespace {

constexpr std::string_view kCategory = "textures";

ENG_LOG_CATEGORY("editor.texture");

}  // namespace

const TextureCache::GpuTexture* TextureCache::acquire(
    const AssetBrowser& assets, eng::rhi::Renderer& renderer,
    std::string_view name)
{
    const std::string key{name};
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.alive) {
        return &it->second.gpu;
    }

    // 1) Bytes do arquivo (via AssetBrowser — anti-traversal no Path).
    auto bytes = assets.read(kCategory, name);
    if (bytes.isError()) {
        ENG_WARN("texture '{}': leitura falhou ({})", name,
                 bytes.error().message);
        return nullptr;
    }

    // 2) Decode PNG/JPEG → RGBA8 (eng::image — sem FS no decode).
    auto decoded = eng::image::decode(std::span{bytes.value()});
    if (decoded.isError()) {
        ENG_WARN("texture '{}': {}", name, decoded.error().message);
        return nullptr;
    }
    const auto& image = decoded.value();

    // 2.5) RECOVERY P0 — ORIENTAÇÃO: stb_image decodifica top-down (linha 0
    // = TOPO da imagem), mas o espaço UV do GL tem v=0 na BASE. Sem este
    // flip na fronteira de upload, o sprite sai DE PONTA-CABEÇA no viewport
    // (o topo do quad amostrava a última linha de memória = base da imagem).
    // Contrato estabelecido: textura GPU vive em convenção GL — v=0 é a
    // base da imagem COMO EXIBIDA; o espaço UV do sprite significa
    // "imagem em pé" (v=1 = topo). Um único lugar conhece a diferença.
    std::vector<std::byte> glPixels;
    const std::size_t rowBytes =
        static_cast<std::size_t>(image.width) * 4u;
    glPixels.resize(image.pixels.size());
    for (std::uint32_t row = 0; row < image.height; ++row) {
        const std::byte* src =
            image.pixels.data() +
            static_cast<std::size_t>(image.height - 1 - row) * rowBytes;
        std::byte* dst = glPixels.data() +
                         static_cast<std::size_t>(row) * rowBytes;
        std::memcpy(dst, src, rowBytes);
    }

    // 3) Upload GPU (RHI — textura imutável RGBA8; mips para sprites).
    eng::rhi::TextureDesc desc{};
    desc.width = image.width;
    desc.height = image.height;
    desc.format = eng::rhi::Format::R8G8B8A8Srgb;
    desc.initialData = std::span{glPixels};
    desc.generateMipmaps = true;
    auto texture = renderer.createTexture(desc);
    if (texture.isError()) {
        ENG_WARN("texture '{}': upload falhou ({})", name,
                 texture.error().message);
        return nullptr;
    }

    // 4) Sampler (linear + mip linear; clamp — região UV do sprite).
    eng::rhi::SamplerDesc samplerDesc{};
    samplerDesc.minFilter = eng::rhi::FilterMode::Linear;
    samplerDesc.magFilter = eng::rhi::FilterMode::Linear;
    samplerDesc.mipFilter = eng::rhi::FilterMode::Linear;
    samplerDesc.addressU = eng::rhi::AddressMode::ClampToEdge;
    samplerDesc.addressV = eng::rhi::AddressMode::ClampToEdge;
    auto sampler = renderer.createSampler(samplerDesc);
    if (sampler.isError()) {
        (void)renderer.destroyTexture(texture.value());
        ENG_WARN("texture '{}': sampler falhou ({})", name,
                 sampler.error().message);
        return nullptr;
    }

    Entry entry{};
    entry.gpu.texture = texture.value();
    entry.gpu.sampler = sampler.value();
    entry.gpu.width = image.width;
    entry.gpu.height = image.height;
    entry.gpu.alpha = image.alpha;
    entry.info.width = image.width;
    entry.info.height = image.height;
    entry.info.alpha = image.alpha;
    entry.info.valid = true;
    entry.alive = true;

    auto [inserted, _] = entries_.insert_or_assign(key, std::move(entry));
    (void)_;
    return &inserted->second.gpu;
}

const TextureCache::GpuTexture* TextureCache::acquireSolid(
    eng::rhi::Renderer& renderer)
{
    static const std::string kKey{"@solid"};
    auto it = entries_.find(kKey);
    if (it != entries_.end() && it->second.alive) {
        return &it->second.gpu;
    }
    std::vector<std::byte> white(2u * 2u * 4u, std::byte{0xFF});
    eng::rhi::TextureDesc desc{};
    desc.width = 2;
    desc.height = 2;
    desc.format = eng::rhi::Format::R8G8B8A8Srgb;
    desc.initialData = std::span{white};
    desc.generateMipmaps = false;
    auto texture = renderer.createTexture(desc);
    if (texture.isError()) {
        return nullptr;
    }
    eng::rhi::SamplerDesc samplerDesc{};
    samplerDesc.minFilter = eng::rhi::FilterMode::Nearest;
    samplerDesc.magFilter = eng::rhi::FilterMode::Nearest;
    samplerDesc.mipFilter = eng::rhi::FilterMode::Nearest;
    samplerDesc.addressU = eng::rhi::AddressMode::ClampToEdge;
    samplerDesc.addressV = eng::rhi::AddressMode::ClampToEdge;
    auto sampler = renderer.createSampler(samplerDesc);
    if (sampler.isError()) {
        (void)renderer.destroyTexture(texture.value());
        return nullptr;
    }
    Entry entry{};
    entry.gpu.texture = texture.value();
    entry.gpu.sampler = sampler.value();
    entry.gpu.width = 2;
    entry.gpu.height = 2;
    entry.gpu.alpha = false;
    entry.alive = true;
    auto [inserted, _] = entries_.insert_or_assign(kKey, std::move(entry));
    (void)_;
    return &inserted->second.gpu;
}

void TextureCache::clear(eng::rhi::Renderer& renderer)
{
    for (auto& [name, entry] : entries_) {
        if (entry.alive) {
            (void)renderer.destroyTexture(entry.gpu.texture);
            (void)renderer.destroySampler(entry.gpu.sampler);
            entry.alive = false;
        }
    }
    entries_.clear();
}

void TextureCache::invalidate(eng::rhi::Renderer& renderer,
                              std::string_view name)
{
    const auto it = entries_.find(std::string{name});
    if (it == entries_.end()) {
        return;
    }
    if (it->second.alive) {
        (void)renderer.destroyTexture(it->second.gpu.texture);
        (void)renderer.destroySampler(it->second.gpu.sampler);
    }
    entries_.erase(it);
}

TextureCache::ImageInfo TextureCache::imageInfo(const AssetBrowser& assets,
                                                std::string_view name)
{
    const std::string key{name};
    const auto it = entries_.find(key);
    if (it != entries_.end() && it->second.info.valid) {
        return it->second.info;
    }
    // Decode SEM GPU (o browser lista dimensões antes de qualquer sprite
    // existir — e sem exigir renderer vivo).
    auto bytes = assets.read(kCategory, name);
    if (bytes.isError()) {
        return {};
    }
    auto decoded = eng::image::decode(std::span{bytes.value()});
    if (decoded.isError()) {
        return {};
    }
    ImageInfo info{};
    info.width = decoded.value().width;
    info.height = decoded.value().height;
    info.alpha = decoded.value().alpha;
    info.valid = true;
    if (it != entries_.end()) {
        it->second.info = info;  // preenche a entry existente (sem GPU ainda)
    }
    return info;
}

}  // namespace eng::editor
