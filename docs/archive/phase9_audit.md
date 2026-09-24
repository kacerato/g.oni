# Auditoria FASE 9 — Input + UI + Audio

- **Base:** `9a144e4` (HEAD FASE 8), tree limpa, 20/20 suites nos dois
  presets (baseline revalidada nesta sessão).
- **Escopo (missão §6):** sistemas fundamentais de gameplay interativo;
  a engine permanece DESACOPLADA das APIs Android (§6.1/§6.9).

## 1. O que existe e é reutilizado

| Sistema | Uso na FASE 9 |
|---|---|
| `eng::ecs`/`eng::scene` | sistemas de gameplay rodam sobre componentes |
| `eng::events` | eventos de UI → gameplay sem acoplamento (ADR-022) |
| `eng::serial::JsonValue` | configuração de bindings/UI/audio por asset JSON |
| `eng::assets` | `AssetType::Json` já tem loader — input.json/ui.json/audio.json são assets reais |
| FASE 7/8 hosts | padrão de JNI/lifecycle para o glue de input do jogo |
| `EditorDocument` (FASE 8) | PLAY/STOP já separa estados — runtime systems ligam no `tick()` |

## 2. Lacunas

- **G1 — Não existe `eng::input`.** Nenhum evento cruza a fronteira JNI
  hoje (GoniActivity não encaminha MotionEvents).
- **G2 — Não existe `eng::ui`.** Nenhum widget/layout/hit-test.
- **G3 — Não existe `eng::audio`.** Nada de WAV/vozes/mixer/backend.
- **G4 — A abstraction RHI não tem texturas.** Label com fonte bitmap
  texturizada é impossível no pipeline atual (pos+cor).
- **G5 — Configuração pelo editor (§8):** componentes ECS são structs
  refletidos FLAT (sem mapas/vetores — ADR-021 §B.1) — bindings de input
  por componente não cabem no serializer atual.

## 3. Decisões

- **D1 (G1):** `eng::input` com estado PURO: `TouchState` (pointers por ID
  com fase/posição/delta/pressão), `InputSystem` (fila de eventos canônicos
  + estado corrente + bindings de ações), teclado com `Key` CANÔNICO do
  engine (KeyCodes Android JAMAIS chegam ao gameplay — §6.1; conversão
  vive no TU JNI). Mouse/gamepad: `DeviceKind` + eventos definidos, coleta
  implementada para os que existem (touch/keyboard); gamepad declarado
  como extensão (§6.2 "quando disponíveis").
- **D2 (G1):** glue Android no runtime do JOGO (GoniActivity → JNI →
  `AndroidRuntime::onTouchEvent` → fila do InputSystem). Editor NÃO usa
  eng::input para seus gestos (§6.4 — já é assim na FASE 8); em Play, os
  toques do viewport vão ao input do RUNTIME (separação explícita por
  modo + ferramenta PAN/MOVER como override de debug).
- **D3 (G4):** texto da UI v1 por **fonte 5×7 pontilhada em quads
  coloridos** (pipeline pos+cor existente — sem texturas). Limitação
  honesta com gatilho de upgrade (texturas no RHI, fase futura);
  `Image` existe com layout/hit-test e conteúdo de cor sólida
  (content aguarda texturas — ADR-046).
- **D4 (G2):** `eng::ui` retained-mode puro: `UiDocument` (árvore de
  widgets Panel/Button/Label/Image/Slider/ProgressBar/Container),
  layout por retângulos + anchors fracionários + fator de escala
  (design-resolution → pixels), hit-test, eventos (pressed/released/
  click/valueChanged) via ponteiros-de-função sem captura (padrão do
  engine, sem std::function). Renderização: a UI produz uma **draw
  list** (quads de cor) e o host RHI desenha — eng::ui NÃO conhece RHI.
- **D5 (G3):** `eng::audio` com mixer por SOFTWARE no engine (vozes,
  gain por voz, loop, pausa) e backend abstraído `IAudioBackend`
  (pull: o backend pede buffers mixados). Backends: `NullAudioBackend`
  (Linux/testes) e `AAudioBackend` (Android, `dlopen("libaaudio.so")`
  em runtime — mesmo padrão dos backends gráficos, sem link edit).
  WAV: parser RIFF (PCM8/16/24/32-float) gerando buffers decodificados.
  `Music` = voz com leitura em CHUNKS do FileSystem (streaming real);
  `Sound` = buffer decodificado.
- **D6 (G5):** configuração de input/UI/audio por **ASSETS JSON**
  (input.json/ui.json/audio.json no projeto) — editados/importados pelo
  Asset Browser da FASE 8 e carregados pelas APIs C++ (loadFromJson —
  testado). Nada de UI-only: TODAS as APIs são C++ primeiro (§9).
- **D7:** threading inalterado (UI thread; mixer chamado no `tick` do
  runtime — callback AAudio puxa do buffer circular com mutex MÍNIMO
  documentado — única exceção à regra single-thread, padrão de áudio
  pull, testada por stress com TSan).

## 4. Riscos

| Risco | Mitigação |
|---|---|
| Thread do callback AAudio | buffer circular SPSC + prova TSan dedicada (Linux: mixer puxado por thread simulada) |
| Fonte pontilhada "feia" | documentada como v1 com caminho de upgrade; DPI ok (escala por quadrado) |
| Streaming WAV grande | chunks de 16 KB; teste com arquivo sintético de MBytes via MemoryFileSystem |
| Regressão no APK | assembleDebug + inspeção após a fase |
