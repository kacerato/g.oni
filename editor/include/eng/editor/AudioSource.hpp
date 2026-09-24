#pragma once

/// eng::editor::AudioSource — componente de áudio authorável.
///
/// Expõe o backend de áudio REAL (eng::audio::AudioMixer — FASE 9) ao
/// authoring 2D: o componente é dados PUROS (refletidos/serializáveis
/// como qualquer outro do catálogo — ADR-043); o AudioTick do documento
/// (camada de composição, mesmo padrão do ScriptTick) toca o SOM REAL
/// no Play: decodifica o WAV do asset (assets/audio), sobe para o
/// AudioMixer e o device output vem do backend do host (AAudio no
/// Android, null em testes — contadores do mixer provam o caminho).
///
/// NÃO é uma camada de áudio paralela: é o MESMO mixer/Sound/Wav da
/// engine, dirigido por um componente de cena.

#include <string>

#include "eng/reflect/Reflect.hpp"

namespace eng::editor {

struct AudioSource {
    /// Nome do asset de áudio (categoria "audio" do AssetBrowser —
    /// arquivo WAV). Vazio = sem som (noop honesto).
    std::string soundAsset{};

    /// Toca ao ENTRAR em Play (autoplay do componente).
    bool playOnStart{true};

    /// Repete continuamente enquanto o Play durar.
    bool loop{false};

    /// Volume linear [0..2] (1 = natural; >1 amplifica com clamp).
    float volume{1.f};
};

} // namespace eng::editor

/// Reflexão (ADR-043: campos por caminho — Inspector/serializer).
/// Hint "audio": picker de assets de ÁUDIO do projeto (a UI lista
/// os WAVs de assets/audio — mesmo mecanismo do hint "texture").
/// clang-format off
ENG_REFLECT_BEGIN(eng::editor::AudioSource)
    ENG_REFLECT_FIELD_HINT(soundAsset, "audio")
    ENG_REFLECT_FIELD(playOnStart)
    ENG_REFLECT_FIELD(loop)
    ENG_REFLECT_FIELD(volume)
ENG_REFLECT_END()
/// clang-format on
