# P3.5 — Hang de inicialização + SIGABRT no Realme C33

Base: `f274ac8` (P3.4 — adaptação total ao HAL + drenagem de callback).
Commits: `64aa34f` (implementação + testes) + `dc89731` (relatório) +
`40e3456` (fix de compile do clang NDK — `kHandledCount` sem uso; o GCC
do CI Linux não pune, o clang do Android sim).

**APK final**: `goni-p3.5-final.apk` (CI Android de `40e3456`,
assembleDebug, NDK 27.0.12077973, arm64-v8a + x86_64)

```
sha256 e2efbd7e1fa67f910a9d0080d96944dcd3364e147c4a8864b0ea984d41f43337
7.112.572 bytes (libgoni.so arm64 1.736.952 B + x86_64 1.917.256 B)
```

CI Linux: **success** (64aa34f, 40e3456) · CI Android: **success**
(40e3456 — após o fix; o primeiro push `dc89731` falhou no compile do
clang, corrigido em `40e3456`).

---

## 1. O que o device mostrou (APK P3.4 instalado)

1. Splash congelado **~1–2 min** → tela preta → o processo morre
   sozinho (sem diálogo ANR — não há input pendente).
2. `goni_startup.log` (público, duas sessões): todos os estágios ok
   até `STARTUP_MATERIAL`; depois `STARTUP_RESUME begin → AUDIO begin →
   AUDIO dlopen ok → AUDIO symbols ok (17) → AUDIO builder null →
   AUDIO failed (gracioso) → STARTUP_RESUME ok`.
   **`STARTUP_SURFACE`, `STARTUP_RHI`, `STARTUP_SHADER_CORE`,
   `STARTUP_COMPLETE` NUNCA aparecem.**
3. `goni_crash.log`: `SIGABRT(6) code=-6` (SI_TKILL — abort chamado
   DE DENTRO do processo), backtrace não-confiável (frames repetidos
   8/10/12, frame 16 = `0x17`), PC em `module=anon` — hoje
   indistinguível entre JIT do ART e erro de atribuição do parser
   porque **o snapshot cru do maps não era gravado**.

A janela de morte P3.5 fica, portanto, **entre `STARTUP_RESUME ok` e
`STARTUP_SURFACE begin`** — o trecho que no Android é puro framework:
retorno do `onResume` → primeiro traversal da view tree →
`surfaceCreated`. Nenhum marco nosso cobria essa janela — o P3.5
passa a cobri-la sub-passo a sub-passo (§4).

## 2. Causa raiz do hang — VERIFIED (por eliminação estrutural)

**A main thread estava presa em I/O do MediaStore do próprio
diagnóstico.** A cadeia completa:

1. Cada `diag::mark()` do P3.2 invocava o trampoline JNI
   **na thread que marcou** (a main) →
   `DiagnosticsMirror.onNativeDiagnosticsChanged()` →
   `mirrorFile()` → `ContentResolver.query()` +
   `openOutputStream()` + `write()` + `update()` **NA MAIN THREAD**.
2. Com ~20 marks no startup, a main thread executava ~20 reescritas
   completas do arquivo via MediaProvider — **binder calls + camada
   Java sobre eMMC lento**, um padrão documentado de UI freezes no
   Android 11–13 (Scoped Storage).
3. O hang de 1–2 min é coerente com um binder para o MediaProvider que
   nunca retorna (scanner de mídia, quota, FUSE). A main thread não
   processa mensagens; sem input pendente não há ANR do sistema.
4. O SIGABRT final é **consequência, não causa**: um GC (ou qualquer
   suspend-all do ART) que não consegue suspender a main thread dentro
   do timeout interno do runtime aborta o processo — `abort()` =
   SIGABRT com SI_TKILL, backtrace dentro do ART. A premissa da
   missão (causa raiz primária = hang) foi confirmada pela estrutura:
   o áudio já tinha falhado GRACIOSAMENTE (builder null) segundos antes
   do congelamento — nenhum caminho de áudio estava ativo.

**Correção estrutural (T1)**: a main thread **nunca mais toca
MediaStore no caminho de mark**. `onNativeDiagnosticsChanged()` agora
apenas enfileira (post numa worker single-background) e retorna em
microssegundos. O arquivo privado em `filesDir` segue síncrono (I/O
local barato — feito pelo C++ com flush+fsync ANTES do enqueue, então
a garantia de sobrevivência à morte súbita do P3.2 permanece).

