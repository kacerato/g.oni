# FASE 12 — Build & Export: design formal (pré-implementação)

> **Estado**: design aprovado ANTES da implementação (mesma disciplina da
> FASE 11). Fonte canônica deste documento; divergência código↔aqui = bug
> do código.
>
> **Princípio norteador** (missão): ENGINE e DADOS DE PROJETO são coisas
> SEPARADAS — o pipeline empacota dados, nunca recompila o motor e nunca
> codifica um projeto específico no código.

## 1. Pipeline (missão: as 10 etapas)

```
projeto (fs) ─→ 1 loadProject   project.goni.json + asset_registry.json
              ─→ 2 loadBuildConfig  build.json (paths RELATIVOS)
              ─→ 3 buildManifest  enumeração determinística + hashes
              ─→ 4 depGraph       cenas de entrada → scripts/refs
              ─→ 5 refScan        conjunto alcançável + não-usados (WARN)
              ─→ 6 validation     bloqueante: ausente/duplicado/inválido/
                                 script não compila
              ─→ 7 cooker        asset → envelope GONI (SOURCE/DERIVED)
              ─→ 8 cache         chave determinística conteúdo+versão
              ─→ 9 bundle        bundle.goni (container de envelopes)
              ─→ 10 verify+export decode+CRC+rescan; alvo materializa
```

Cada etapa devolve `Result` com erros precisos (path, id, motivo) —
nunca exceções (ADR-004), nunca abort.

## 2. BuildConfig — `build.json` (NA RAIZ do projeto)

```json
{
  "formatVersion": 1,
  "targets": ["android-arm64", "linux-dev"],
  "entryScenes": ["scenes/main.json"],
  "cacheDir": "build-cache"
}
```

Regras:
- TODOS os paths são RELATIVOS à raiz do projeto — path ABSOLUTO =
  erro de compilação da config (missão: "配置用相对路径"; também fecha a
  porta para state fora do projeto);
- `targets`: subconjunto de {"android-arm64", "linux-dev"}; alvo
  desconhecido = ERRO; VAZIO = erro (nada a exportar);
- `entryScenes`: ≥ 1 cena de entrada (raízes do grafo de dependência);
  ausente no disco = ERRO bloqueante;
- `cacheDir` default "build-cache" (relativo);
- `formatVersion` ≠ 1 = NotSupported (política do serial, ADR-030).

## 3. Manifest — gerado (determinístico)

Enumeração de TODOS os assets do `asset_registry.json` ORDENADOS por
AssetId (hi, lo — mesma ordem do registry) + cenas de entrada:

```json
{
  "formatVersion": 1,
  "projectName": "…", "projectId": "uuid",
  "cookerVersion": 1,
  "entryScenes": ["scenes/main.json"],
  "assets": [
    {"id":"uuid","type":"Scene","path":"scenes/main.json",
     "contentHash":"0123456789abcdef"} ]
}
```

- `contentHash` = **FNV-1a 64** do CONTEÚDO do arquivo (hex 16 dígitos) —
  chave do cache (§8) e checagem de reprodução;
- determinismo: mesma árvore de arquivos ⇒ mesmos bytes do manifest.

## 4. Grafo de dependências + scan de referências (§4/§5)

Nós: cenas (arquivos .json) + assets (por AssetId). Arestas v1:

1. **entrada → cena**: cada `entryScenes[i]` é raiz;
2. **cena → asset**: o CONTEÚDO JSON da cena é varrido em busca de
   QUALQUER AssetId canônico (regex uuid-v4) PRESENTE no registro —
   referência por identidade estável (ADR-028), independente de campo;
   o scanner é tipo-agnóstico (funciona para qualquer componente que
   apareça a portar AssetId no futuro — nada de hard-code de campo);
3. **cena → script embutido**: `NiScriptComponent.source` dentro da
   cena (componente do catálogo) — extraído por parse JSON dos
   componentes do tipo conhecido, VALIDADO compilando (não empacota
   script quebrado);
