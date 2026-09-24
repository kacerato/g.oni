#include "eng/ui/Ui.hpp"

/// eng::ui — implementação.

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace eng::ui {

namespace {

/// Margem interna dos widgets de conteúdo (fração da própria altura).
constexpr float kPaddingFraction = 0.12f;

[[nodiscard]] bool contains(const UiRect& rect, float x, float y) noexcept
{
    return x >= rect.x && y >= rect.y && x <= rect.x + rect.w &&
           y <= rect.y + rect.h;
}

}  // namespace

// =============================================================================
// Widget
// =============================================================================

void Widget::setText(std::string_view text)
{
    if (kind_ == WidgetKind::Label || kind_ == WidgetKind::Button) {
        text_ = std::string(text);
    }
}

void Widget::setValue(float value) noexcept
{
    if (kind_ == WidgetKind::Slider || kind_ == WidgetKind::ProgressBar) {
        value_ = std::clamp(value, 0.f, 1.f);
    }
}

// =============================================================================
// UiDocument
// =============================================================================

UiDocument::UiDocument()
{
    root_ = new Widget(WidgetKind::Container, 0);
    widgets_.emplace_back(root_);
    root_->setName("root");
}

UiDocument::~UiDocument() = default;

void UiDocument::setDesignResolution(float width, float height) noexcept
{
    design_ = {width > 0.f ? width : 1.f, height > 0.f ? height : 1.f};
}

void UiDocument::setScreenSize(float width, float height) noexcept
{
    screen_ = {width > 0.f ? width : 1.f, height > 0.f ? height : 1.f};
}

void UiDocument::setScale(float scale) noexcept
{
    scale_ = scale > 0.05f ? scale : 1.f;
}

Widget* UiDocument::createWidget(WidgetKind kind, Widget* parent)
{
    Widget* widget = new Widget(kind, nextId_++);
    widgets_.emplace_back(widget);
    Widget* actualParent = parent != nullptr ? parent : root_;
    actualParent->children_.push_back(widget);
    widget->parent_ = actualParent;
    return widget;
}

void UiDocument::destroyWidget(Widget* widget)
{
    if (widget == nullptr || widget == root_) {
        return;
    }
    // Coleta a subárvore (folhas primeiro — irrelevante, ids apenas saem).
    std::vector<Widget*> subtree;
    subtree.push_back(widget);
    for (std::size_t i = 0; i < subtree.size(); ++i) {
        for (Widget* child : subtree[i]->children_) {
            subtree.push_back(child);
        }
    }
    if (widget->parent_ != nullptr) {
        auto& siblings = widget->parent_->children_;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), widget),
                       siblings.end());
    }
    for (Widget* node : subtree) {
        widgets_.erase(std::remove_if(widgets_.begin(), widgets_.end(),
                                      [node](const std::unique_ptr<Widget>& w) {
                                          return w.get() == node;
                                      }),
                       widgets_.end());
    }
}

void UiDocument::layoutChild(Widget& widget,
                             const UiRect& parentRect) noexcept
{
    // Anchors esticam o rect contra frações do pai; sem âncora ativa
    // (left==right==0 e top==bottom==0), o rect fica onde está.
    UiRect resolved = widget.rect_;
    resolved.x += parentRect.x + widget.anchors_.left * parentRect.w;
    resolved.y += parentRect.y + widget.anchors_.top * parentRect.h;
    if (widget.anchors_.right > widget.anchors_.left ||
        (widget.anchors_.left != 0.f || widget.anchors_.right != 0.f)) {
        const float anchorWidth =
            (widget.anchors_.right - widget.anchors_.left) * parentRect.w;
        if (anchorWidth > 0.f) {
            resolved.w = anchorWidth;
        }
    }
    if (widget.anchors_.bottom > widget.anchors_.top ||
        (widget.anchors_.top != 0.f || widget.anchors_.bottom != 0.f)) {
        const float anchorHeight =
            (widget.anchors_.bottom - widget.anchors_.top) * parentRect.h;
        if (anchorHeight > 0.f) {
            resolved.h = anchorHeight;
        }
    }
    widget.resolved_ = resolved;

    for (Widget* child : widget.children_) {
        layoutChild(*child, resolved);
    }
}

void UiDocument::layout()
{
    // Raiz cobre a design-resolution inteira.
    UiRect rootRect{0.f, 0.f, design_.x, design_.y};
    root_->resolved_ = rootRect;
    for (Widget* child : root_->children_) {
        layoutChild(*child, rootRect);
    }
}

