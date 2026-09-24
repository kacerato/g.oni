#pragma once

/// eng::serial — envelope binário mínimo.
///
/// Layout (tudo big-endian):
///   [magic 'G','O','N','I' 4B]
///   [formatVersion u32]   — versão do PRÓPRIO envelope (kEnvelopeFormatVersion)
///   [assetType u32]       — semântica do payload (AssetType)
///   [payloadVersion u32]  — versão do SCHEMA do payload (migrations)
///   [payloadSize u64]
///   [payload bytes]
///   [crc32 u32]           — CRC-32/ISO-HDLC de TUDO antes deste campo
///
/// Sem compressão, sem criptografia, sem TLV — deliberadamente mínimo
/// (missão §2.4: "Só. Sem compression, sem fancy.").
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "eng/core/Result.hpp"

namespace eng::serial {

inline constexpr std::array<std::byte, 4> kEnvelopeMagic{
    static_cast<std::byte>('G'), static_cast<std::byte>('O'),
    static_cast<std::byte>('N'), static_cast<std::byte>('I')};

inline constexpr std::uint32_t kEnvelopeFormatVersion = 1;

/// Tamanho do envelope sem payload: magic(4)+format(4)+type(4)+schema(4)+
/// size(8)+crc(4).
inline constexpr std::size_t kEnvelopeOverhead = 28;

struct Envelope {
    std::uint32_t assetType = 0;
    std::uint32_t payloadVersion = 0;
    std::vector<std::byte> payload;
};

/// CRC-32/ISO-HDLC (poly 0xEDB88320 refletido, init/final 0xFFFFFFFF).
/// Exposto para testes e usos futuros de cache.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data);

/// Monta o envelope completo.
[[nodiscard]] std::vector<std::byte> encodeEnvelope(
    std::uint32_t assetType, std::uint32_t payloadVersion,
    std::span<const std::byte> payload);

/// Valida magic, formatVersion (futuro → NotSupported), consistência de
/// tamanho e CRC (errado → ParseError claro). NUNCA aborta.
[[nodiscard]] eng::core::Result<Envelope> decodeEnvelope(
    std::span<const std::byte> whole);

} // namespace eng::serial
