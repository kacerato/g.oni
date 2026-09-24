# FASE 3 — Auditoria do código real (8469f7c)

> Documento obrigatório e bloqueante (missão FASE 0). Auditoria feita sobre a
> árvore real em `8469f7c` — onde o código contradiz a especificação, o código
> vence e o desvio é registrado aqui.

## 0. Estado verificado

- HEAD: `8469f7c3c305e4e80e97d73758e4cbb2ec0564a2` ("docs: estado FASE 2…"),
  árvore limpa, 9 commits no `main` (nenhum rewrite desde `d9b2d9d`).
- Módulos existentes: `core`, `math`, `mem`, `log` (FASE 1) + `reflect`,
  `events`, `jobs`, `ecs`, `scene` (FASE 2).
- Suíte: 9 executáveis de teste, 186 casos / ~5.000 asserções (CI verde no
  `8469f7c` conforme worklog da FASE 2; revalidada localmente nesta fase).

## 1. Respostas ao questionário (§0.2 da missão)

### 1.1 Helper CMake de módulos

`eng_add_module(<name> [fontes...])` em `cmake/EngineOptions.cmake`: cria
biblioteca estática `eng_<name>` + alias `eng::<name>`, include público em
`include/`, e aplica a política completa — C++20, warnings (ADR-020),
sanitizers, LTO opcional e `-fno-exceptions -fno-rtti` (ADR-004/005).
Testes: executável + `eng_apply_test_policy(<target>)` (warnings, sanitizers,
LTO, `-fno-rtti`; exceções permanecem para o Catch2) + `add_test(...)`.
Dependências externas: `cmake/EngineDependencies.cmake` via `FetchContent`
com tag pinada e proteção de `BUILD_TESTING`.

### 1.2 Tipo de erro/result da FASE 1/2

`eng::core::Result<T, E = eng::core::Error>` (header-only, união bruta +
tag, `[[nodiscard]]`, `Unexpected`/`makeUnexpected`, especialização
`Result<void, E>`). `Error{StatusCode code; std::string message}` com
`StatusCode` canônico: `Ok, Unknown, InvalidArgument, OutOfMemory, NotFound,
AlreadyExists, ParseError, NotSupported, IOError`.

**Decisão:** reutilizar `core::Result`/`core::Error` integralmente. A FASE 3
não cria `FsError`/`SerialError` paralelos — códigos existentes cobrem o
domínio (`IOError`, `NotFound`, `ParseError`, `InvalidArgument`), e a missão
(§2.8) manda auditar e reutilizar. Erros de domínio são distinguidos pela
mensagem + código; se a discriminação programática se tornar necessária, a
extensão é adicionar valores ao `StatusCode` (evolução, não duplicação).

### 1.3 Tipo de string

`std::string` (posse) e `std::string_view` (vistas). Sem `eng::String`.
Campos de `ENG_REFLECT_FIELD` do tipo string usam `std::string`
(`PrimitiveName<std::string> = "string"`).

### 1.4 reflect registra NOME estável de tipo?

**Sim.** `TypeInfo::name` é o nome canônico string: primitivas via traço
`PrimitiveName` (`bool`, `i8..u64`, `f32`, `f64`, `string`); tipos do usuário
via `#Type` literal do macro (`ENG_REFLECT_BEGIN(eng::math::Vec3)` registra
`"eng::math::Vec3"` — nome qualificado é o recomendado). `TypeId` = FNV-1a 64
do nome canônico, determinístico entre execuções e TUs. Consulta:
`TypeRegistry::global().find(name|id) → const TypeInfo*` (ausente → nullptr,
nunca aborta). Registro idempotente (primeiro vence).

### 1.5 reflect suporta visitar membros com nome + tipo + acesso?

`PropertyInfo{name, offset, typeName, typeId}` por propriedade — acesso por
`reinterpret_cast<char*>(base) + offset`, sem API get/set tipada pronta.
A MENOR extensão necessária para a FASE 3 é **nenhuma**: o codec de
serialização resolve o tipo do campo pelo `typeName` no registry (kind
`Primitive` → valor; `Struct` → recursão; `Enum` → nome do enumerador) e lê/
escreve por offset. Limitação herdada do ADR-021 (aceita e documentada):
sem reflexão de `std::vector` — componentes persistidos na FASE 3 usam
apenas primitivas/structs planas registradas.

### 1.6 Como ecs/scene armazenam Entity e parent/child?