Estado: **VERIFIED** como causa raiz *por eliminação estrutural* —
cada candidato da janela foi removido ou instrumentado (§4/§5); a
confirmação definitiva no hardware depende da execução do APK P3.5 no
device (§8). Nenhuma hipótese foi "provada" por evidência direta do
device — e é exatamente por isso que a instrumentação P3.5 existe.

## 3. Origem do SIGABRT — NOT VERIFIED (hipótese, agora com captura dedicada)

O backtrace P3.4 (frames `anon` repetidos + `0x17`) é não-confiável. As
hipóteses permanecem: (a) timeout de suspend-all do ART sobre a main
presa; (b) erro JNI fatal numa thread nativa não-attachada; (c) abort
de heap do FORTIFY. **O que mudou no P3.5**: se o SIGABRT voltar a
acontecer, o novo handler grava o CONTEXTO EXATO (pc + pilhas `[fp]` e
`[scan]`) da thread sinalizada **e de TODAS as outras threads do
processo** (§5) — num abort por JNI/suspend a thread culpada costuma
não ser a sinalizada. Além disso o maps cru verbatim permite validar
offline a atribuição módulo/base contra a build exata.

## 4. Micro-marks da janela resume→surface (T2)

Cada sub-passo invisível agora deixa rastro com **timestamps duplos**
(`[wt=<ms wallclock> mo=<ms monotônico>]` — deltas confiáveis entre
marks, correlação com logcat/tombstones):

| Mark | Onde | Prova |
|---|---|---|
| `MIRROR_ENQUEUE` | Activity.onResume, pós-JNI | o espelho assíncrono aceitou o handoff |
| `RESUME_RETURN` | fim do onResume | choreographer armado, main escapou do resume |
| `LOOPER_IDLE` | runnable postada no main looper | **a main processa mensagens** (se travar no resume/traversal, este é o 1º ausente) |
| `FIRST_TRAVERSAL` | primeiro `doFrame` | o Choreographer entregou frame |
| `SURFACE_DISPATCH` | entry Kotlin do surfaceCreated | o framework entregou a surface |
| `STARTUP_SURFACE begin/window/acquired/renderer` | fronteira JNI (P3.3) | aquisição da janela |
| `RHI_BACKEND_SELECT` | `Renderer::create` (hook novo) | seleção de backend |
| `RHI_INSTANCE` | vkCreateInstance / eglInitialize | instância viva |
| `RHI_DEVICE` | vkCreateDevice / contexto ES3 | device lógico pronto |
| `RHI_SURFACE` | VkSurfaceKHR / EGLSurface | surface de apresentação |
| `RHI_SWAPCHAIN` | createSwapchain (Vulkan) | swapchain pronta |
| `FIRST_FRAME` | primeira submissão | frame no pipeline |
| `STARTUP_COMPLETE` | primeira apresentação | editor visível |

O log exportado deve mostrar **exatamente qual sub-passo nunca
completa** — o critério de sucesso diagnóstico da missão.

O hook do RHI (`eng/rhi/Progress.hpp`) segue o padrão ADR-047 do
eng::audio: a camada de RHI não conhece o diagnóstico (grafo acíclico)
— o host instala `rhiBackendProgress` antes de `Renderer::create`.

## 5. Watchdog de hang + forense completa (T3/T3b)

Hang sem ADB é invisível (`/data/anr`, `/data/tombstones` exigem
root). O G.ONI agora tem o seu próprio "ANR":

1. `Watchdog.start()` (onCreate) → `diag::watchdog::arm()` captura a
   main thread (pthread_t/tid).
2. Pinger daemon (1 s): `Handler.post` → runnable chama
   `heartbeat()`. **Por que Handler.post e não Choreographer**: o
   Choreographer só entrega callbacks com trabalho de UI agendado (app
   em background/UI estática = `doFrame` não dispara = falso
   positivo). O looper da main processa mensagens sempre que está
   saudável.
3. Sem heartbeat por **8 s** (graça de 15 s no startup) →
   `evaluate()` grava `[watchdog]` no `goni_crash.log` e
   `pthread_kill(main, SIGUSR1)`.
