#pragma once

/// eng::serial — transformação pura entre valores e as duas formas de
/// persistência (JSON humano, envelope binário) + versionamento de schema
/// (FASE 3; ADR-030/031).
///
/// O que este módulo NÃO faz: JSON5/CBOR/MessagePack/FlatBuffers,
/// compressão, criptografia, I/O de arquivos (isso é eng::fs/chamador),
/// schemas de tipos específicos da engine (assets/scene/project definem os
/// seus usando o StructCodec).
///
/// Thread-safety: funções puras — seguras em qualquer thread
/// quando os valores de entrada não são mutados concorrentemente.
#include "eng/serial/Binary.hpp"
#include "eng/serial/Envelope.hpp"
#include "eng/serial/Json.hpp"
#include "eng/serial/JsonValue.hpp"
#include "eng/serial/SchemaVersion.hpp"
#include "eng/serial/StructCodec.hpp"
