# P3.4 — Resolução definitiva do crash AAudio no Realme C33

Base: `5743466` (P3.3 — instrumentação de crash com `/proc/self/maps` +
backtrace). Commits: `d4bc2db` (correção + testes) + `645d95b` (link
eng::log — o TU Android inclui `eng/log/Macros.hpp`; no Linux o TU é
vazio e o CI Android foi quem apanhou) + fechamento (sha256 abaixo).

**APK final**: `goni-p3.4-final.apk` (CI Android de `645d95b`,
assembleDebug, NDK 27.0.12077973, arm64-v8a + x86_64)

```
sha256 a00c64d2cbb41a439f90f72785a033dbea1ee13effede5af9a17b1da58621790
7.059.372 bytes
```

---

## 1. Evidência e janela de morte (recapitulação P3.3 → P3.4)

- Última linha persistida do `goni_startup.log` no device:
  `STARTUP_AUDIO backend AAudio` — o processo morre DENTRO de
  `AAudioBackend::start()`, entre o marco `backend` e `started/failed`.
- O PC do crash não está na `libgoni.so` (análise exaustiva P3.3 — 45
  candidatos de store eliminados): está numa biblioteca de sistema
  chamada na janela de áudio, com `addr = pc + 0x3320` (escrita em
  página read-only da mesma lib — padrão SEGV_ACCERR).
- No emulador ATD o caminho nunca passa de `createStreamBuilder`
  (retorna null — fallback gracioso observado no P3.3). No hardware
  Unisoc T612 o HAL abre o stream de verdade — e é onde o processo
  morre.

A missão P3.4 listou 4 causas prováveis. Auditoria e correção de cada
uma abaixo.

## 2. Causas — auditoria e correção aplicada

### Causa 1 — Parâmetros do stream divergem do pedido (ALTA) → ADAPTADO

O P3.3 RECUSAVA divergências (o app seguia sem áudio). O P3.4 remove
a recusa e **adapta o mixer ao layout que o HAL abriu de verdade**
(nunca presumir que o builder foi respeitado):

| Divergência | Correção |
|---|---|
| Canais | `convertFrames()` mapeia mixerCh→streamCh: extras duplicam o último canal; downmix soma os restantes no último (2→1 = L+R, clampado) |
| Formato | `AAUDIO_FORMAT_PCM_FLOAT/I16/I32` convertidos na entrega; i24/i8/inválido recusados com erro preciso ANTES do start |
| Taxa | `LinearResampler` de fase exata (int64): posição da saída n = n·inRate/outRate sem drift; push/pull com histórico interno (razões densas precisam de lookback > 1 frame — bug de design real encontrado e corrigido DURANTE o desenvolvimento, com teste de regressão dedicado) |

O callback mistura no layout do MIXER (f32 intercalado) e a camada
`eng::audio` (`AudioAdapt.{hpp,cpp}`, NDK-free, testável no Linux)
converte para o layout EFETIVO. Zero alocação e zero lock no caminho
quente; scratch dimensionado no start a partir da capacidade real do
stream (`getBufferCapacityInFrames`), com chunking em lotes de ≤ 512
frames — qualquer `numFrames` do callback é servido sem overflow.

### Causa 2 — `requestStart` em stream inválido (ALTA) → VALIDADO

- `openStream` retornando `AAUDIO_OK` com stream **null**: recusado
  (era caminho direto para SIGSEGV dentro de `libaaudio.so`).
- `AAudioStream_getState` consultado: só `requestStart` a partir de
  `AAUDIO_STREAM_STATE_OPEN` — estado divergente recusa com o valor no
  erro. (Nota: a missão citava `AAUDIO_STATE_OPEN`; o identificador
  real do NDK é `AAUDIO_STREAM_STATE_OPEN`.)

### Causa 3 — Callback acessa objeto destruído (MÉDIA) → PROTOCOLO DE DRENAGEM

- O **global `gMixer` foi extinto** — o callback recebe `this` via
  `userData`; estado compartilhado é feito de átomos.
- `CallbackGate`: `tryEnter()` com dupla checagem (quem entra depois do
  `close` é recusado; quem entrou antes é visto pela drenagem),
  validado sob TSan (harness dedicado: 50/50 ciclos close→drain→reopen
  sem race e sem deadlock — ver `scripts/tsan_p34_harness.cpp`).
