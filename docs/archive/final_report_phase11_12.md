# Relatório final de evidências — FASES 11 (NI-Script) e 12 (Build & Export)

> Sessão de 2026-09-18. Este relatório usa APENAS estados verificados;
> nada é inferido. Classificação de estados: IMPLEMENTED / BUILT / UNIT
> TESTED / INTEGRATION TESTED / CI VERIFIED / APK PACKAGED / APK
> INSTALLED / DEVICE TESTED / REAL DEVICE FRAME VERIFIED.

## 1. Estado do repositório

| Item | Valor |
|---|---|
| Repositório | https://github.com/criandojogodenovo-sketch/g.oni |
| Branch | `main` (única) |
| HEAD (local == remoto) | `ca07ef82291cd7b0010de353938096ce358179ec` |
| Working tree | limpa (0 alterações pendentes) |
| Histórico | preservado — nenhum rebase/amend/force-push nesta sessão |

Commits desta sessão (base `8be0be8`, Stage A/B da sessão anterior):

| Commit | Conteúdo |
|---|---|
| `524a3d8` | feat(fase11): NI-Script — linguagem, VM, bindings, editor, docs, testes |
| `889bf35` | chore(fase11): remove binários de dev acidentalmente commitados |
| `016dd38` | fix(niscript): from_chars\<double\> inexistente em libc++/NDK → strtod |
| `87c5393` | feat(fase12): Build & Export Pipeline (dados, GONI, cache, export) |
| `ca07ef8` | fix(fase12): .gitignore `build/` engolia `engine/build/` (módulo sumia no CI) |

CI (GitHub Actions) no HEAD `ca07ef8`: **CI Linux = success** ·
**CI Android = success** (assembleDebug arm64-v8a — APK BUILT+INSPECTED).
CI no `016dd38` (FASE 11 completa): Linux = success · Android = success.

## 2. Estado por fase

| Fase | Estado |
|---|---|
| 1–10 | concluídas (sessões anteriores; auditoria/remediação em `docs/final_phase4_10_audit.md`) |
| **11 NI-Script** | **IMPLEMENTED · BUILT · UNIT TESTED · INTEGRATION TESTED · CI VERIFIED (Linux + Android) · APK PACKAGED (CI)** |
| **12 Build & Export** | **IMPLEMENTED · BUILT · UNIT TESTED · INTEGRATION TESTED · CI VERIFIED (Linux + Android)** |
| 13+ | apenas PLANEJADO (roadmap) — nada implementado, conforme instrução |

Gates locais da sessão: linux-debug **28/28 suites** (ASan+UBSan+LSan,
`-Werror`, zero warnings) e linux-release **28/28 suites** (LTO, zero
warnings); android_runtime SKIPa localmente sem driver — o CI executa.

## 3. FASE 11 — o que existe (e como é verificado)

- **Linguagem própria** `.nis` (ADR-049): Lexer → Parser → Sema →
  Compiler → bytecode → NI VM. Sem Lua, sem Python, sem browser, sem JIT.
- **Semântica formal ANTES da implementação** (`phase11_audit/design.md
  §5`) para `repeat` ([0,65536], var reinicia por iteração), `repair`
  (captura de faults; não-transacional; atomicidade por instrução;
  contador de orçamento nunca restaurado) e `timeout` (orçamento de
  INSTRUÇÕES, não relógio — sem threads/alocação por frame).
- **VM determinística**: orçamento global 1M instruções/evento (loop
  infinito impossível), faults `NiFault{kind,message,line,col}`,
  isolamento por evento, hooks de trace.
- **Segurança**: nativos fechados em compile-time (+conferência de
  `nativeCount` a cada execução), handles geracionais (EntityStale =
  Fault, nunca UB), SEM nil na linguagem, sem FS/shell/eval.
- **Bindings ECS**: tabela registrada pelo CONSUMIDOR (padrão ADR-043)
  reusando catálogo+reflexão por offset (mecanismo do Inspector); no
  editor: `position`/`scale`/`transform`/`name` refletidos + `rotation`
  custom (euler↔quat em GRAUS) + catálogo inteiro (alias canônico e
  curto).
- **Integração editor**: `NiScriptComponent` no catálogo ÚNICO; PLAY
  compila scripts do CLONE (ADR-044) e roda `@init`→`up start`→
  `up update`(por tick)→`up destroy`; script quebrado é desabilitado
  com log; edição intacta.
- **Testes**: niscript 58 casos/516 asserções (inclui E2E
  `.nis→compile→bytecode→VM→binding→MUDANÇA no componente ECS`,
  determinismo byte-a-byte, segurança); editor 35 casos/354 asserções
  (inclui 2 de play com scripts).

## 4. FASE 12 — o que existe

- **Pipeline de DADOS** (`engine/build`, ADR-050): as 10 etapas —
  loadProject → loadBuildConfig (paths RELATIVOS, absoluto=erro) →
  manifest determinístico (FNV-1a 64) → depGraph/scan (AssetId em
  texto, agnóstico) → validação bloqueante → cook em envelope GONI →
  cache conteúdo-endereçado → bundle → verify → export.
- **Validação bloqueante**: fonte ausente, ids duplicados, script que
  não compila (embutido OU standalone), cena JSON inválida,
  target/manifest/formatVersion inválidos. Não-usado = WARN no report.
- **Cook SOURCE/DERIVED**: classificação é campo do formato (v1 tudo
  SOURCE/verbatim); scripts validados por compilação e empacotados como
  FONTE (ADR-049: uma única fonte de verdade).
- **Export**: android-arm64 (prioritário; INSTALL.md documenta caminho
  no APK existente) + linux-dev (README). HONESTO: o LOADER do bundle na
  Activity do runtime é FUTURO declarado.
