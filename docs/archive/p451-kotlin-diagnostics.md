# P4.5.1 — Rede de diagnóstico Kotlin (handler de exceções + micro-marks)

> Contexto: o APK P4.5 "Curved Dark" (`0b5bcde`) estreou no Realme C33 com
> **5 mortes de startup silenciosas**. O handler existente (P3.1) apanha
> apenas **sinais nativos** — exceções Kotlin morriam SEM nenhuma evidência
> (sem `goni_crash.log`, sem tombstone nomeando a linha). P4.5.1 fecha a
> lacuna SEM tocar em comportamento (contrato §3 do P4.5 intacto).
>
> Verificação: **VERIFIED (código)** = inspeção/compilação local confirma.
> **VERIFIED (CI)** = CI Linux (ASan+UBSan, Werror) + CI Android verdes no
> commit. **VERIFIED (APK)** = auditado o artefato real do CI (o mesmo
> binário instalado no device). **NOT VERIFIED (device)** = exige a próxima
> execução no C33.

## 0. A evidência que motivou (forense do device)

| Sessões | Último mark | Janela da morte (mapeada no código) |
|---|---|---|
| 4 (pid 24087…24618) | `STARTUP_EDITOR_DOCUMENT ok` | DENTRO de `buildUi()` — build da UI P4.5 (OniUi/sheets/chips/1º frame): **zero marks** entre `EDITOR_DOCUMENT` e `STARTUP_EDITOR_UI` |
| 1 (pid 24646) | `STARTUP_ACTIVITY ok` | Entre `STARTUP_ACTIVITY ok` e `EDITOR_HOST begin`: `maybeOfferCrashExport()` → **`OniDialog.custom()` (código P4.5, roda SÓ quando há crash report anterior)** + `mkdirs` + entrada JNI (`registerBackendFactories` antes do begin) |

O nativo é inocente em todas: `libgoni.so` carrega, JNI ok, filesystem ok,
documento ok (`STARTUP_EDITOR_DOCUMENT ok` emitido pelo C++). A morte vive
na camada Kotlin reescrita pelo P4.5 — e era invisível porque o crash
handler atual só apanha sinais, não exceções.

## 1. KotlinCrashGuard (o handler que faltava)

**Arquivo novo:** `android/app/src/main/java/com/goni/runtime/KotlinCrashGuard.kt`

| Propriedade | Decisão |
|---|---|
| Instalação | `Thread.setDefaultUncaughtExceptionHandler` no onCreate, ANTES de qualquer código que possa lançar (logo após `DiagnosticsMirror.init`) |
| Idempotência | UMA vez por processo — Activity recriada NÃO re-encadeia (cadeia guard→prev preservada) |
| Evidência | `filesDir/goni_crash.log` (append) com a MESMA assinatura `[crash]` do handler nativo → o pipeline existente trata crash Kotlin igual ao nativo: `hasPreviousCrashReport` → espelho automático para `Download/GONI` na execução seguinte + crash-prompt |
| Formato | Linha 1: `[crash] kotlin \| phase=<FASE> \| thread=<nome> \| time=<epoch> \| <classe> \| <msg>` — segue o stack completo (a LINHA que o C33 nunca conseguiu mostrar) |
| Fase corrente | `@Volatile` atualizada pelos micro-marks R3 — o relatório nomeia a fase; o stack nomeia a linha |
| Espelho público | Best-effort BORNED: `DiagnosticsMirror.exportCrashLogIfPresent()` (fila assíncrona, prazo 2 s) — a cópia privada em filesDir já garante a evidência na execução seguinte de qualquer forma |
| Delegação | SEMPRE ao handler anterior — o comportamento de morte do sistema (diálogo do Android, ART, tombstone) é INTACTO; só ADICIONA forense antes |
| I/O | Java puro, válido aqui: exceção Kotlin corre no frame normal da JVM — NÃO é contexto de sinal (o handler nativo mantém suas regras async-signal-safe para sinais) |

**Escopo deliberado:** instalado apenas no `EditorActivity` (as 5 mortes são
todas do caminho do editor). O runtime-demo `GoniActivity` fica intocado
(contrato zero-mudança). LIMITATION: um crash futuro do runtime-demo
continua sem forense Kotlin — uma linha (`KotlinCrashGuard.install(this)`)
fecha isso quando houver evidência de necessidade.

## 2. R1 — `STARTUP_EDITOR_HOST` fecha com "ok"