- Sequência de `stop()` (determinística):
  1. `gate.close()` — nenhum NOVO callback toca o mixer;
  2. `requestStop` (assíncrono);
  3. `waitForStateChange` (≤ 50 ms) — o estado assenta;
  4. `gate.waitDrained(150 ms)` — nenhum callback NOSSO em execução;
  5. `closeStream` (join autoritativo da thread interna do AAudio) e
     só então o mixer é solto.
- `mixer_` é `std::atomic<AudioMixer*>`: mesmo num device patológico
  que ignore a parada, o callback degrada para silêncio em vez de
  use-after-free.

### Causa 4 — Race onResume × surfaceCreated (BAIXA) → AUDITADA, INEXISTENTE

`onResume` (JNI → `startAudio`) e `surfaceCreated` (JNI → renderer)
rodam AMBOS na UI thread do Android (looper único — o próprio
`EditorActivity` os invoca sequencialmente; o Choreographer `doFrame`
idem). Não há paralelismo entre áudio e surface na fronteira. A única
concorrência real — a thread de áudio do AAudio — é coberta pelo gate +
átomos + mutex do mixer (contrato §D7/ADR-047). Nada a corrigir; a
auditoria está registrada aqui como evidência.

## 3. Marcos granulares (a evidência da próxima execução no C33)

O backend emite estágios DURANTE `start()` via hook instalado pelo
host (`setBackendProgressHook` — a engine de áudio não depende do
editor; direção da dependência preservada). Cada um persiste +
espelha ANTES do próximo começar (infra P3.2 intacta):

```
AUDIO_DLOPEN begin/ok/failed          (dlopen libaaudio.so)
AUDIO_SYMBOLS ok/failed               (17 dlsym — nome do ausente)
AUDIO_BUILDER_CREATE ok/failed        (null = emulador: esperado)
AUDIO_BUILDER_CONFIG ok               (req <rate>Hz <ch> float)
AUDIO_STREAM_OPEN begin/ok/failed     (ch/rate/fmt/cap EFETIVOS; código+texto)
AUDIO_STREAM_PARAMS_VERIFY ok/failed  (exact | adapt[...]; motivo da recusa)
AUDIO_STREAM_START ok/failed          (requestStart; código+texto)
AUDIO_CALLBACK_FIRST_FRAME ok         (observado pelo host na UI thread:
                                       "first=<frames do 1º bloco>")
```

Se o C33 ainda morrer na janela, o `goni_startup.log` mostrará a
chamada EXATA e o `goni_crash.log` (handler P3.3) o módulo+offset+bt —
a evidência se auto-documenta.

## 4. Fallback gracioso (o app NÃO pode morrer por áudio)

- QUALQUER falha retornável emite `failed` com detalhe preciso e o
  `start()` devolve `Error` — o host (`EditorHost::startAudio`) loga,
  marca `STARTUP_AUDIO failed` e segue **sem device** (previews/Play
  continuam; o editor fica 100% funcional, sem som).
- Nada foi desligado: o caminho AAudio continua ATIVO e é
  re-tentado a cada `onResume` (ciclo de vida P2 §12 inalterado).
- O que escapa a essa rede: SIGSEGV duro DENTRO de uma lib de sistema
  (o processo morre — impossível capturar sem violar a proibição de
  mascarar crash). Para esse caso restam os marcos §3 + handler P3.3.

## 5. Testes de regressão (Linux, ASan/UBSan/-Werror)

`engine/audio/tests/AudioTests.cpp` — 15 casos novos (29 no total do
módulo; 4.891 asserções):

- `convertFrames`: cópia float; downmix 2→1 soma+clamp; upmix 1→2
  duplica; 6→2 (política documentada); i32 escala ±(2³¹−1); clamp nos
  extremos ±1.5; **fonte nula → silêncio (nunca lixo no device)**.
- `LinearResampler`: recusa config inválida (taxa 0, razão 16×,
  canais 0); **exatidão matemática na rampa** (lerp de rampa = a
  própria posição — asserção EXATA): up 2×, down 2×, 48000→44100
  (160/147, lotes não alinhados → seams), **razão densa 1/6 em lotes
  de 1 frame** (regressão do bug de design do seam encontrado no
  desenvolvimento); DC inalterada; contabilidade push/produce;
  defensivo sem dados (finito, sem OOB).
- `CallbackGate`: recusa pós-close; drenagem imediata; timeout com
  entrante preso (tempo medido); drenagem real com thread (o callback
  sai durante a espera); reabertura.
