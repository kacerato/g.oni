# FASE 3 — Design dos módulos (pós-auditoria)

> Design curto por módulo, conforme missão (FASE 1 do processo). Depende da
> auditoria real (`phase3_audit.md`); desvios registrados lá (D1–D5).

## Grafo entregue (FASE 3)

```
core (+Uuid128, D1)   math   mem   log          (FASE 1)
reflect  events  jobs  ecs  scene               (FASE 2)
fs       → core, log
platform → core, log, fs
serial   → core, log, reflect, fs (declarada, D2)  + nlohmann/json v3.11.3
assets   → core, log, fs, serial, reflect, events (events declarada)
project  → core, log, fs, platform, assets, serial
scene    → + serial, reflect (PRIVATE; serialização vive em scene — D3)
tests/   → todos (integração e2e)
```

Regras duras respeitadas: `fs` não depende de `platform`; `assets` não
depende de `jobs`; nenhum módulo depende de Vulkan/GL/Android/JNI; nenhum
módulo depende de `scene` exceto `tests/`.

---

## eng::fs (ADR-027)

**Responsabilidade única:** ser o dono da abstração de caminhos e de I/O de
arquivos do motor. **Dependências:** core, log. **Thread-safety:**
thread-compatible — instâncias independentes em threads distintas são
seguras; UMA instância não é safe para escrita concorrente; MemoryFileSystem
idem (sem lock interno, documentado).

**API pública (assinaturas principais):**
- `Path` — wrapper fino de `std::filesystem::path` (value type): `join`,
  `parent`, `filename`, `stem`, `extension`, `normalize` (lexically_normal),
  `isAbsolute`, `str()` (forma genérica '/'), `native()`, `isWithin(root)`
  (ambos normalizados; base para anti-traversal), comparação.
- `File` — handle RAII (`std::FILE*`): modo Read/Write binário, `read`/
  `write` em blocos, `size`. Sem buffering duplo.
- `FileSystem` — base abstrata: `exists`, `readAll` (bytes `std::vector<std::byte>`
  e texto `std::string`), `writeAll`, `remove`, `rename`, `mkdirs`,
  `list(recursive)`.
- `NativeFileSystem : FileSystem` — `std::filesystem` + `std::FILE*`,
  sobrecargas `std::error_code` (runtime sem exceções).
- `MemoryFileSystem : FileSystem` — árvore em memória (`std::map` por Path
  normalizado) para testes; round-trip completo das operações.

**Não faz:** watch de arquivos, permissões, attrs, VFS/mapeamentos,
async — nada disso na FASE 3. Não menciona Android/JNI. Erros via
`Result<T>` com `core::Error` (`IOError`/`NotFound`/`InvalidArgument`).

## eng::platform (ADR-026)

**Responsabilidade única:** produzir FATOSES do sistema hospedeiro — quem
somos, onde estão as raízes conhecidas, quais variáveis de ambiente existem.
**Dependências:** core, log, fs. **Thread-safety:** todas as funções são
constes/reatradas após inicialização; `Environment::set/unset` não é safe
contra escrita concorrente (wrapper de setenv).

**API pública:** `PlatformInfo{name, arch, endianness, buildType,
sanitizers}` (determinístico por build — macros do compilador; é o ÚNICO
módulo autorizado a ter `#ifdef` por SO); `PlatformPaths{userDataRoot,
cacheRoot, tempRoot, executableRoot}` (Linux: XDG/`$HOME`/`/tmp`/
`/proc/self/exe`; usa `fs::Path` como value type); `Environment::get/set/
unset`. `ProcessInfo::currentExecutablePath()`.

**Não faz:** I/O de arquivos, tempo (core/jobs cobrem), contagem de CPUs,
logging, thread pool, display/window — nada "guarda-chuva".

## eng::core::Uuid128 (extensão aditiva — D1, ADR-028)

`struct Uuid128{u64 hi, lo}` POD, comparável/hashable; `Uuid128::generate()`
(v4: `random_device` semeando `mt19937_64`, 122 bits aleatórios, versão/
variante fixados); `toString()`/`fromString()` com forma canônica estrita
`xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx` (minúsculas, hífen, sem chaves);
`toBytesBE()`/`fromBytesBE()` (16B big-endian). **Não faz:** v7/URN/
namespace/ordenação RFC 4122 — só o que a engine consome.

## eng::serial (ADR-030, ADR-031)

**Responsabilidade única:** transformação pura entre valores do motor e as
duas formas de persistência (JSON humano, envelope binário) + infraestrutura
de versionamento de schema. **Dependências:** core, log, reflect (fs
declarada — D2). **Thread-safety:** funções puras — seguras em qualquer
thread quando os valores de entrada não são mutados concorrentemente.

**API pública:**
- `JsonValue` — wrapper MÍNIMO verificado sobre `nlohmann::json`
  (holding por valor); leitura SEMPRE pré-checada (`isString/isNumber…` +
  `asString/asF32…`), escrita tipada. Sem exceções escaparem (biblioteca
  compilada `-fno-exceptions` — via `allow_exceptions=false`, ADR-030).
- `parseJson(text, {maxDepth, maxSize}) → Result<JsonValue>`;
  `dumpJson(JsonValue) → std::string` (determinístico: objetos com chaves
  ordenadas — `std::map` interno do nlohmann).
- `StructCodec` — codifica/decodifica structs REGISTRADAS no reflect
  (`TypeInfo` → objeto JSON por propriedade; primitivas por kind; structs
  por recursão; enums por NOME de enumerador). Erro claro em propriedade
  ausente/tipo errado/desconhecida.
- `BinaryWriter/Reader` — primitivas big-endian (u8..u64, i32/i64, f32/f64,
  bytes, uuid16) sobre buffers; `Reader` valida limites.