4. O handler roda **na main travada** e despeja o contexto exato —
   pc, pilhas `[fp]`/`[scan]`, TODAS as outras threads, maps cru.
   **O processo continua vivo**: SIGUSR1 aqui é diagnóstico, não
   fatal (uma arma por episódio).
5. **Preservação do ART**: o ART usa SIGUSR1 para suspensão de GC. Um
   *sentinel* (`g_pendingWatchdogTid`) distingue o nosso poke do
   suspend do ART: nosso poke → dump + retorno; poke do ART →
   encadeamento ao handler anterior (GC intacto). Nunca mata por
   sinal diagnóstico.

Captura forense (em QUALQUER sinal handled — fatal ou watchdog):

```
[crash] signal=SIGABRT(6) code=-6 ... si_pid=<tid do remetente> tid=<gettid>
[dump] tag=fatal|watchdog|other tid=... signal=... code=... si_pid=...
[pc] 0x... module=<path|anon> base=0x... off=0x...
[fp] N 0x... module=... base=0x... off=0x...     (frame-pointer walk, 24 frames)
[scan] N 0x... module=... base=0x... off=0x...   (candidatos executáveis na pilha, 64)
[thread] tid=... comm=... (ping SIGUSR2)          (enumeração getdents64)
[dump] tag=other tid=...                          (cada outra thread despeja a si mesma)
[maps.raw begin] ... verbatim 256 KiB ... [maps.raw end]
```

Tudo com `open/read/write/getdents64/nanosleep` — sem malloc, sem
locks, sem `dladdr` (getdents64 lido por `memcpy` — os registros NÃO
são alinhados e o linux-debug compila este TU com UBSan).

## 6. Áudio endurecido (T4)

- `AAudioStreamBuilder_setPerformanceMode(AAUDIO_PERFORMANCE_MODE_NONE)`
  explícito — **sem MMAP** no Unisoc T612 (suportado só em devices
  selecionados; padrão da indústria/Oboe). Agora 18 símbolos por dlsym.