- Estágios: nomes exatos (contrato do protocolo de diagnóstico — um
  typo destruiria a leitura do log no device); hook recebe
  estágio/status/detalhe 1:1 e limpeza com nullptr.
- NullBackend não reporta primeiro callback (observador do host nunca
  marca sem device real).

Extra: harness TSan dedicado (`scripts/tsan_p34_harness.cpp`) — 3
threads de "callback" × ciclos close/drain/reopen do "dono": 50/50,
zero data races.

## 6. Validação

| Item | Resultado |
|---|---|
| Linux Debug (ASan/UBSan/-Werror) | 31/31 — 4.891 asserções no módulo de áudio (5 execuções consecutivas) |
| Linux Release (LTO) | 31/31 |
| TSan (gate+resampler) | OK — ver §5 |
| CI Linux (`d4bc2db` e `645d95b`) | ✓ success |
| CI Android assembleDebug (arm64-v8a) | ✓ success (`645d95b`; o `d4bc2db` pegou o link faltante de eng::log — corrigido no `645d95b`) |
| Emulador ATD (P3.3) | Comportamento inalterado esperado: `AUDIO_BUILDER_CREATE failed null` → app VIVO sem áudio |
| **Realme C33** | **PENDENTE — critério decisivo, requer o usuário (ver §8)** |

## 7. APK

`goni-p3.4-final.apk` — artefato `goni-debug-apk` do run CI Android de
`645d95b` (assembleDebug, NDK 27.0.12077973, arm64-v8a + x86_64;
libgoni.so arm64 = 1.716.896 B; assinado debug):

```
sha256 a00c64d2cbb41a439f90f72785a033dbea1ee13effede5af9a17b1da58621790
7.059.372 bytes
```

## 8. Instruções para o Realme C33

1. Instalar `goni-p3.4-final.apk` (por cima do P3.3 é OK).
2. Abrir o G.ONI e observar: o esperado agora é o editor ABRIR e
   PERMANECER aberto (com ou sem som — o áudio adapta ou cai no
   fallback gracioso).
3. Se fechar sozinho: abrir DE NOVO — o app exporta os logs da
   execução morta para **Download/GONI/** e envia os dois arquivos:
   - `goni_startup.log` — a última linha `AUDIO_*` mostra a chamada
     exata da morte (agora com granularidade de chamada-a-chamada);
   - `goni_crash.log` — módulo+offset do pc/addr + backtrace (P3.3).
4. Com áudio funcionando, testar: Play com um som tocando (o pull
   adapta canais/formato/taxa automaticamente).

## 9. Limitações declaradas

- **SIGSEGV dentro de lib de sistema não é capturável no processo** —
  se o HAL da Unisoc falhar duramente em `openStream`/`requestStart`,
  o app morre (com a evidência §3+P3.3 no disco). Nenhuma correção do
  lado do app pode interceptar isso sem mascarar o crash (proibido).
- Erro-callback do AAudio (disconnect em runtime) não registrado —
  um disconnect só para o pull silenciosamente (documentado no AAudio);
  diagnóstico futuro se aparecer no device.
- PCM_I24/I8 sem conversor (recusa honesta com erro preciso) — nenhum
  HAL mobile real usa esses formatos para output em 2024–2026.
- A qualidade do resampler é linear (não band-limitado): correto para
  razões próximas de 1 (48000↔44100 — o caso real); para razões
  extremas (≥ 4×) haverá aliasing audível — aceitável para o editor,
  documentado aqui.
- ATD não valida o caminho com HAL real (createStreamBuilder null) —
  limitação permanente do emulador, já registrada no P3.3.

## 10. Arquivos tocados

```
engine/audio/include/eng/audio/AudioAdapt.hpp   (NOVO — conversor/resampler/gate)
engine/audio/src/AudioAdapt.cpp                 (NOVO)
engine/audio/src/AAudioBackend.cpp              (reescrito: 4 causas + marcos)
engine/audio/include/eng/audio/Audio.hpp        (estágios + hook + first-callback)
engine/audio/src/Audio.cpp                      (armazenamento do hook)
engine/audio/tests/AudioTests.cpp               (+15 casos de regressão)
engine/audio/CMakeLists.txt                     (+AudioAdapt.cpp)
editor/include/eng/editor/EditorHost.hpp        (observador first-frame)
editor/src/EditorHost.cpp                       (hook + marcos AUDIO_*)
scripts/tsan_p34_harness.cpp                    (NOVO — harness TSan)
docs/p34-aaudio-fix.md                          (este relatório)
```