`editor/src/EditorHost.cpp` (EditorHost::create): o begin era o ÚNICO mark
sem par "ok" — a janela entre o begin e o próximo mark do Kotlin nunca
fechava ("morreu a criar o host" vs "morreu depois do host" eram
indistinguíveis). Agora, após host + documento prontos:

```
STARTUP_EDITOR_HOST begin → STARTUP_FILESYSTEM ok
→ STARTUP_EDITOR_DOCUMENT ok → STARTUP_EDITOR_HOST ok ("host + documento prontos")
```

**Teste nativo novo** (`editor/tests/EditorTests.cpp`): "editor: P4.5.1 R1 —
STARTUP_EDITOR_HOST fecha com ok (par begin/ok)" — cria host em workspace
temporário, valida no DELTA do log: begin+ok presentes, ok DEPOIS do begin,
e ordem interna grep-ável (filesystem → documento → host fechado).

## 3. R2 — Auditoria do APK real (o binário que está no C33)

Artefato `goni-debug-apk` do CI Android @ `0b5bcde` (execução #56):

| Verificação | Resultado |
|---|---|
| SHA256 | `659166917a13be1e91763302f513042707515fa90f370c170d28f458e3650253` |
| Conteúdo | 42 entradas: 3 dex, `lib/{arm64-v8a,x86_64}/{libgoni.so,libc++_shared.so}`, res splash/launcher completos |
| `androidx` nos 3 dex (strings) | **ZERO ocorrências** — a dependency é intencionalmente vazia (build.gradle) |
| `androidx.core.splashscreen` nos dex | **ZERO** — a teoria `NoClassDefFoundError` de splashscreen está **REFUTADA**: as classes não existem no APK E nenhuma linha de código as referencia (se referenciassem, seria erro de COMPILO, não de runtime) |
| Splash | `windowBackground` = `@drawable/oni_splash` puro (layer-list: `@color/oni_bg` + logo + wordmark; 4 densidades de PNG presentes no APK) |
| Tema (API level do C33, Android 12) | `GoniTheme` usa só attrs `android:` de API 21+ (windowBackground/colorEdgeEffect/statusBarColor/navigationBarColor) — válidos para minSdk 24 |
| Android 12+ splash do sistema | Usa o ícone do LAUNCHER: `mipmap-anydpi-v26/ic_launcher.xml` → `@drawable/oni_launcher_mark.xml` (vector válido, refs `@color/oni_bg`/`#8AB4F8` presentes) — cadeia íntegra |

Consequência: a janela da morte #2 reduz-se a (a) `OniDialog.custom` do
crash-prompt, (b) `mkdirs` trivial, (c) entrada JNI antes do begin. Os
micro-marks R3 + o guard nomeiam exatamente qual.

## 4. R3 — Micro-marks que cobrem as duas janelas

Cada mark: persistido NA HORA (filesDir + espelho assíncrono) + logcat
`[GONI]`, e atualiza a fase do guard. Auditorias de tema/splash são
best-effort: falha = mark `failed` com a exceção no detalhe — NUNCA aborta
o startup (quem mataria de verdade é o framework; aí o guard pega).

| Mark | Onde | O que fecha |
|---|---|---|
| `UI_THEME` | onCreate, após `STARTUP_ACTIVITY ok` | Resolve o `windowBackground` do tema ativo (confirma = `oni_splash`) + ícone do launcher (usado pelo splash do sistema 12+) |
| `UI_SPLASH` | onCreate, após `UI_THEME` | Infla a cadeia completa do splash (layer-list + cores + PNGs por densidade) |
| — | onCreate, após `UI_SPLASH` | `maybeOfferCrashExport()` (OniDialog P4.5) roda DELIMITADO: morte nele = último mark `UI_SPLASH` + stack nomeando a linha |
| `UI_BUILD_START` | onCreate, antes de `buildUi()` | Abre a janela da morte #1 |
| `ONIUI_INIT` | buildUi, após header completo | Header card + chips + play (primeiro trecho P4.5: tokens/helpers Oni) |
| `UI_SHEETS` | buildUi, após `buildAssetsPanel()` | Sheets + scrim + adapters (a metade pesada do build) |
| `UI_FIRST_FRAME` | buildUi, após `setContentView` + insets + estados | UI anexada à janela, traversal agendado (o 1º frame em si já tem `FIRST_TRAVERSAL` no doFrame) |
| `STARTUP_EDITOR_HOST ok` (R1) | C++ (EditorHost::create) | Fecha a fase de criação do host |

Fases do guard (mais finas que marks, sem custo): `BOOTSTRAP → UI_THEME →
UI_SPLASH → CRASH_PROMPT → EDITOR_CREATE → UI_BUILD (ONIUI_INIT/UI_SHEETS/
UI_FIRST_FRAME) → STARTUP_EDITOR_UI → POST_PROJECT → UI_SYNC → UI_SCALE →
IDLE → RESUME`.

Sequência completa de startup após P4.5.1 (marks em ordem):

```
STARTUP_NATIVE_LIBRARY → STARTUP_JNI → STARTUP_APPLICATION → STARTUP_ACTIVITY
→ UI_THEME → UI_SPLASH → (crash-prompt opcional)
→ STARTUP_EDITOR_HOST begin → STARTUP_FILESYSTEM ok → STARTUP_EDITOR_DOCUMENT ok
→ STARTUP_EDITOR_HOST ok (R1 — NOVO)
→ UI_BUILD_START → ONIUI_INIT → UI_SHEETS → UI_FIRST_FRAME (NOVOS R3)
→ STARTUP_EDITOR_UI → STARTUP_POST_PROJECT → STARTUP_UI_SYNC
→ MIRROR_ENQUEUE → RESUME_RETURN → FIRST_TRAVERSAL → …
```

## 5. Como ler a PRÓXIMA morte (protocolo)

1. **`goni_crash.log` em `Download/GONI/`** (agora também para exceções
   Kotlin): linha `[crash] kotlin | phase=X` nomeia a FASE; o stack anexo
   nomeia a CLASSE/ARQUIVO/LINHA.
2. **`goni_startup.log`**: o último mark `UI_*`/`STARTUP_*` contextualiza a
   fase — sem depender de ADB (o C33 não tem).
3. Se a linha for `[crash] <sinal nativo>` (ex.: SIGSEGV), é o handler P3.1
   de sempre (formato [pc]/[fp]/maps — ver p33-symbolization.md).
4. Execução seguinte: o espelho automático (P3.2) publica o crash ANTES de
   qualquer carga; o crash-prompt oferece o zip completo.

## 6. Verificação

| Item | Estado | Evidência |
|---|---|---|
| R1: par begin/ok do EDITOR_HOST | VERIFIED (código + teste) | Teste nativo novo verde local (debug ASan e release); ordem filesystem→documento→host ok validada |
| R3: 6 micro-marks + fases | VERIFIED (código) | `EditorActivity.kt` — marks persistidos na hora via `nativeStartupMark` (mesma via P3.1) |
| Guard: forense Kotlin em `goni_crash.log` | VERIFIED (código) | Assinatura `[crash]` integra com `hasPreviousCrashReport`/espelho/crash-prompt existentes |
| Guard: não re-encadeia em Activity recriada | VERIFIED (código) | Flag `installed` idempotente |
| R2: splashscreen NoClassDef refutado | VERIFIED (APK) | 0 ocorrências de `androidx`/`splashscreen` nos 3 dex do artefato @0b5bcde (SHA256 no §3) |
| R2: tema/splash válidos p/ API do C33 | VERIFIED (código) | Atributos API 21+; cadeia de recursos completa no APK (§3) |
| Suíte de editor existente não regrediu | VERIFIED (local) | 167 casos: 155 passaram, 12 SKIP (sem GPU no container); únicos FAILs = 2 testes de sinal que falham idênticos no HEAD limpo neste container (kernel/ASan) e são verdes no CI |
| CI Linux (ASan+UBSan+Werror) + CI Android | VERIFIED (CI) | #63 / #57 verdes @ `f262661` — suíte completa com o teste R1 novo |
| APK P4.5.1 + SHA256 | VERIFIED (CI) | Artefato da execução #57: SHA256 `c16d99d795e0ccfac152043cb19c4f8fb390406d31d227bf446e86de0ca168a5` — os 6 marks R3 presentes nos dex (strings) e `host + documento prontos`/`STARTUP_EDITOR_HOST` no `libgoni.so` arm64 |
| Morte real nomeada no device | NOT VERIFIED (device) | Exige a próxima morte (esperada: nenhuma, OU agora com forense completo) |
| Runtime-demo (GoniActivity) com guard | LIMITATION | Fora do escopo P4.5.1 (5 mortes são todas do editor); uma linha fecha quando necessário |
| Comportamento em caminho saudável | PRESERVADO | Zero mudanças em lógica de UI/JNI/gameplay; marks são writes log-only; auditorias de tema são read-only |


---

## 7. P4.5.2 — Primeira captura do guard e o binding JNI que faltava

### 7.1 A rede funcionou: o guard nomeou a morte

O P4.5.1 fechou no commit `f262661` e o APK com o guard foi instalado no C33.
Na primeira morte depois disso, o `KotlinCrashGuard` capturou o que o
handler antigo (native-only) deixava passar: o crash tinha **stack trace
completo em Kotlin**, direto do `goni_crash.log`:

```
UnsatisfiedLinkError: nativeEditorGetSnapTranslate(long)
  at EditorActivity.syncSnapChips:720
  at EditorActivity.buildUi:678
  at EditorActivity.onCreate:236
```

A teoria do splash (R2, já refutada no §3) morreu de vez: o processo
chegou ao `buildUi()` — depois morreu na PRIMEIRA chamada às funções
novas de snap do P4.5. `UnsatisfiedLinkError` = o símbolo JNI não existe
no `libgoni.so` carregado pelo ART.

### 7.2 Causa raiz (provada no APK real)

`EditorJni.cpp` fecha o bloco `extern "C"` na linha 2450 (fecho herdado do
P4.3, commit `9a144e4a`), e o commit P4.5 `3b483c5` anexou as 8 funções
snap/fit/undo/redo **DEPOIS** do fecho. Com `JNIEXPORT` mas sem
`extern "C"`, o compilador C++ **mangla** os nomes — `dlsym` do ART
procura `Java_com_goni_runtime_EditorJni_nativeEditorGetSnapTranslate`
e encontra `_Z60Java_com_goni_…GetSnapTranslateP7_JNIEnvP8_jobjectl`.
Sintaxe correta → CI Android verde desde sempre; símbolo errado → crash
no device. O gate antigo (`llvm-nm -D | grep Java_com_goni`) era cego a
isto: o símbolo manglado CONTÉM a substring greppada.

Evidência no APK P4.5.1 real (artefato CI #57, o mesmo do C33) —
`llvm-readelf --dyn-syms lib/arm64-v8a/libgoni.so`:

```
_Z51Java_com_goni_runtime_EditorJni_nativeEditorSetSnapP7_JNIEnvP8_jobjectlhh
_Z60Java_com_goni_runtime_EditorJni_nativeEditorGetSnapTranslateP7_JNIEnvP8_jobjectl
_Z57Java_com_goni_runtime_EditorJni_nativeEditorGetSnapRotateP7_JNIEnvP8_jobjectl
_Z55Java_com_goni_runtime_EditorJni_nativeEditorViewportFitP7_JNIEnvP8_jobjectl
_Z51Java_com_goni_runtime_EditorJni_nativeEditorCanUndoP7_JNIEnvP8_jobjectl
_Z51Java_com_goni_runtime_EditorJni_nativeEditorCanRedoP7_JNIEnvP8_jobjectl
_Z48Java_com_goni_runtime_EditorJni_nativeEditorUndoP7_JNIEnvP8_jobjectl
_Z48Java_com_goni_runtime_EditorJni_nativeEditorRedoP7_JNIEnvP8_jobjectl
```

8 símbolos manglados (`_Z…`); os outros 112 (dentro de `extern "C"`) estavam limpos.
GoniJni.cpp audita na mesma sessão: 10/10 definições dentro do bloco — sem risco.

### 7.3 Correção

O fecho `}  // extern "C"` moveu-se para a ÚLTIMA linha do TU (depois de
`nativeEditorRedo`), com comentário de guarda no próprio código. Nenhuma
assinatura, nenhum corpo, nenhuma ordem de função alterada — só o escopo
de ligação. As 8 funções delegam em `EditorHost`/`EditorDocument` como
desenhado (sem stub — implementação real desde `3b483c5`).

### 7.4 Matriz de binding (Kotlin ⇄ cpp, 1:1)

Gerada por `scripts/jni_binding_matrix.py` (nomes + retorno + parâmetros
JNI por posição; nullability Kotlin não altera o tipo JNI). 120 funções:

| # | Kotlin | retorno | parâmetros | JNI cpp | estado |
|---|--------|---------|------------|---------|--------|
| 1 | `nativeEditorCreate` | `Long` | `backend: String, workspaceRoot: String` | `jlong` | ok |
| 2 | `nativeEditorDestroy` | `Unit` | `handle: Long` | `void` | ok |
| 3 | `nativeEditorSurfaceCreated` | `Unit` | `handle: Long, surface: Surface` | `void` | ok |
| 4 | `nativeEditorSurfaceChanged` | `Unit` | `handle: Long, width: Int, height: Int` | `void` | ok |
| 5 | `nativeEditorSurfaceDestroyed` | `Unit` | `handle: Long` | `void` | ok |
| 6 | `nativeEditorOnPause` | `Unit` | `handle: Long` | `void` | ok |
| 7 | `nativeEditorOnResume` | `Unit` | `handle: Long` | `void` | ok |
| 8 | `nativeEditorRenderFrame` | `Boolean` | `handle: Long, deltaSeconds: Float` | `jboolean` | ok |
| 9 | `nativeEditorSetBackend` | `Unit` | `handle: Long, backend: String` | `void` | ok |
| 10 | `nativeEditorNewProject` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 11 | `nativeEditorOpenProject` | `Boolean` | `handle: Long, relPath: String` | `jboolean` | ok |
| 12 | `nativeEditorSaveProject` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 13 | `nativeEditorProjectName` | `String?` | `handle: Long` | `jstring` | ok |
| 14 | `nativeEditorProjectFolder` | `String?` | `handle: Long` | `jstring` | ok |
| 15 | `nativeEditorExportProjectZip` | `Boolean` | `handle: Long, zipRelPath: String` | `jboolean` | ok |
| 16 | `nativeEditorImportProjectZip` | `String?` | `handle: Long, zipRelPath: String, preferredName: String` | `jstring` | ok |
| 17 | `nativeEditorHasProject` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 18 | `nativeEditorSetProjectName` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 19 | `nativeEditorEnsureProject` | `String?` | `handle: Long` | `jstring` | ok |
| 20 | `nativeEditorListProjects` | `String?` | `handle: Long` | `jstring` | ok |
| 21 | `nativeEditorDumpState` | `Unit` | `handle: Long, origin: String` | `void` | ok |
| 22 | `nativeStartupInit` | `Unit` | `dir: String` | `void` | ok |
| 23 | `nativeStartupMark` | `Unit` | `stage: String, status: String, detail: String?` | `void` | ok |
| 24 | `nativeStartupHasCrashReport` | `Boolean` | `—` | `jboolean` | ok |
| 25 | `nativeWatchdogArm` | `Unit` | `—` | `void` | ok |
| 26 | `nativeWatchdogHeartbeat` | `Unit` | `—` | `void` | ok |
| 27 | `nativeWatchdogEvaluate` | `Boolean` | `—` | `jboolean` | ok |
| 28 | `nativeEditorNewScene` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 29 | `nativeEditorSaveScene` | `Boolean` | `handle: Long, relPath: String` | `jboolean` | ok |
| 30 | `nativeEditorLoadScene` | `Boolean` | `handle: Long, relPath: String` | `jboolean` | ok |
| 31 | `nativeEditorSceneDirty` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 32 | `nativeEditorHierarchy` | `String?` | `handle: Long` | `jstring` | ok |
| 33 | `nativeEditorCreateEntity` | `Long` | `handle: Long, name: String, parentPacked: Long` | `jlong` | ok |
| 34 | `nativeEditorDeleteEntity` | `Boolean` | `handle: Long, packed: Long` | `jboolean` | ok |
| 35 | `nativeEditorRenameEntity` | `Boolean` | `handle: Long, packed: Long, name: String` | `jboolean` | ok |
| 36 | `nativeEditorDuplicateEntity` | `Long` | `handle: Long, packed: Long` | `jlong` | ok |
| 37 | `nativeEditorReparentEntity` | `Boolean` | `handle: Long, packed: Long, parentPacked: Long` | `jboolean` | ok |
| 38 | `nativeEditorGetTransform` | `FloatArray?` | `handle: Long, packed: Long` | `jfloatArray` | ok |
| 39 | `nativeEditorSetTransform` | `Boolean` | `handle: Long, packed: Long, px: Float, py: Float, pz: Float, rx: Float, ry: Float, rz: Float, sx: Float, sy: Float, sz: Float` | `jboolean` | ok |
| 40 | `nativeEditorSelection` | `Long` | `handle: Long` | `jlong` | ok |
| 41 | `nativeEditorSelect` | `Boolean` | `handle: Long, packed: Long` | `jboolean` | ok |
| 42 | `nativeEditorSelectionRevision` | `Long` | `handle: Long` | `jlong` | ok |
| 43 | `nativeEditorSetTool` | `Unit` | `handle: Long, tool: Int` | `void` | ok |
| 44 | `nativeEditorGetTool` | `Int` | `handle: Long` | `jint` | ok |
| 45 | `nativeEditorSetUiScale` | `Unit` | `handle: Long, scale: Float` | `void` | ok |
| 46 | `nativeEditorGizmoDragBegin` | `Int` | `handle: Long, x: Float, y: Float` | `jint` | ok |
| 47 | `nativeEditorGizmoDragTo` | `Boolean` | `handle: Long, x: Float, y: Float` | `jboolean` | ok |
| 48 | `nativeEditorGizmoDragEnd` | `Unit` | `handle: Long` | `void` | ok |
| 49 | `nativeEditorCreateSprite` | `Long` | `handle: Long, name: String` | `jlong` | ok |
| 50 | `nativeEditorComponentCatalog` | `String?` | `handle: Long` | `jstring` | ok |
| 51 | `nativeEditorEntityComponents` | `String?` | `handle: Long, packed: Long` | `jstring` | ok |
| 52 | `nativeEditorComponentFields` | `String?` | `handle: Long, packed: Long, component: String` | `jstring` | ok |
| 53 | `nativeEditorSetComponentField` | `Boolean` | `handle: Long, packed: Long, component: String, fieldPath: String, value: String` | `jboolean` | ok |
| 54 | `nativeEditorAddComponent` | `Boolean` | `handle: Long, packed: Long, component: String` | `jboolean` | ok |
| 55 | `nativeEditorRemoveComponent` | `Boolean` | `handle: Long, packed: Long, component: String` | `jboolean` | ok |
| 56 | `nativeEditorViewportTap` | `Long` | `handle: Long, x: Float, y: Float` | `jlong` | ok |
| 57 | `nativeEditorViewportPan` | `Unit` | `handle: Long, dx: Float, dy: Float` | `void` | ok |
| 58 | `nativeEditorViewportZoom` | `Unit` | `handle: Long, factor: Float, focusX: Float, focusY: Float` | `void` | ok |
| 59 | `nativeEditorMoveEntity` | `Boolean` | `handle: Long, packed: Long, dx: Float, dy: Float` | `jboolean` | ok |
| 60 | `nativeEditorPlay` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 61 | `nativeEditorStop` | `Unit` | `handle: Long` | `void` | ok |
| 62 | `nativeEditorIsPlaying` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 63 | `nativeEditorSetPaused` | `Unit` | `handle: Long, paused: Boolean` | `void` | ok |
| 64 | `nativeEditorIsPaused` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 65 | `nativeEditorAssetCategories` | `String?` | `handle: Long` | `jstring` | ok |
| 66 | `nativeEditorAssetList` | `String?` | `handle: Long, category: String` | `jstring` | ok |
| 67 | `nativeEditorAssetImport` | `Boolean` | `handle: Long, tempRelPath: String, category: String, name: String` | `jboolean` | ok |
| 68 | `nativeEditorAssetRename` | `Boolean` | `handle: Long, category: String, name: String, newName: String` | `jboolean` | ok |
| 69 | `nativeEditorAssetDelete` | `Boolean` | `handle: Long, category: String, name: String` | `jboolean` | ok |
| 70 | `nativeEditorAssetMove` | `Boolean` | `handle: Long, fromCategory: String, name: String, toCategory: String` | `jboolean` | ok |
| 71 | `nativeEditorScriptStats` | `String?` | `handle: Long` | `jstring` | ok |
| 72 | `nativeEditorAudioStatus` | `String?` | `handle: Long` | `jstring` | ok |
| 73 | `nativeEditorAssetImageInfo` | `String?` | `handle: Long, category: String, name: String` | `jstring` | ok |
| 74 | `nativeEditorListTextures` | `String?` | `handle: Long` | `jstring` | ok |
| 75 | `nativeEditorScriptList` | `String?` | `handle: Long` | `jstring` | ok |
| 76 | `nativeEditorScriptRead` | `String?` | `handle: Long, name: String` | `jstring` | ok |
| 77 | `nativeEditorScriptWrite` | `Boolean` | `handle: Long, name: String, content: String` | `jboolean` | ok |
| 78 | `nativeEditorScriptCreate` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 79 | `nativeEditorScriptDelete` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 80 | `nativeEditorScriptCompile` | `String?` | `handle: Long, source: String` | `jstring` | ok |
| 81 | `nativeEditorScriptAssign` | `Boolean` | `handle: Long, packed: Long, name: String` | `jboolean` | ok |
| 82 | `nativeEditorGameTouch` | `Unit` | `handle: Long, phase: Int, pointerId: Int, x: Float, y: Float, pressure: Float` | `void` | ok |
| 83 | `nativeEditorSetGameViewportSize` | `Unit` | `handle: Long, width: Int, height: Int` | `void` | ok |
| 84 | `nativeEditorAddableComponents` | `String?` | `handle: Long, packed: Long` | `jstring` | ok |
| 85 | `nativeEditorAnimationList` | `String?` | `handle: Long` | `jstring` | ok |
| 86 | `nativeEditorAnimationRead` | `String?` | `handle: Long, name: String` | `jstring` | ok |
| 87 | `nativeEditorAnimationWrite` | `Boolean` | `handle: Long, name: String, json: String` | `jboolean` | ok |
| 88 | `nativeEditorAnimationCreate` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 89 | `nativeEditorAnimationDelete` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 90 | `nativeEditorAnimationAssign` | `Boolean` | `handle: Long, packed: Long, name: String` | `jboolean` | ok |
| 91 | `nativeEditorAnimationAddFrame` | `Float` | `handle: Long, name: String, texture: String` | `jfloat` | ok |
| 92 | `nativeEditorAnimationSetMeta` | `Boolean` | `handle: Long, name: String, loop: Boolean, fps: Float` | `jboolean` | ok |
| 93 | `nativeEditorPreviewStart` | `Boolean` | `handle: Long, packed: Long, clip: String` | `jboolean` | ok |
| 94 | `nativeEditorPreviewStop` | `Unit` | `handle: Long` | `void` | ok |
| 95 | `nativeEditorPreviewing` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 96 | `nativeEditorAudioPreview` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 97 | `nativeEditorAudioPreviewStop` | `Unit` | `handle: Long` | `void` | ok |
| 98 | `nativeEditorAudioPreviewPlaying` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 99 | `nativeEditorLayerList` | `String?` | `handle: Long` | `jstring` | ok |
| 100 | `nativeEditorLayerAdd` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 101 | `nativeEditorLayerSetTimeScale` | `Boolean` | `handle: Long, name: String, ts: Float` | `jboolean` | ok |
| 102 | `nativeEditorLayerSetParticipation` | `Boolean` | `handle: Long, name: String, update: Boolean, physics: Boolean, render: Boolean` | `jboolean` | ok |
| 103 | `nativeEditorPhysicsDt` | `Float` | `handle: Long` | `jfloat` | ok |
| 104 | `nativeEditorPhysicsSetDt` | `Boolean` | `handle: Long, dt: Float` | `jboolean` | ok |
| 105 | `nativeEditorListAudio` | `String?` | `handle: Long` | `jstring` | ok |
| 106 | `nativeEditorMaterialList` | `String?` | `handle: Long` | `jstring` | ok |
| 107 | `nativeEditorMaterialRead` | `String?` | `handle: Long, name: String` | `jstring` | ok |
| 108 | `nativeEditorMaterialWrite` | `Boolean` | `handle: Long, name: String, json: String` | `jboolean` | ok |
| 109 | `nativeEditorMaterialCreate` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 110 | `nativeEditorMaterialDelete` | `Boolean` | `handle: Long, name: String` | `jboolean` | ok |
| 111 | `nativeEditorListMaterials` | `String?` | `handle: Long` | `jstring` | ok |
| 112 | `nativeEditorLastError` | `String?` | `handle: Long` | `jstring` | ok |
| 113 | `nativeEditorSetSnap` | `Unit` | `handle: Long, translate: Boolean, rotate: Boolean` | `void` | ok |
| 114 | `nativeEditorGetSnapTranslate` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 115 | `nativeEditorGetSnapRotate` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 116 | `nativeEditorViewportFit` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 117 | `nativeEditorCanUndo` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 118 | `nativeEditorCanRedo` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 119 | `nativeEditorUndo` | `Boolean` | `handle: Long` | `jboolean` | ok |
| 120 | `nativeEditorRedo` | `Boolean` | `handle: Long` | `jboolean` | ok |

Resultado: **120 external fun no Kotlin ⇄ 120 definições JNI no cpp —
0 faltantes, 0 extras, 0 divergências de assinatura.**

### 7.5 Contrato travado por teste — dlsym no TU de teste

- `EditorJni.cpp` compila AGORA também no Linux, dentro de
  `eng_editor_tests`, contra um **shim hermético** (`editor/tests/jni_shim/` —
  jni.h + ANativeWindow mínimos, SEM JDK; as funções nunca são chamadas).
- `eng_editor_tests` linka com `ENABLE_EXPORTS` (+rdynamic) e o novo caso
  **"JNI symbol contract"** faz `dlsym(RTLD_DEFAULT, …)` dos 120 símbolos
  esperados (a lista do §7.4, literal em `EditorTests.cpp`).
- **Prova do laço (proof-of-catch)**: com o bug reintroduzido
  (git stash do fix), o teste falha e nomeia EXATAMENTE as 8 funções
  P4.5 em falta — `Símbolos JNI ausentes/manglados (8/120)`. Com o fix,
  passa. A regressão do C33 não recompila.

### 7.6 Gate novo no CI Android

O passo "Inspeção do APK" ganhou um guard que **falha o CI** se existir
QUALQUER símbolo `_Z[0-9]*Java_com_goni` no `libgoni.so` do artefato
(mangling = `Java_com_*` fora de `extern "C"`). O grep simples antigo
continua como evidência; o guard novo fecha a sua cegueira.

### 7.7 Verificação local ANTES do push (Tarefa 4 da missão)

Cross-build do `.so` com NDK r27b (mesma toolchain do CI),
`-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
-DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release` (flags do gradle):

| Check | Esperado | Obtido |
|---|---|---|
| Símbolos `_Z…Java_com_goni` mangled | 0 | **0** |
| Símbolos limpos `Java_com_goni_runtime_EditorJni` | 120 | **120** |
| As 8 funções P4.5 (limpas, `nm -D`) | presentes | **8/8** |
| Contrato completo (lista §7.4 vs `nm -D`) | 120/120 | **120/120** |

### 7.8 Estado por função

| Função | Estado | Evidência |
|---|---|---|
| `nativeEditorSetSnap` | VERIFIED | símbolo limpo no .so local (§7.7) + dlsym CI; implementação real em `EditorDocument` (sem stub) |
| `nativeEditorGetSnapTranslate` | VERIFIED | idem — era a função do crash (`syncSnapChips:720`) |
| `nativeEditorGetSnapRotate` | VERIFIED | idem |
| `nativeEditorViewportFit` | VERIFIED | idem — delega em `viewportFit(&textureCache())` |
| `nativeEditorCanUndo` / `CanRedo` | VERIFIED | idem — delegam em `canUndo()/canRedo()` |
| `nativeEditorUndo` / `Redo` | VERIFIED | idem — delegam em `undo()/redo()` (result.ok()) |
| Outras 112 funções EditorJni | VERIFIED | matriz 1:1 (§7.4) + dlsym 120/120 + CI Linux |
| Contrato dlsym no CI Linux | VERIFIED (CI #64 @ `dca9922`) | "JNI symbol contract" verde na suíte ASan+UBSan+Werror |
| APK do CI re-auditado (anti-mangling) | VERIFIED (CI Android #58 @ `dca9922`) | Gate "OK: zero símbolos JNI manglados" no log; APK artefato SHA256 `f0ae5a5de2d0f9dd9a81a8c52ba6a6dd7ec4ea7694794af7f6fbc40975358ba5`; re-auditoria local do `.so` do artefato: 0 mangled / 120 limpos / 8 P4.5 `T` |
| Instalação no C33 + round 5b | NOT VERIFIED (device) | requer usuário: APK abre e fica aberta; 10 pontos do round 5 |

### 7.9 Regras preservadas

Zero função stub (as 8 já delegavam em código real — o bug era só
ligação); zero mudanças de comportamento além do escopo de ligação;
P4.5/P4.5.1 intocados (marks, guard, docs); gizmo/cores/contratos JNI
existentes sem alteração; `GoniJni.cpp` auditado sem risco.