`Entity{u32 index; u32 generation}` — handle geracional; `World` sparse-set
por tipo (`Pool<T>`), chaves por `TypeTag<T>` (endereço de estático — sem
RTTI). `Scene` compõe um `World`; nó = entidade com `Hierarchy{parent,
children}` + `math::Transform` (ambos emplantados por `createNode`).
`kNoEntity` é a sentinela de raiz. `isNode(e) == valid(e) && has<Hierarchy>`.

**Achado crítico 1 — enumeração de componentes:** `World` NÃO expõe
enumeração dinâmica de pools (chaves opacas, mapa privado). Não é possível
perguntar "quais componentes a entidade E tem" por metadado. Solução adotada
(sem alterar FASE 2): o serializador de cena mantém um registro EXPLÍCITO de
tipos de componente serializáveis — funções tipadas sobre a API pública
(`has<T>/get<T>/emplace<T>`) — e os DADOS de cada componente são codificados
via reflect por nome (conforme a missão exige). Tipos internos
`Hierarchy`/`WorldMatrix` não passam pelo registro: viram o campo `parent`
do formato e cache derivado (não persistido), respectivamente.

**Achado crítico 2 — enumeração de nós:** `Scene` não expõe `eachNode`/
`roots`. Solução sem alterar FASE 2: `scene.world().each<Hierarchy>(...)`
visita exatamente os nós (todo nó tem `Hierarchy`; `isNode` confirma).
Entidades criadas direto no `world()` (bypass, sem `Hierarchy`) são
runtime-only e não são persistidas — documentado no ADR-033.

### 1.7 Como eng::log é chamado?

`ENG_LOG_CATEGORY("categoria")` por TU antes do uso; macros `ENG_TRACE…
ENG_FATAL("fmt {}", args)` com formatação `{}` sem exceções; curto-circuito
por nível. `Logger::get()` singleton thread-safe com sinks. Todos os módulos
novos usam isto — sem `printf`/`std::cout` (exceção registrada: `eng::mem`
usa `fprintf(stderr)` por decisão de camadas em 00-overview.md).

### 1.8 Presets CMake

`CMakePresets.json` v6, generator Ninja, `binaryDir = build/<preset>`:
`linux-debug` (Debug, ASan+UBSan, Werror, `ENG_JOBS_CATCH_EXCEPTIONS=ON`,
testPreset com `ASAN_OPTIONS=detect_leaks=1` — LSan anda junto do ASan) e
`linux-release` (Release, LTO). **Não há preset TSan** — e a missão manda NÃO
adicionar TSan na FASE 3 (quase tudo single-threaded). Sem novos presets.

### 1.9 CI

`.github/workflows/ci-linux.yml` — "CI Linux", matriz
`[linux-debug, linux-release]` em `ubuntu-24.04`: configure → build → ctest
(`--output-on-failure`), upload de artefatos em falha. A suíte nova entra
automaticamente via `add_test` — a CI é ESTENDIDA por cobertura, sem
substituição (nenhum passo removido). Runner tem rede para o novo
FetchContent (nlohmann/json).

### 1.10 third_party/ e integração de dependências

**Não existe pasta `third_party/`.** Padrão existente: `FetchContent` em
`cmake/EngineDependencies.cmake` com `GIT_TAG` pinada (`Catch2 v3.5.2`,
`GIT_SHALLOW`, `BUILD_TESTING OFF` durante o fetch). A FASE 3 segue este
padrão: `nlohmann/json v3.11.3` (MIT, header-only) por `FetchContent`,
busca incondicional (serial é dependência de build, não só de teste).
Compila com flags nativos (isas de warnings não se aplicam a dependências).

### 1.11 ADRs até 025?

**Confirmado parcialmente:** existem FISICAMENTE `ADR-021…ADR-025` em
`docs/adr/`. `ADR-001…ADR-020` são referências contratuais da missão FASE 1
(citadas em comentários/código; sem arquivo físico). A numeração física da
FASE 3 continua em **ADR-026**, conforme a lista da missão.

## 2. Desvios da especificação (código/spec vence, registrados)

