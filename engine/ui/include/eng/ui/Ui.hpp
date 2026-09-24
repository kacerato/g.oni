#pragma once

/// eng::ui — widgets, layout e draw-list.
///
/// - RETAINED-MODE puro, SEM RHI: a UI produz uma DRAW-LIST (quads de cor)
///   que o HOST desenha — eng::ui não conhece Vulkan/GLES (§6.7: a UI
///   pertence à engine; acoplamento zero com Android Views).
/// - LAYOUT: rect relativo ao pai + ANCHORS fracionários + pivot;
///   resolução-independente por DESIGN-RESOLUTION (escala px = design→
///   tela) + fator de escala (DPI).
/// - EVENTOS: fn-ptr + contexto sem captura (padrão do engine,
///   ADR-004) — Button(pressed/released/click), Slider(valueChanged).
/// - TEXTO v1: fonte 5×7 PONTILHADA em quads de cor (a abstraction RHI
///   não tem texturas — ADR-046; upgrade quando texturas existirem).
///   Image é widget com layout/hit-test e conteúdo de COR sólida v1.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "eng/input/Input.hpp"
#include "eng/math/Vec2.hpp"

namespace eng::ui {

// =============================================================================
// Geometria
// =============================================================================

struct UiRect {
    float x{0.f}; ///< relativo ao pai (esquerda)
    float y{0.f}; ///< relativo ao pai (topo)
    float w{0.f};
    float h{0.f};
};

/// Âncoras fracionárias relativas ao RETÂNGULO DO PAI.
struct UiAnchors {
    float left{0.f};   ///< fração da largura do pai
    float top{0.f};    ///< fração da altura do pai
    float right{1.f};  ///< fração da largura do pai (borda direita)
    float bottom{1.f}; ///< fração da altura do pai (borda inferior)
};

enum class WidgetKind : std::uint8_t {
    Container,
    Panel,
    Button,
    Label,
    Image,
    Slider,
    ProgressBar,
};

/// Cor RGBA [0..1].
struct UiColor {
    float r{1.f}, g{1.f}, b{1.f}, a{1.f};
};

/// Um quad da draw-list (coordenadas ABSOLUTAS de design-resolution).
struct UiQuad {
    UiRect rect{};
    UiColor color{};
    int layer{0}; ///< ordem de desenho (pais primeiro)
};

/// Callbacks sem captura.
using ButtonClicked = void (*)(void* context, std::uint32_t widgetId);
using SliderChanged = void (*)(void* context, std::uint32_t widgetId,
                               float value);

// =============================================================================
// Widget
// =============================================================================

class UiDocument;

class Widget final {
public:
    friend class UiDocument;

    [[nodiscard]] WidgetKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void setVisible(bool visible) noexcept { visible_ = visible; }
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // --- layout ---------------------------------------------------------

    void setRect(const UiRect& rect) noexcept { rect_ = rect; }
    [[nodiscard]] const UiRect& rect() const noexcept { return rect_; }
    void setAnchors(const UiAnchors& anchors) noexcept { anchors_ = anchors; }
    [[nodiscard]] const UiAnchors& anchors() const noexcept
    {
        return anchors_;
    }
    [[nodiscard]] UiRect resolvedRect() const noexcept
    {
        return resolved_;
    }

    // --- conteúdo -----------------------------------------------------------------

    void setColor(const UiColor& color) noexcept { color_ = color; }
    [[nodiscard]] const UiColor& color() const noexcept { return color_; }
    void setText(std::string_view text);
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    /// Slider/ProgressBar: 0..1.
    void setValue(float value) noexcept;
    [[nodiscard]] float value() const noexcept { return value_; }

    // --- eventos ------------------------------------------------------------

    void setOnClick(ButtonClicked callback, void* context) noexcept
    {
        onClick_ = callback;
        clickContext_ = context;
    }
    void setOnValueChange(SliderChanged callback, void* context) noexcept
    {
        onValueChange_ = callback;
        valueContext_ = context;
    }

    [[nodiscard]] Widget* parent() noexcept { return parent_; }
    [[nodiscard]] const std::vector<Widget*>& children() const noexcept
    {
        return children_;
    }