void UiDocument::collectDraw(const Widget& widget, std::vector<UiQuad>& out,
                             int layer) const
{
    if (!widget.visible_) {
        return;
    }
    const float pad = std::max(widget.resolved_.w, widget.resolved_.h) *
                      kPaddingFraction;

    switch (widget.kind_) {
    case WidgetKind::Container:
        // Container só organiza (sem fundo) — desenha os filhos.
        break;
    case WidgetKind::Panel:
    case WidgetKind::Image: {
        UiQuad quad;
        quad.rect = widget.resolved_;
        quad.color = widget.color_;
        quad.layer = layer;
        out.push_back(quad);
        break;
    }
    case WidgetKind::Button: {
        // Fundo (+borda clara quando pressed).
        UiQuad quad;
        quad.rect = widget.resolved_;
        quad.color = widget.color_;
        if (widget.pressed_) {
            quad.color.r = std::clamp(quad.color.r + 0.25f, 0.f, 1.f);
            quad.color.g = std::clamp(quad.color.g + 0.25f, 0.f, 1.f);
            quad.color.b = std::clamp(quad.color.b + 0.25f, 0.f, 1.f);
        }
        quad.layer = layer;
        out.push_back(quad);
        if (!widget.text_.empty()) {
            const float pixel = std::max(
                1.f, (widget.resolved_.h - pad) / 7.f);
            auto quads = textQuads(widget.text_,
                                   widget.resolved_.x + pad,
                                   widget.resolved_.y + pad, pixel,
                                   UiColor{0.f, 0.f, 0.f, 1.f});
            for (auto& text : quads) {
                text.layer = layer + 1;
            }
            out.insert(out.end(), quads.begin(), quads.end());
        }
        break;
    }
    case WidgetKind::Label: {
        if (!widget.text_.empty()) {
            const float pixel = std::max(1.f, widget.resolved_.h / 7.f);
            auto quads = textQuads(widget.text_, widget.resolved_.x,
                                   widget.resolved_.y, pixel, widget.color_);
            for (auto& text : quads) {
                text.layer = layer;
            }
            out.insert(out.end(), quads.begin(), quads.end());
        }
        break;
    }
    case WidgetKind::Slider: {
        // Trilho + knob na posição do valor.
        UiQuad track;
        track.rect = widget.resolved_;
        track.color = UiColor{0.25f, 0.27f, 0.30f, 1.f};
        track.layer = layer;
        out.push_back(track);
        UiQuad fill;
        fill.rect = widget.resolved_;
        fill.rect.w *= widget.value_;
        fill.color = widget.color_;
        fill.layer = layer + 1;
        out.push_back(fill);
        UiQuad knob;
        knob.rect.h = widget.resolved_.h;
        knob.rect.w = std::max(4.f, widget.resolved_.h * 0.6f);
        knob.rect.x = widget.resolved_.x +
                      widget.value_ *
                          std::max(0.f, widget.resolved_.w - knob.rect.w);
        knob.rect.y = widget.resolved_.y;
        knob.color = UiColor{0.95f, 0.96f, 0.98f, 1.f};
        knob.layer = layer + 2;
        out.push_back(knob);
        break;
    }
    case WidgetKind::ProgressBar: {
        UiQuad track;
        track.rect = widget.resolved_;
        track.color = UiColor{0.20f, 0.22f, 0.25f, 1.f};
        track.layer = layer;
        out.push_back(track);
        UiQuad fill;
        fill.rect = widget.resolved_;
        fill.rect.w *= widget.value_;
        fill.color = widget.color_;
        fill.layer = layer + 1;
        out.push_back(fill);
        break;
    }
    }

    for (const Widget* child : widget.children_) {
        collectDraw(*child, out, layer + 1);
    }
}

std::vector<UiQuad> UiDocument::buildDrawList() const
{
    std::vector<UiQuad> quads;
    collectDraw(*root_, quads, 0);
    return quads;
}

Widget* UiDocument::hitTest(float screenX, float screenY)
{
    // Tela → design (escala uniforme por design-resolution + fator DPI).
    const float toDesignX = design_.x / (screen_.x * scale_);
    const float toDesignY = design_.y / (screen_.y * scale_);
    const float dx = screenX * toDesignX;
    const float dy = screenY * toDesignY;

    // Top-most: depth-first REVERSO (últimos filhos por cima).
    const auto visit = [&](auto&& self, Widget& node) -> Widget* {
        for (auto it = node.children_.rbegin(); it != node.children_.rend();
             ++it) {
            Widget* child = *it;
            if (!child->visible_) {
                continue;
            }
            if (Widget* hit = self(self, *child)) {
                return hit;
            }
        }
        if (&node != root_ && contains(node.resolved_, dx, dy)) {
            return &node;
        }
        return nullptr;
    };
    return visit(visit, *root_);
}

Widget* UiDocument::onTouch(float screenX, float screenY,
                            eng::input::TouchPhase phase)
{
    Widget* hit = hitTest(screenX, screenY);
    if (hit == nullptr || !hit->enabled_) {
        return nullptr;
    }
    if (hit->kind() == WidgetKind::Button) {
        if (phase == eng::input::TouchPhase::Down) {
            hit->pressed_ = true;
        } else if (phase == eng::input::TouchPhase::Up ||
                   phase == eng::input::TouchPhase::Cancelled) {
            if (hit->pressed_ && phase == eng::input::TouchPhase::Up) {
                if (hit->onClick_ != nullptr) {
                    hit->onClick_(hit->clickContext_, hit->id_);
                }
            }
            hit->pressed_ = false;
        }
    } else if (hit->kind() == WidgetKind::Slider) {
        if (phase == eng::input::TouchPhase::Down ||
            phase == eng::input::TouchPhase::Move) {
            dragSlider(hit, screenX);
        }
    }
    return hit;
}

void UiDocument::dragSlider(Widget* slider, float screenX)
{
    if (slider == nullptr || slider->kind() != WidgetKind::Slider) {
        return;
    }
    const float toDesignX = design_.x / (screen_.x * scale_);
    const float dx = screenX * toDesignX;
    const float innerW = slider->resolved_.w;
    float value = 0.f;
    if (innerW > 0.f) {
        value = (dx - slider->resolved_.x) / innerW;
    }
    const float clamped = std::clamp(value, 0.f, 1.f);
    if (clamped != slider->value_) {
        slider->value_ = clamped;
        if (slider->onValueChange_ != nullptr) {
            slider->onValueChange_(slider->valueContext_, slider->id_,
                                    clamped);
        }
    }
}

}  // namespace eng::ui