- **Testes**: build 23 casos/232 asserções — toda a lista obrigatória
  da missão + determinismo byte-a-byte + E2E export com verificação
  independente + bundle corrompido falha no verify.

## 5. Bugs reais corrigidos NA SESSÃO (todos com teste de regressão)

1. Lexer: ordem de avaliação de argumentos — `push(keywordOf(text), …,
   std::move(text))` no GCC movia a string ANTES do lookup (todas as
   keywords viravam Ident). Corrigido com avaliação prévia.
2. Parser: posse do AST — `ParseResult` guardava ponteiros crus para
   nós destruídos com o Parser (use-after-free detectado pelo ASan).
   Arena adicionada ao resultado.
3. VM: profundidade de emissão contava CHAMADAS de doEmit, não frames
   entregues — reemissão infinita possible até 256 frames. Movida para
   o frame (`fromEmit`).
4. VM: fault capturado por `repair` não era registrado em `lastFault`
   (2 caminhos — dispatch e deadline). Registrado (design §5.3.2d).
5. VM/Compiler: `DYN_SET` escrevia a base acumulada de volta no slot
   raiz (clobbering de compview/entidade). Opcodes `TO_ENTITY` + POP
   por tipo de raiz.
6. Lexer (portabilidade): `std::from_chars<double>` não existe em
   libc++/NDK (existe no libstdc++/GCC) — CI Android falhou; trocado
   por `strtod` (motor nunca chama `setlocale`).
7. Build (meta): `.gitignore` com `build/` sem âncora engolia o
   MÓDULO-FONTE `engine/build/` — checkout do CI não tinha o diretório.
   Padrões ancorados na raiz.

## 6. Documentação entregue

- `phase11_audit/design.md` (semântica formal PRÉ-implementação) e
  `phase12_audit/design.md` (pipeline formal);
- `docs/ni-script/01…08` (overview/syntax/types/control-flow/bytecode/
  vm/bindings/tools) — oito arquivos, conforme exigido;
- `docs/architecture/19-ni-script.md` e `20-build-export.md`;
- `docs/adr/ADR-049-ni-script.md` e `ADR-050-build-export.md`;
- README/roadmap/build.md/00-overview sincronizados — roadmap **12/12
  CONCLUÍDAS**, pós-12 apenas PLANEJADO.

## 7. Limitações conhecidas e adiamentos (declarados, não ocultos)

- **UI de script no editor**: ADIADA — fonte editável via Inspector
  (campo string/JNI); realce/numeração são futuros.
- **Loader de bundles no runtime Android** (browsing de projetos na
  Activity): FUTURO — o formato/export/validação existem e são testados.
- **Serialização de bytecode NI** (cache de compilação): FUTURO (ADR-049).
- **Cooks DERIVED** (texturas/malhas) e loaders das categorias
  reservadas: FUTURO (AssetType 100+ reservado desde a FASE 3).
- Breakpoints UI, inspetor de variáveis do VM, profiler: NÃO existem
  (docs/ni-script/08 declara o estado real das ferramentas).
- `repair` não é transacional; colunas de diagnóstico contam BYTES
  UTF-8; campos de componente com nome de keyword não são acessíveis
  pela sintaxe de ponto (documentados).

## 8. Teste em dispositivo físico

**Realme C33: NÃO TESTADO nesta sessão** — nenhum dispositivo físico
estava disponível no ambiente de execução. Nenhuma afirmação de
DEVICE TESTED é feita. APK arm64-v8a: PACKAGED (montado e inspecionado
pelo CI Android); INSTALLED em dispositivo: NÃO.

## 9. Resposta às perguntas do gate (YES/NO/NOT APPLICABLE)

| Pergunta | Resposta |
|---|---|
| Pipeline NI-Script `.nis→bytecode→VM` implementado sem Lua/Python/browser? | **YES** |
| Semântica de `repeat/repair/timeout` definida formalmente ANTES? | **YES** (`phase11_audit/design.md §5`) |
| VM determinística com orçamento (sem threads, sem aloc./frame)? | **YES** |
| Bindings reusam reflexão/catálogo existentes (ADR-043/D2)? | **YES** |
| ECS com segurança de geração respeitada? | **YES** (EntityStale = Fault) |
| ≥1 teste E2E `.nis→compile→bytecode→VM→binding→ECS`? | **YES** (niscript E2E + play do editor) |
| Editor PLAY integra scripts (clone, start/update/destroy)? | **YES** (2 casos de teste) |
| Linux Debug E Release verdes, Werror, zero warnings? | **YES** (28/28 ambos) |
| Todos os testes antigos ainda passam? | **YES** (28 suites) |
| Build Android + APK BUILT? | **YES** (CI Android assembleDebug arm64-v8a) |
| APK INSTALLED em dispositivo/emulador? | **NO** |
| Build pipeline com paths relativos? | **YES** (absoluto = erro) |
| Cook reusa envelope GONI com SOURCE/DERIVED? | **YES** |
| Cache determinístico (invalida por conteúdo/versão)? | **YES** (testado) |
| Testes: vazio/mínimo/ausente/não-usado/duplicado/script-inválido/manifest/target/cache/clean-incremental? | **YES** (23 casos) |
| ≥1 teste E2E de export? | **YES** (android-arm64 + linux-dev + verify) |
| Reproducibilidade do build? | **PARCIAL — determinismo byte-a-byte dos DADOS verificado; builds do MOTOR não são reproduzíveis bit-a-bit (fora do escopo da fase)** |
| Realme C33 testado fisicamente? | **NO** |