| # | Desvio | Justificativa |
|---|--------|---------------|
| D1 | Gerador UUID (`Uuid128`: hi/lo, geração v4, forma canônica, parse estrito) vive em **eng::core** (arquivos novos `Uuid.hpp/.cpp`), não em `eng::assets` | Três consumidores em camadas distintas — `assets` (AssetId), `project` (ProjectId), `scene` (SceneEntityId). A regra dura "ninguém depende de scene" + o layering (scene não deve depender de assets) exigem um ancestral comum; `core` é o único. Extensão ADITIVA (nenhum arquivo existente tem semântica alterada; `Core.hpp` ganha include, `CMakeLists` ganha fonte). Alternativa `scene → assets` rejeitada. |
| D2 | Aresta `serial → fs` da proposta (§5.1) é **declarada no CMake sem consumo de símbolos** | Precedente idêntico da FASE 2 (`ecs → reflect` declarada, §B.4). serial opera sobre bytes/strings puros; I/O pertence a fs/assets. Aresta declarada com comentário documentando o estado — grafo §5.1 literalmente satisfeito, sem dependência fantasma em código. |
| D3 | Serialização de Scene/ECS vive **dentro de eng::scene** como arquivos novos (`SceneIdentity.hpp`, `SceneSerializer.hpp/.cpp`) | A missão lista 5 módulos novos + "serialização de Scene/ECS" (não é 6º módulo) e a regra dura "nenhum módulo depende de scene exceto testes em tests/" — o serializador é o único código que PRECISA dos internals de scene, logo só pode viver em scene (ou em tests/, o que rebaixaria entrega a código de teste). `eng::scene` ganha arestas PRIVATE para `serial` e `reflect` (ambas abaixo no grafo); nenhum arquivo/semântica de FASE 2 é alterado — os testes existentes permanecem intocados e verdes (critério D). Registrado no ADR-033. |
| D4 | `JsonAssetLoader` entrega **`serial::JsonValue`** para tipos Scene/Prefab/Json; a interpretação estrutural (entidades/componentes) é do `SceneSerializer` em eng::scene | `assets` não pode depender de `scene` (§5.1). A composição fim-a-fim (assets → JsonValue → SceneSerializer) é exercitada no teste de integração em `tests/`. |
| D5 | Componentes são enumerados por registro explícito tipado no serializador, não por introspecção dinâmica do `World` | Ver achado crítico 1 (§1.6). Zero alteração em FASE 2; dados continuam "por nome via reflect" como a missão exige. |

Não houve bloqueio arquitetural: nenhum item de "O QUE FAZER SE ALGO DER
ERRADO" foi acionado; `docs/phase3_blockers.md` não é necessário.

## 3. Verificação de hipóteses da auditoria externa

- Fronteira platform/fs (§2.1 da auditoria): aceita — `fs` é dono de
  `Path`/I/O; `platform` só produz raízes e depende de fs. Implementado assim.
- AssetId = UUIDv4 {hi, lo} (§2.3): aceito; gerador próprio ~80 linhas
  (`random_device` + `mt19937_64`), sem dependência externa (ver D1 para a
  localização).
- JSON puro + nlohmann (§2.4): aceito. **Nota técnica:** bibliotecas `eng::*`
  compilam com `-fno-exceptions` — o uso de nlohmann fica restrito às vias
  que não lançam: `json::parse(text, cb, /*allow_exceptions=*/false)` +
  `is_discarded()`, acesso sempre pré-checado (`is_string()`/`is_number()`
  antes de `get`), dump para serializar. ADR-030 documenta.
- Envelope binário mínimo + CRC32 + big-endian (§2.4): aceito.
- MemoryFileSystem (§2.5): aceito — os testes de assets/serial/project rodam
  sobre ele, sem tocar disco (exceto os testes do próprio fs em tmpdir).
- Project relativo (§2.6): aceito; nenhuma string absoluta persistida
  (validação ativa no parse: path absoluto em campo relativo → erro).
- SceneEntityId distinto de AssetId (§2.7): aceito (tipos fortes distintos
  sobre `core::Uuid128`).
- `Result` reutilizado (§2.8): confirmado existente e suficiente.
- Thread-safety por módulo (§2.9): declarada em ADR-034 conforme a missão.
- Sem jobs em assets (§2.10): confirmado — nenhuma aresta `assets → jobs`.
- `eng::fs::Path` envolve `std::filesystem::path` (§2.11): aceito; operações
  usam as sobrecargas `std::error_code` (sem exceções no runtime).
- Escopo de assets (§2.12): aceito; sem PNG/glTF/import/watch/hot-reload.

## 4. Ambiente de build local (revalidado nesta fase)

GCC 14.2.0, CMake 3.31.6, Ninja 1.13.2, rede OK (FetchContent). Baseline
revalidado do zero antes de qualquer código novo: `linux-debug` configure +
build + ctest verdes (ver relatório final da fase).