    /// Nome do widget p/ debugging (não é identidade — o id é).
    void setName(std::string_view name) { name_ = std::string(name); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    Widget(WidgetKind kind, std::uint32_t id) : kind_(kind), id_(id) {}

    WidgetKind kind_{WidgetKind::Container};
    std::uint32_t id_{0};
    bool visible_{true};
    bool enabled_{true};
    bool pressed_{false};

    UiRect rect_{};
    /// Default SEM stretch (rect como está); preencher o pai exige
    /// anchors explícitos {0,0,1,1} (bug pego pelos testes: o default
    /// fill-parent sobrescrevia todo rect explícito).
    UiAnchors anchors_{0.f, 0.f, 0.f, 0.f};
    UiRect resolved_{};     ///< computado no layout (design-resolution)

    UiColor color_{};
    std::string text_;
    float value_{0.f};

    ButtonClicked onClick_{nullptr};
    void* clickContext_{nullptr};
    SliderChanged onValueChange_{nullptr};
    void* valueContext_{nullptr};

    Widget* parent_{nullptr};
    std::vector<Widget*> children_;
    std::string name_;
};

// =============================================================================
// UiDocument — árvore + layout + eventos + draw-list
// =============================================================================

class UiDocument final {
public:
    UiDocument();
    ~UiDocument();
    UiDocument(const UiDocument&) = delete;
    UiDocument& operator=(const UiDocument&) = delete;

    /// Resolução de DESIGN (§6.6: resolução-independente — os rects vivem
    /// neste espaço; a tela final aplica escala).
    void setDesignResolution(float width, float height) noexcept;
    [[nodiscard]] eng::math::Vec2 designResolution() const noexcept
    {
        return design_;
    }

    /// Fatores de exibição (tela real + escala extra de DPI).
    void setScreenSize(float width, float height) noexcept;
    void setScale(float scale) noexcept;

    /// Cria widget filho de `parent` (nullptr = filho da raiz).
    [[nodiscard]] Widget* createWidget(WidgetKind kind,
                                       Widget* parent = nullptr);

    /// Remove e destrói (filhos em cascata; raiz NÃO é removível).
    void destroyWidget(Widget* widget);

    [[nodiscard]] Widget* root() noexcept { return root_; }

    /// Recomputa resolved_ de toda a árvore (chamar após editar rects).
    void layout();

    /// Draw-list ABSOLUTA em design-resolution (pais antes dos filhos).
    [[nodiscard]] std::vector<UiQuad> buildDrawList() const;

    /// Hit-test em COORDENADAS DE TELA (converte para design antes).
    /// Top-most (filhos por cima dos pais); nullptr se nada.
    [[nodiscard]] Widget* hitTest(float screenX, float screenY);

    /// Alimenta um toque canônico: Down/Move→ pressed/hover, Up→ click.
    /// Retorna o widget atingido (ou nullptr).
    Widget* onTouch(float screenX, float screenY,
                    eng::input::TouchPhase phase);

    /// Valor do Slider a partir de um arraste (posição de tela).
    void dragSlider(Widget* slider, float screenX);

    [[nodiscard]] std::size_t widgetCount() const noexcept
    {
        return widgets_.size();
    }

private:
    void layoutChild(Widget& widget, const UiRect& parentRect) noexcept;
    void collectDraw(const Widget& widget, std::vector<UiQuad>& out,
                     int layer) const;

    Widget* root_{nullptr};
    std::vector<std::unique_ptr<Widget>> widgets_;
    eng::math::Vec2 design_{1280.f, 720.f};
    eng::math::Vec2 screen_{1280.f, 720.f};
    float scale_{1.f};
    std::uint32_t nextId_{1};
};

// =============================================================================
// Fonte 5×7 (texto pontilhado — ADR-046)
// =============================================================================

/// Um glifo aceso (linha/coluna do grid 5×7).
struct FontGlyph {
    char character{' '};
    bool pixels[7][5]{};
};

/// Glifo por caractere ASCII 32..126 (95 entradas; ' ' → vazio).
[[nodiscard]] const FontGlyph* fontGlyph(char character) noexcept;

/// Quads de um TEXTO (posição em design-resolution, altura da linha dada;
/// cada pixel aceso = 1 quad). Cor única.
[[nodiscard]] std::vector<UiQuad> textQuads(std::string_view text, float x,
                                             float y, float pixelHeight,
                                             const UiColor& color);

}  // namespace eng::ui
