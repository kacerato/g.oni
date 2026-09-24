#include "eng/assets/AssetRegistry.hpp"

#include <algorithm>

#include "eng/serial/Json.hpp"

namespace eng::assets {

namespace {

using eng::core::Error;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

} // namespace

eng::core::Result<void> AssetRegistry::upsert(AssetMeta meta)
{
    if (meta.id.isNil()) {
        return makeUnexpected(Error{StatusCode::InvalidArgument,
                                    "AssetRegistry::upsert: id nulo"});
    }
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), meta.id,
        [](const AssetMeta& entry, const AssetId& id) { return entry.id < id; });
    if (it != entries_.end() && it->id == meta.id) {
        *it = std::move(meta); // atualização (renomeação usa isto)
        return {};
    }
    entries_.insert(it, std::move(meta));
    return {};
}

eng::core::Result<void> AssetRegistry::remove(AssetId id)
{
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const AssetMeta& entry, const AssetId& key) {
            return entry.id < key;
        });
    if (it == entries_.end() || !(it->id == id)) {
        return makeUnexpected(
            Error{StatusCode::NotFound,
                  "AssetRegistry::remove: " + id.toString() + " ausente"});
    }
    entries_.erase(it);
    return {};
}

const AssetMeta* AssetRegistry::find(AssetId id) const
{
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const AssetMeta& entry, const AssetId& key) {
            return entry.id < key;
        });
    if (it == entries_.end() || !(it->id == id)) {
        return nullptr;
    }
    return &*it;
}

std::vector<AssetMeta> AssetRegistry::all() const { return entries_; }

eng::core::Result<eng::serial::JsonValue> AssetRegistry::toJson() const
{
    using eng::serial::JsonValue;

    JsonValue assets = JsonValue::array();
    for (const AssetMeta& meta : entries_) {
        JsonValue entry = JsonValue::object();
        entry.set("id", JsonValue::string(meta.id.toString()));
        entry.set("type",
                  JsonValue::string(assetTypeName(meta.type)));
        entry.set("sourcePath", JsonValue::string(meta.sourcePath.str()));
        if (meta.size.has_value()) {
            entry.set("size", JsonValue::uinteger(*meta.size));
        }
        // contentHash: declarado no struct, NUNCA calculado na FASE 3 —
        // ausente do JSON até existir dedup real.
        assets.append(std::move(entry));
    }

    JsonValue root = JsonValue::object();
    root.set("formatVersion",
             JsonValue::uinteger(kAssetRegistryFormatVersion));
    root.set("assets", std::move(assets));
    return root;
}

eng::core::Result<std::string> AssetRegistry::serialize() const
{
    const auto json = toJson();
    if (json.isError()) {
        return makeUnexpected(json.error());
    }
    return eng::serial::dumpJson(json.value());
}

eng::core::Result<AssetRegistry> AssetRegistry::fromJson(
    const eng::serial::JsonValue& value)
{
    using eng::serial::JsonValue;

    if (!value.isObject()) {
        return makeUnexpected(
            Error{StatusCode::ParseError,
                  "AssetRegistry: raiz não é objeto"});
    }
    const auto format = value.find("formatVersion");
    if (!format.has_value() || !format->isUnsigned() ||
        format->asU64() > kAssetRegistryFormatVersion) {
        return makeUnexpected(Error{
            StatusCode::NotSupported,
            "AssetRegistry: formatVersion ausente/inválida/maior que a "
            "suportada (" +
                std::to_string(kAssetRegistryFormatVersion) + ")"});
    }
    const auto assets = value.find("assets");
    if (!assets.has_value() || !assets->isArray()) {
        return makeUnexpected(
            Error{StatusCode::ParseError,
                  "AssetRegistry: campo 'assets' ausente ou não-array"});
    }

    AssetRegistry registry;
    for (std::size_t i = 0; i < assets->size(); ++i) {
        const JsonValue entry = assets->at(i);
        if (!entry.isObject()) {
            return makeUnexpected(
                Error{StatusCode::ParseError,
                      "AssetRegistry: entrada " + std::to_string(i) +
                          " não é objeto"});
        }
        const auto id = entry.find("id");
        if (!id.has_value() || !id->isString()) {
            return makeUnexpected(
                Error{StatusCode::ParseError,
                      "AssetRegistry: entrada " + std::to_string(i) +
                          " sem 'id' string"});
        }
        const auto type = entry.find("type");
        if (!type.has_value() || !type->isString()) {
            return makeUnexpected(
                Error{StatusCode::ParseError,
                      "AssetRegistry: entrada " + std::to_string(i) +
                          " sem 'type' string"});
        }
        const auto path = entry.find("sourcePath");
        if (!path.has_value() || !path->isString()) {
            return makeUnexpected(
                Error{StatusCode::ParseError,
                      "AssetRegistry: entrada " + std::to_string(i) +
                          " sem 'sourcePath' string"});
        }

        auto parsedId = AssetId::fromString(id->asString());
        if (parsedId.isError()) {
            return makeUnexpected(parsedId.error());
        }
        auto parsedType = assetTypeFromName(type->asString());
        if (parsedType.isError()) {
            return makeUnexpected(parsedType.error());
        }

        AssetMeta meta;
        meta.id = parsedId.value();
        meta.type = parsedType.value();
        meta.sourcePath = eng::fs::Path{path->asString()};
        if (const auto size = entry.find("size");
            size.has_value() && size->isUnsigned()) {
            meta.size = size->asU64();
        }
        const auto added = registry.upsert(std::move(meta));
        if (added.isError()) {
            return makeUnexpected(added.error());
        }
    }
    return registry;
}

eng::core::Result<AssetRegistry> AssetRegistry::deserialize(
    std::string_view text)
{
    const auto parsed = eng::serial::parseJson(text);
    if (parsed.isError()) {
        return makeUnexpected(parsed.error());
    }
    return fromJson(parsed.value());
}

} // namespace eng::assets