- `AAudio_createStreamBuilder` devolvendo null em device REAL agora é
  **ERRO com errno explícito** (a mensagem P3.4 dizia "emuladores:
  esperado" — errada para device). A função não retorna
  `aaudio_result_t`; o errno no momento da chamada é a única evidência
  disponível e agora é logada.
- **Retry assíncrono** (nunca mais síncrono em `onResume`): worker de
  áudio dedicado; tentativa imediata + retries a +1/+2/+4 s
  (interpretação documentada do "backoff 1/2/4, máx 3 tentativas de
  retry": 4 tentativas totais). Época (`audioEpoch_`) invalida ciclos
  de retry de gerações velhas no próximo checkpoint.
- Falha definitiva → **NullBackend gracioso** com mark
  `STARTUP_AUDIO null-fallback` — app vivo, previews/Play sem som; o
  próximo resume descarta o null e tenta o device real de novo.
- `onPause` **nunca** para o device (apenas `pauseAll` das vozes — o
  stream segue puxando silêncio): um binder do HAL preso não pode
  congelar o pause do app. O teardown é do destrutor, no worker, sob
  a posse serializada do AAudio.
- Serialização T0: `audioOpMutex_` protege sequências inteiras de
  start/stop (só worker/destrutor a tomam — a main nunca);
  `audioPtrMutex_` (µs) guarda o `shared_ptr` do backend que a
  `renderFrame` lê por snapshot.

## 7. Validação

| Item | Resultado |
|---|---|
| linux-debug (ASan+UBSan+LSan, `-Werror`) | **31/31** |
| linux-release (LTO) | **31/31** |
| editor (dentro do 31) | 118 casos / **1.749 asserções** — 0 falhas |
| Casos novos | espelho assíncrono T0 (callback fora da thread que marcou), stress 4×64 marks concorrentes, timestamps wt/mo, watchdog fork-vivo (SIGUSR1 diagnosticado, processo segue vivo), forense completa (threads+maps cru) |
| TSan dedicado (`scripts/tsan_p35_harness.cpp`) | **ZERO data races** — mirror×6 threads com callback lento, watchdog arm/heartbeat/evaluate concorrentes, CallbackGate (regressão P3.4), 5 ciclos resume/pause/destroy do host com worker de áudio |
| Bug real encontrado pelo TSan | corrida no `crashGlobals()` (escritas concorrentes de `mark()` sem lock) — corrigida com serialização; o handler de sinal continua lendo best-effort (documentado desde P3.1) |
| CI Linux | VERDE (`64aa34f`, `40e3456`) |
| CI Android (assembleDebug) | VERDE (`40e3456`) |
| APK final + SHA256 | §9 (CI de `40e3456`) |

Regressões P3.2/P3.4 preservadas: zero duplicatas no MediaStore
(Owner+prefixo+limpeza), ordem FIFO, re-publicação de IS_PENDING,
AudioAdapt/CallbackGate intocados (suite P3.4 verde).

Mudança de contrato documentada: o callback de espelho do
`diag::mark()` passou de síncrono (1 chamada por mark, na thread do
chamador) para **assíncrono em lote** (thread de despacho dedicada,
coalescing — o export reescreve o arquivo COMPLETO, então o último
estado sempre prevalece; a garantia "cópia pública reflete o último
estágio concluído" é mantida). O teste P3.2 foi reescrito para o
contrato novo; a garantia funcional (zero duplicatas, ordem,
sobrevivência à morte) é verificada pelos casos novos.

## 8. Instruções de teste no device (Realme C33)

1. Instalar o APK P3.5 (arm64) e abrir o G.ONI.
2. **Sucesso esperado**: editor visível em <10 s, processo vivo, com
   ou sem som (o NullBackend gracioso é uma resposta válida do áudio).
3. **Se ainda falhar**, enviar os DOIS arquivos de `Download/GONI/`:
   - `goni_startup.log` — com os micro-marks: o último mark antes do
     silêncio nomeia o sub-passo que nunca completa (ex.:
     `RESUME_RETURN` sem `LOOPER_IDLE` = main presa no primeiro
     traversal; `RHI_INSTANCE` sem `RHI_DEVICE` = driver preso na
     criação do device).
   - `goni_crash.log` — agora contém (a) o dump `[watchdog]` com o
     contexto EXATO da main travada, (b) dumps `[other]` de todas as
     threads (a culpada de um abort JNI/suspend costuma não ser a
     sinalizada), (c) `[maps.raw]` verbatim para validar offline a
     atribuição módulo/base contra o `libgoni.so` não-stripped da
     build, (d) `si_pid`/`gettid` de cada sinal.
4. Não é preciso mais nada (sem ADB, sem root, sem cabo): os arquivos
   públicos cobrem todo o ciclo de evidência.

## 9. APK final

`goni-p3.5-final.apk` (CI Android do commit `40e3456`, assembleDebug,
NDK 27.0.12077973, arm64-v8a + x86_64):

```
sha256 e2efbd7e1fa67f910a9d0080d96944dcd3364e147c4a8864b0ea984d41f43337
7.112.572 bytes
```

## 10. Limitações honestas

- A causa raiz do hang é VERIFIED por eliminação estrutural, mas a
  confirmação final no hardware depende do device (§8) — emulador e
  CI não provam o Realme C33.
- A origem exata do SIGABRT permanece NOT VERIFIED (§3): com o
  watchdog + forense total, a próxima ocorrência é capturada com a
  thread culpada nomeada (evidência suficiente para symbolização
  offline definitiva).
- Worker de áudio presa num binder do HAL: um `openStream` que nunca
  retorna segura o join do destrutor (o AAudio não oferece timeout
  de chamada). Limitação inerente da API — e exatamente o cenário que
  o watchdog captura com dump completo da main (que estaria presa no
  join do teardown).
- Export wedged (MediaStore >2 s): a worker antiga fica presa no
  binder (impossível abortar um binder call do cliente); tarefas
  abandonadas por geração são no-op. Pior caso residual: um
  intercalado brevíssimo de conteúdo antigo/novo no arquivo público,
  curado pelo próximo mark.
- SIGUSR2 de terceiros: ninguém além do G.ONI envia SIGUSR2 a si
  mesmo aqui; um disparo externo produz um dump extra (inofensivo),
  nunca morte.
- O stream de áudio segue aberto (puxando silêncio) durante pause —
  custo de bateria mínimo aceito em troca de um pause que nunca
  bloqueia em binder do HAL.