- Envelope: `magic 'G','O','N','I'` + `formatVersion u32` + `assetType u32`
  + `payloadVersion u32` + `payloadSize u64` + payload + `crc32 u32`
  (CRC-32/ISO-HDLC tabulado). `writeEnvelope/readEnvelope`.
- `SchemaVersion` + `Migration{from,to,fn}` + `MigrationRegistry` —
  infraestrutura; **nenhuma migration ativa** (ADR-031).

**Não faz:** JSON5/CBOR/MessagePack/FlatBuffers, compressão, criptografia,
I/O de arquivos, schemas de tipos específicos da engine (isso é de assets/
scene/project), migrations reais.

## eng::assets (ADR-028, ADR-029)

**Responsabilidade única:** identidade, catálogo e ciclo de vida de assets.
**Dependências:** core, log, fs, serial, reflect (events declarada — FASE 4
introduz `loadAsync` e a consome). **Thread-safety:** `AssetManager` é
single-threaded na FASE 3 (documentado); `AssetRegistry` idem;
`AssetResolver`/loaders são puros após construção.

**API pública:**
- `AssetId` — tipo forte sobre `Uuid128` (não conversível implicitamente);
  geração/parse/formato idênticos ao Uuid128; string canônica em JSON,
  16B BE em binário.
- `AssetType` — enum: `Scene, Prefab, Json` implementados;
  `Texture, Mesh, Material, Shader, Audio, Script` RESERVADOS (declarados,
  não implementados — sem loader); `Unknown`.
- `AssetMeta{id, type, sourcePath (relativo), size?, contentHash?}` —
  contentHash não calculado na FASE 3 (campo declarado).
- `AssetRegistry` — mapa persistente id↔meta em `asset_registry.json`
  (ordem determinística por (hi,lo)); renomear = atualizar sourcePath —
  referências por AssetId permanecem válidas.
- `AssetResolver` — `resolve(id) → Result<Path relativa aprovada>`; consulta
  registry + resolve via raiz do projeto; **rejeita traversal** (path
  normalizado deve permanecer dentro da raiz — `fs::Path::isWithin`).
- `AssetManager` — cache single-threaded:
  `load<T>(id) → Result<AssetHandle<T>>` (registry → resolver → fs →
  loader); `getLoaded<T>(id) → optional<AssetHandle<T>>`; `unload(id)`.
  Handles seguram `shared_ptr` — unload remove do cache, dados vivem
  enquanto houver handle. Sem futures/callbacks/jobs.
- `IAssetLoader`/`LoaderRegistry` (TypeTag, sem RTTI) +
  `JsonAssetLoader` — carrega `Scene/Prefab/Json` → `serial::JsonValue`
  (interpretação estrutural é do consumidor — D4).

**Não faz:** GPU, importadores reais, dedup por content-hash, watch, hot
reload, async (FASE 4), referências fortes entre assets.

## eng::project (ADR-032)

**Responsabilidade única:** descrever e localizar um projeto de jogo.
**Dependências:** core, log, fs, platform, assets, serial. **Thread-safety:**
inicialização single-threaded; após construído, read-only.

**API pública:** `ProjectId` (tipo forte sobre Uuid128); `ProjectConfig
{formatVersion, projectId, name, engineVersion, assetRegistryPath,
sceneRoots}`; `ProjectPaths` — resolve `assetsRoot/cacheRoot/buildRoot/
scenesRoot` RELATIVOS ao diretório do arquivo de projeto (nunca persiste
absolutos); `ProjectFile::parse/serialize` de `project.goni.json`
(validações: path absoluto em campo relativo → rejeitado; formatVersion
maior que o suportado → erro claro; JSON corrompido → erro claro).

**Não faz:** build/export, migração de projetos antigos, múltiplos projetos
por processo (ok criar vários, mas sem gestão), settings de editor.

## Serialização de Scene/ECS (em eng::scene — D3, ADR-033)

**Responsabilidade única:** persistir/restaurar o GRAFO de nós de uma
`Scene` em JSON determinístico. **Dependências novas de scene:** serial,
reflect (PRIVATE). **Thread-safety:** operações de (de)serialização não são
concorrentes sobre a mesma Scene (single-threaded, documentado).

**API pública:** `SceneEntityId` (tipo forte sobre Uuid128) + componente
`SceneIdentity{SceneEntityId id}`; `SceneSerializer::save(const Scene&) →
Result<std::string>` (atribui SceneEntityId a nós sem id — efeito visível
na cena) e `SceneSerializer::load(Scene&, text) → Result<void>>` +
`registerComponentType<T>(name)` (registro explícito — achado crítico 1).

**Formato (missão §2.7):** entidades ordenadas por SceneEntityId (hi,lo);
componentes por nome de tipo; `Hierarchy` → campo `parent` (uuid|null);
`WorldMatrix` não persistido (cache derivado); referência de asset quebrada
→ warning via `eng::log`, cena carrega; `serialize(deserialize(x)) ==
x` byte a byte (floats via shortest-round-trip do dump; chaves ordenadas).

**Não faz:** prefabs aninhados, patches/overrides, instancing, binário de
cena (o envelope é para cache futuro), migração entre versões de formato
(infraestrutura existe em serial, sem migrations ativas).

## tests/ (integração — novo diretório raiz)

E2E: `project.goni.json` (em MemoryFileSystem) → `ProjectPaths` →
`AssetRegistry` com cena registrada → `AssetManager::load` →
`SceneSerializer::load` → mutação leve → `save` → bytes idênticos ao
esperado; e bateria de corrupção por estágio (JSON ruim, envelope com CRC
errado, magic errado, versão futura, traversal) → erro claro, sem crash.
