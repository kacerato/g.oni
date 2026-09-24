> **CORREÇÃO (auditoria final 4–10):** NÃO existe buffer SPSC entre o
> callback AAudio e o mixer (o callback chama `mix()` sob o mutex único
> — o design original foi simplificado); `TouchPoint::delta`/`frameStamp`
> agora são implementados e testados (C-7/C-8/C-9); "streaming real"
> = janelas de 16k frames sobre o arquivo integralmente em memória (o
> decoder de janela é que é progressivo); deps de áudio: core/fs/serial
> (sem assets). Ver `docs/final_phase4_10_audit.md`.

# Design FASE 9 — Input + UI + Audio

- **Base:** `9a144e4` + `docs/phase9_audit.md` (decisões D1–D7).

## 1. Layout

```text
engine/input/    eng::input  — TouchState, InputSystem (fila canônica),
                             Key canônico, ActionBindings (to JSON asset)
engine/ui/       eng::ui     — UiDocument/widgets/layout/draw-list +
                             fonte 5×7 pontilhada (quads de cor)
engine/audio/    eng::audio  — Wav (RIFF), Sound/Music/Voice/AudioBus,
                             mixer software, IAudioBackend
                backends:    Null (testes) · AAudio (Android, dlopen)
```

Dependências: input→core/math/serial; ui→core/math/input(eventos de
focus)/serial; audio→core/fs/serial/assets(id). Nenhuma depende de
RHI/Android. O HOST desenha a draw-list da UI e alimenta/puxa o áudio.

## 2. eng::input

```text
enum class TouchPhase { Down, Move, Up, Cancelled };
struct TouchPoint { id, position, delta, pressure, phase, frameStamp }
class TouchState {  // estado corrente por pointer id
    onTouch(id, phase, x, y, pressure);  beginFrame();  // limpa delta/up
    active() (span), find(id), positionOf(id), isDown(id)
}
enum class Key { None, A..Z, Num0..9, Space, Enter, Back, Up/Down/L/R, VolUp... }
struct InputEvent { DeviceKind device; Key key; TouchPoint touch; }  // canônico
class InputSystem {
    queueEvent(...);            // JNI/AndroidRuntime enfileira
    update();                   // aplica fila → estados (uma vez por tick)
    const TouchState& touch(); bool keyDown(Key)/keyPressed(Key)...
    // Ações (§6.3): "jump" ← {Key::Space, Key::J, touch(esq-baixo)}
    void bindAction(name, ActionSource...);   // + loadFromJson(JsonValue)
    bool actionDown(name); actionPressed(name); actionReleased(name);
}
```
Gameplay consulta APENAS ações/estado — nunca códigos de dispositivo.

## 3. eng::ui

```text
struct UiRect { x,y,w,h (relativo ao pai) }  + Anchor (frações) + pivot
class UiDocument {
    Widget& root();  // Container
    Widget* hitTest(x,y);  void setDesignResolution(w,h);  setScale(dpi)
    buildDrawList(out)     // quads {rect absoluto, cor, nível}
}
Widgets: Container/Panel/Button/Label/Image/Slider/ProgressBar
  - Button: pressed/released/click (fn-ptr+ctx, sem captura — ADR-004)
  - Slider: value 0..1, drag → valueChanged
  - Label: text → quads da fonte 5×7 (kFont5x7, 95 glifos ASCII)
  - Image: cor sólida v1 (sem texturas no RHI — ADR-046)
Layout: measure/arrange minimal (quads empilhados no Container, gap);
  anchors fracionários relativos ao pai; escala design→pixels.
```

## 4. eng::audio

```text
struct WavData { sampleRate, channels, bits, interleaved PCM (f32 interno) }
class Wav { static parse(span<const byte>) → WavData }     // RIFF/PCM*
class AudioBus { gain; vozes }
class Voice { play/stop/pause/resume; volume; loop;  // SOLO estado+posição
              source: Sound (buffer) | Music (chunks do FileSystem) }
class AudioMixer {           // pull: mix(numFrames, out f32 interleaved)
      tick();                // consome loops/pausas; recarrega chunks
      playSound(SoundId, bus)/playMusic(path, bus) → VoiceHandle }
IAudioBackend { start(mixer)/stop() }  // puxa buffers mixados
NullAudioBackend (contadores p/ testes) · AAudioBackend (dlopen, stream
  callback → mixer.pull com buffer SPSC + mutex único — ADR-047)
```
Config: `audio.json` (buses/volumes), `input.json` (bindings) — assets.

## 5. Integração

- **Runtime do jogo (FASE 7):** `AndroidRuntime` ganha `InputSystem`
  (onTouchEvent via JNI) — demo continua triangle; input testado no
  Linux com fila sintética (o mesmo binário do APK).
- **Editor (§8):** em Play, toques do viewport alimentam o input do
  runtime (`nativeEditorGameTouch`); gestos de câmera exigem a
  ferramenta PAN/MOVER (separação §6.4). Config por assets no browser.
- **`tick()` do EditorDocument (FASE 8):** em Play, também avança o
  runtime (audio mixer/UI — componentes futuros consomem).

## 6. Testes (§6.11)

- Input: down/move/up; multitouch (3 dedos concorrentes); identidade por
  pointer id; delta por frame; pressão; beginFrame limpa; ações
  (touch+zona, teclas, combinação); loadFromJson de bindings; teclado
  pressed/released com janela de um update.
- UI: hierarquia (pais/filhos); layout com âncoras/escala; hit-test
  (top-most, janelas sobrepostas); Button press/click; Label → contagem
  de quads da fonte; Slider valueChanged; ProgressBar preenchimento;
  resoluções diferentes (design→pixels ida e volta).
- Audio: WAV PCM8/16/float32 parse (dados sintéticos com checksum);
  play/stop/pause/resume/volume/loop (mix de N frames verificado
  matematicamente); streaming de Music em chunks; cleanup de vozes
  (handles órfãos); bus gain; stress de pull concorrente (TSan);
  NullBackend contadores.
- Regressão: 20 suites anteriores + Android assembleDebug + inspeção.
