#include <utility>

#include "eng/log/Macros.hpp"
#include "eng/rhi/Renderer.hpp"

ENG_LOG_CATEGORY("rhi")

namespace eng::rhi {
namespace {

/// Fail-safe do dtor/move: encerra sessão pendente sem lançar.
void endIfPending(const std::shared_ptr<detail::RendererState>& state,
                  std::uint64_t frameId, bool ended) noexcept {
    if (ended || state == nullptr || state->backend == nullptr || frameId == 0) {
        return;
    }
    const auto submitted = state->backend->endFrame(frameId);
    if (!submitted) {
        ENG_WARN("rhi.frame: end() fail-safe falhou: {}", submitted.error().message);
    }
}

} // namespace

Frame::Frame(std::shared_ptr<detail::RendererState> state, std::uint64_t frameId)
    : state_(std::move(state)), frameId_(frameId) {}

Frame::Frame(Frame&& other) noexcept
    : state_(std::move(other.state_)), frameId_(other.frameId_), ended_(other.ended_) {
    other.frameId_ = 0;
    other.ended_ = false;
}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        // Preserva a garantia RAII do frame atual antes de sobrescrever.
        endIfPending(state_, frameId_, ended_);
        state_ = std::move(other.state_);
        frameId_ = other.frameId_;
        ended_ = other.ended_;
        other.frameId_ = 0;
        other.ended_ = false;
    }
    return *this;
}

Frame::~Frame() {
    endIfPending(state_, frameId_, ended_);
}

eng::core::Result<void> Frame::checkUsable() const {
    if (state_ == nullptr || state_->backend == nullptr) {
        return eng::core::makeUnexpected(
            eng::core::Error{eng::core::StatusCode::InvalidArgument,
                             "rhi.frame: sessão inválida (moved-from?)"});
    }
    if (ended_) {
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::InvalidArgument,
            "rhi.frame: frame já submetido — comece outro com beginFrame()"});
    }
    return {};
}

eng::core::Result<void> Frame::clear(const ClearDesc& clear) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->frameClear(frameId_, clear);
}

eng::core::Result<void> Frame::setViewport(const Viewport& viewport) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->frameSetViewport(frameId_, viewport);
}

eng::core::Result<void> Frame::setPipeline(GraphicsPipelineHandle pipeline) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->frameSetPipeline(frameId_, pipeline);
}

eng::core::Result<void> Frame::bindVertexBuffer(BufferHandle buffer) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->frameBindVertexBuffer(frameId_, buffer);
}

eng::core::Result<void> Frame::bindIndexBuffer(BufferHandle buffer, IndexType indexType) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->frameBindIndexBuffer(frameId_, buffer, indexType);
}

eng::core::Result<void> Frame::bindTexture(TextureHandle texture, SamplerHandle sampler,
                                            std::uint32_t slot) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->frameBindTexture(frameId_, texture, sampler, slot);
}

eng::core::Result<void> Frame::setUniformData(std::span<const std::byte> data) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    if (data.size() > kMaxFrameUniformData) {
        return eng::core::makeUnexpected(eng::core::Error{
            eng::core::StatusCode::InvalidArgument,
            "rhi.frame: setUniformData excede kMaxFrameUniformData (" +
                std::to_string(kMaxFrameUniformData) + " bytes)"});
    }
    return state_->backend->frameSetUniformData(frameId_, data);
}

eng::core::Result<void> Frame::draw(std::uint32_t vertexCount, std::uint32_t firstVertex) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->frameDraw(frameId_, vertexCount, firstVertex);
}

eng::core::Result<void> Frame::drawIndexed(std::uint32_t indexCount,
                                           std::uint32_t firstIndex) {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    return state_->backend->frameDrawIndexed(frameId_, indexCount, firstIndex);
}

eng::core::Result<void> Frame::end() {
    if (auto usable = checkUsable(); !usable) {
        return eng::core::makeUnexpected(usable.error());
    }
    const auto submitted = state_->backend->endFrame(frameId_);
    if (submitted) {
        ended_ = true;
    }
    return submitted;
}

} // namespace eng::rhi
