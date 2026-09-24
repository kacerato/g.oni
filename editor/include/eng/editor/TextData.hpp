#pragma once

/// TextData — texto desenhado na cena com a fonte 5×7 do eng::ui.
///
/// Em mundo, o texto fica na posição da entidade. Com `screenSpace`, fica
/// preso à tela (placar, mensagens) em `screenX/screenY` (frações da tela,
/// origem no canto superior esquerdo). Scripts escrevem `e.text.text`.

#include <cstdint>
#include <string>

#include "eng/reflect/Reflect.hpp"

namespace eng::editor {

enum class TextAlign : std::uint8_t { Left = 0, Center = 1, Right = 2 };

struct TextData {
    std::string text{"Texto"};
    float size{0.6f};   ///< altura da letra em unidades de mundo
    float colorR{1.f};
    float colorG{1.f};
    float colorB{1.f};
    float opacity{1.f};
    TextAlign align{TextAlign::Center};
    bool screenSpace{false};
    float screenX{0.5f};
    float screenY{0.12f};
    float sort{100.f};  ///< ordem de desenho (acima dos sprites por padrão)
};

}  // namespace eng::editor

ENG_REFLECT_ENUM_BEGIN(eng::editor::TextAlign)
    ENG_REFLECT_ENUM_VALUE(Left)
    ENG_REFLECT_ENUM_VALUE(Center)
    ENG_REFLECT_ENUM_VALUE(Right)
ENG_REFLECT_ENUM_END()

ENG_REFLECT_BEGIN(eng::editor::TextData)
    ENG_REFLECT_FIELD(text)
    ENG_REFLECT_FIELD(size)
    ENG_REFLECT_FIELD_HINT(colorR, "color:0:r")
    ENG_REFLECT_FIELD_HINT(colorG, "color:0:g")
    ENG_REFLECT_FIELD_HINT(colorB, "color:0:b")
    ENG_REFLECT_FIELD(opacity)
    ENG_REFLECT_FIELD_AS(align, "eng::editor::TextAlign")
    ENG_REFLECT_FIELD(screenSpace)
    ENG_REFLECT_FIELD(screenX)
    ENG_REFLECT_FIELD(screenY)
    ENG_REFLECT_FIELD(sort)
ENG_REFLECT_END()