4. **asset script standalone** (assets/scripts/*.nis do registro):
   também compilado para validação.

NÃO-ALCANÇÁVEL (asset do registro que nenhuma cena referencia):
**WARN no log** + listado no manifest como "unused" — não é erro (cenas
futuras podem referenciar; a missão pede detectar, não punir). Ausente
no disco (registrado, arquivo sumiu) = **ERRO bloqueante**. Duplicado
(dois AssetIds apontando o MESMO sourcePath) = **ERRO bloqueante**
(empacotar duas identidades para o mesmo arquivo é ambíguo).

## 5. Cooker (§7) — envelope GONI por asset

Cada asset vira UM envelope GONI (ADR-030 — magic+format+type+schema+
size+payload+crc32):

| AssetType do registro | payload | payloadVersion | classificação |
|---|---|---|---|
| Scene/Prefab/Json | bytes VERBATIM do fonte (JSON válido checado) | 1 | **SOURCE** |
| Script (.nis) | bytes do fonte (JÁ validado por compilação §4.3) | 1 | **SOURCE** |
| demais (Texture…reservados) | bytes verbatim | 1 | SOURCE |

- **SOURCE/DERIVED** (missão): a classificação é um CAMPO do formato
  (payload do bundle, §6). Em v1 TODA entrada cozida é SOURCE (não há
  conversão de formato — textura comprimida/GLES-sRGB são cooks DERIVED
  futuros com payloadVersion próprio); o mecanismo + marcador existem
  DESDE JÁ para que o formato do bundle não mude quando cooks derivados
  chegarem (decisão de compatibilidade);
- **scripts**: validados por compilação no BUILD (bloqueante) mas
  EMPACOTADOS como fonte — o runtime compila no load (a serialização
  de bytecode é futuro declarado do ADR-049; recompilar é barato e
  mantém uma única fonte de verdade). O manifest registra
  `scriptsValidated` (contagem) como evidência.

## 6. Bundle — `bundle.goni` (container de envelopes)

UM envelope GONI de nível externo:

```
assetType      = 1000 (BUNDLE — valor fora do range do AssetType,
                     reservado pela FASE 12, ADR-050)
payloadVersion = 1
payload:
  [manifestSize u32 BE] [manifest JSON bytes]
  [entryCount u32 BE]
  para cada entrada (ORDENADA por AssetId — determinístico):
    [envelopeSize u32 BE] [envelope GONI do asset]
    [entryMeta: id(32 hex) + type(u32) + pathLen(u32) + path bytes
                + flags u32 (bit0 = SOURCE/DERIVED… v1: sempre SOURCE)]
```

Big-endian nos campos do container (consistente com o envelope).
Determinismo: mesma árvore ⇒ mesmos bytes do bundle (testado).

## 7. Validação do bundle (§10)

Re-abrir o bundle gerado: decode do envelope externo (CRC), decode de
TODAS as entradas (CRC cada), conferir manifest ↔ entradas (contagem e
ids), e CRC do conteúdo == contentHash do manifest (FNV conferido).
Falha qualquer = erro com a entrada culpada.

## 8. Cache (§8) — determinístico

- chave = **FNV-1a 64** de: `cookerVersion ‖ assetType(u32BE) ‖ bytes do
  fonte` (hex, nome do arquivo em `<cacheDir>/<key>`);
- conteúdo do arquivo de cache = o envelope GONI pronto do asset;
- HIT: envelope reutilizado SEM recocinar (e sem RELER o fonte além do
  hash — o hash já foi computado no manifest); MISS: cozinha + grava;
- INVALIDAÇÃO é automática e conteúdo-endereçada: mudou o fonte OU a
  versão do cooker OU o tipo ⇒ outra chave (miss). Nada de timestamps;
- clean build = `forceCook=true` (ignora o cache — teste); incremental =
  padrão (teste: 2º build é hit-only);
- o cache é um DIRETÓRIO DE ARTEFATOS (gitignored), não estado lógico:
  apagar é sempre seguro.

## 9. Export — alvos (§11)

- **android-arm64** (PRIORITÁRIO — missão): materializa
  `<out>/android-arm64/<projectName>.goni` (o bundle) +
  `<out>/android-arm64/INSTALL.md` documentando o caminho de instalação
  no APK existente (`filesDir/projects/<projectId>/bundle.goni` — o
  runtime arm64-v8a JÁ embute engine+editor; dados são INJETADOS, nunca
  recompilados). INTEGRIDADE: o bundle alvo declara
  `"abi":"arm64-v8a"` no meta;
- **linux-dev**: `<out>/linux-dev/<projectName>.goni` + um `README.md`
  com o layout do bundle e como validá-lo.
- `<out>` default: `export/` na raiz do projeto (relativo! configurável
  na BuildConfig como `outDir`).

**Fora de escopo v1 (honesto)**: loader do bundle NO runtime Android
(browsing de projetos na Activity) — o formato e a validação existem e
são testados; o consumidor de runtime é FASE futura declarada. Também
fora: assinatura de APK, splits por ABI, cook DERIVED, upload de loja.

## 10. Módulo `engine/build`

```
engine/build (nova)
  deps: core, fs, serial, project, assets, niscript   (grafo 00-overview
        atualizado: build → project & assets & serial (+core/fs/niscript))
  NÃO conhece: rhi, android, editor, physics, scene (scan de refs é
        agnóstico — JSON text scan por AssetId)
```

API pública (`eng/build/Build.hpp`):
```cpp
struct BuildReport {   // resultado completo (evidência)
    std::string manifestJson;
    std::size_t assetsCooked, cacheHits, cacheMisses;
    std::vector<std::string> unusedAssets;  // WARN
    std::vector<std::string> targets;
    std::vector<std::byte> bundleBytes;      // pós-verificação
};
Result<BuildReport> buildProject(FileSystem&, Path projectRoot,
                                 const BuildOptions& opts,
                                 std::vector<BuildDiag>* diags);
// BuildOptions { bool forceCook = false; }   // clean vs incremental
```

## 11. Plano de testes (missão: lista obrigatória)

1. projeto vazio (sem assets) — erro claro ("sem assets")? NÃO: projeto
   mínimo com 1 cena passa; VAZIO (nem entryScene) = erro de config;
2. projeto mínimo — build ok, manifest determinístico (2× = mesmos
   bytes), bundle decodifica, CRC ok;
3. asset ausente (registrado, arquivo sumiu) — ERRO bloqueante;
4. asset não usado — WARN listado no report, build SEGUE;
5. assets duplicados (2 ids, mesmo sourcePath) — ERRO;
6. script que não compila (embutido em cena) — ERRO bloqueante;
7. script standalone inválido — ERRO;
8. manifest inválido (formatVersion errado) — ERRO NotSupported;
9. target inválido — ERRO; targets vazio — ERRO;
10. entryScene inexistente — ERRO;
11. path ABSOLUTO na config — ERRO (missão: relativos);
12. cache HIT (2º build incremental: hits=N, misses=0);
13. cache MISS após mudar conteúdo (chave muda);
14. cache MISS após bumpar cookerVersion (simulado via opção);
15. clean build (forceCook) — tudo recosido;
16. e2e EXPORT: projeto com cena+script+asset de cada tipo → build →
    export android-arm64 E linux-dev → arquivos existem → bundle decoda
    → manifest bate → contentHash confere → scripts validados;
17. determinismo byte-a-byte: 2 builds limpos = bundles IDÊNTICOS;
18. cenário com referência a AssetId (JSON com uuid registrado) —
    vira aresta (asset referenciado não aparece em unused).
