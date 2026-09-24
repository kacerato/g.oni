# P3.3 — Realme C33 SIGSEGV: Symbolização + Correção

Base: `5c7e203` (P3.2) → este commit: `a3bef7a` (+fix CI: snapshot de
maps truncava em 512 entradas em processos com ASan — elevado a 2048
com report de truncagem)
APK instrumentado: `goni-p3.3-instrumented.apk`
sha256 `9dbb587f217a9db780bd9716d8f77a672dcfa9a8f82ce0c0ee14453d000f030d`

---

## 1. Crash real observado (device, exportado pelo mecanismo P3.2)

```
signal=SIGSEGV(11)
code=2          (SEGV_ACCERR — página mapeada, permissão insuficiente)
addr=0x6e1b835460
pc  =0x6e1b832140
delta addr-pc = 0x3320
stage=STARTUP_MATERIAL  detail=MeuJogo
```

Última linha persistida do startup: `STARTUP_MATERIAL ok MeuJogo` —
o estágio **concluiu** (incluindo o espelho público em Download/GONI,
que o usuário leu). `STARTUP_RHI begin` (primeira instrução útil de
`ViewportRenderer::create`) **nunca** foi gravado.

## 2. ETAPA 1 — Symbolização

### 2.1 Libgoni.so correspondente ao APK

Rebuild exato do `5c7e203` com o toolchain oficial (NDK `27.0.12077973`,
CMake `Release`, AGP 8.5.2, dual-ABI arm64-v8a + x86_64 — mesmo
`android/app/build.gradle.kts` do P3.2). A cópia **não stripped** está
preservada em `/home/z/my-project/p33/libgoni.arm64.unstripped.so`
(34 MB, símbolos + debug info). O `.text` arm64 vai de
`0x7a4b0` a `0x195134` (LOAD1 RX = `[0, 0x1997a0)`; RELRO
`[0x19a7a0, 0x19f000)`; RW depois disso).

### 2.2 Cálculo do offset do PC (documentado)

O handler P3.2 **não registra o load-bias** da lib — sem ele, o offset
do PC não é conhecido diretamente. O que é possível afirmar:

- `LOAD1` tem vaddr `0` ⇒ load-bias = base do mapeamento, **alinado a
  página (0x1000)**;
- logo `pc_off ≡ 0x140 (mod 0x1000)`;
- `code=2` com `addr = pc + 0x3320` ⇒ acesso (quase certamente
  **escrita**) a página **read-only** do MESMO mapeamento (~12,9 KB
  acima do PC): `.text`/`.rodata`/`.data.rel.ro` de uma biblioteca.

### 2.3 Enumeração exaustiva dos candidatos em libgoni.so

Varredura completa do `.text` (294.073 instruções) coletando toda
instrução em offset `≡ 0x140 (mod 0x1000)` que seja **store**:
**45 candidatos**. Todos foram eliminados:

- **41 são stores de PILHA** (`str …, [sp, #imm]` / `stp`): stack é RW —
  não produzem SEGV_ACCERR (exceto stack overflow, cujo endereço de
  falta ficaria na região da pilha, nunca `pc + 0x3320` dentro da
  própria lib);
- os 4 restantes (`File::open`, `ViewportRenderer::create`,
  `GlesBackend::frameDraw`, `introsort`) escrevem em heap/stack via
  registrador válido e/ou **não pertencem à janela de execução** —
  nenhum alvo em `+0x3320` cai em objeto estático real (todos caem em
  código).

**Conclusão (ETAPA 1):** o PC **não está em libgoni.so**. Está numa
biblioteca de sistema do processo, chamada na janela de morte (ver
§3). A symbolização definitiva exige a identidade do módulo — que o
handler P3.2 não capturava. O novo handler (§5) captura.

## 3. ETAPA 2 — Fluxo imediatamente depois de STARTUP_MATERIAL

Auditoria de código completa da janela (a Activity é single-thread UI +
thread de áudio):

```
diag::mark("STARTUP_MATERIAL","ok","MeuJogo")   ← concluído (privado + espelho público)
  ↓ retorno JNI (NewStringUTF)                   [trivial]
  ↓ refreshAll() → projectName + brand.text      [trivial; activePanel = PANEL_NONE ⇒ sem hierarquia]
  ↓ onResume → startAudio():
      dlopen("libaaudio.so") + 10×dlsym          [chamada de sistema]
      AAudioStreamBuilder → openStream           [binder → audio policy → HAL do dispositivo]
      requestStart → THREAD DE CALLBACK DE ÁUDIO [concorrente dali em diante]
  ↓ primeira travessia de UI → surfaceCreated:
      dumpState (logcat)                          [trivial]
      ANativeWindow_fromSurface/acquire           [libandroid]
      watchdogRead (arquivo)                      [trivial]
  ↓ createRendererForWindow → ViewportRenderer::create
  diag::mark("STARTUP_RHI","begin")              ← NUNCA alcançado
```

No emulador P3.1/P3.2 este trecho **nunca rodou**: `-no-audio` e a
primeira travessia da UI do editor sob TCG leva minutos. No Realme C33
(hardware real) ele roda — e é a janela onde o processo morre.

## 4. ETAPA 3 — Reprodução nativa

- **Linux Debug ASan/UBSan/Werror** (base `5c7e203`, antes das mudanças):
  31/31 testes — nenhum UB/UAF nos caminhos cobertos.
- **Linux Debug ASan/UBSan/Werror** (com P3.3): 31/31 — incluindo os
  novos testes (§6).
- **Linux Release/LTO**: 31/31.
- Reprodução no emulador local: o sandbox mata processos background de
  CPU (comprovado com teste dedicado — o emulador TCG morre ~3–4 min
  no boot em background) e a janela de 600 s por chamada não comporta
  boot de imagem completa + observação. Com a imagem **ATD x86_64**
  (a mesma do P3.1, boot ~207–326 s) o ciclo completo coube: ver §7.

## 5. Evidência que faltava (implementada)

### 5.1 Crash handler: módulo do PC + do alvo + backtrace

O handler agora grava, além da linha `[crash]` (formato P3.1/P3.2
intocado):

```
[pc]        0xADDR module=<path> base=0xB off=0xD
[fault.addr] 0xADDR module=<path> base=0xB off=0xD
[bt.pc]     …
[bt] 0 0xRET <modulo>+0xOFF … (até 24 frames, frame-pointer walk)
```

Implementação async-signal-safe: `/proc/self/maps` via `open/read`
(**sem** `dladdr` — os locks do dynamic linker deadlockariam se o crash
ocorrer DURANTE o `dlopen` do AAudio, que é exatamente a janela
suspeita); parse em buffers estáticos (zero alocação); cada
desreferência do walk é validada contra o snapshot de mapeamentos
(somente páginas legíveis); `write(2)` por linha + `fsync`. O snapshot
comporta 2048 entradas e reporta truncagem — processos com ASan têm
milhares de mapeamentos (shadow) e a versão com 512 fazia o pc cair
fora do snapshot (bug real achado pelo CI Linux).

### 5.2 Marcos granulares na janela (infra P3.2 intacta)

`STARTUP_POST_PROJECT` / `STARTUP_UI_SYNC` (Activity) ·
`STARTUP_RESUME begin|ok` · `STARTUP_AUDIO begin|backend|started|failed`
(com parâmetros **efetivos** do stream) · `STARTUP_SURFACE
begin|window|acquired|renderer`.

## 6. Correção real encontrada e aplicada (ETAPA 5)

**Bug (AAudio stride):** os valores passados ao builder são
**sugestões** (documentação do AAudio) — o stream aberto pode ter
outros. O callback escrevia `numFrames × mixer.channels() × float`
incondicionalmente: com canais/formato diferentes no stream aberto,
`memset`/`mix` escrevem **fora** do buffer do AAudio (corrupção de
heap). Correção: após `openStream`, consulta
`AAudioStream_getChannelCount/getSampleRate/getFormat` do stream
**aberto**; se canais ≠ mixer ou formato ≠ PCM_FLOAT, fecha e devolve
erro preciso (o app segue sem áudio — falha honesta, sem crash, sem
corrupção). `IAudioBackend::describeDevice()` expõe `ch/rate/fmt` no
marco `STARTUP_AUDIO started`.

**Nota de honestidade:** não há evidência de que ESTE bug seja a causa
do SIGSEGV do C33 (a assinatura `addr=pc+0x3320` em página read-only de
lib não casa com overflow de buffer de heap). A correção elimina uma
classe real de crash no mesmo trecho; a causa exata será identificada
pela nova evidência do §5 no próximo crash no device.

## 7. Validação

| Item | Resultado |
|---|---|
| Linux Debug ASan/UBSan/Werror | 31/31 (2 casos novos/estendidos) |
| Linux Release/LTO | 31/31 |
| assembleDebug (arm64+x86_64) | OK |
| CI Linux / CI Android (a3bef7a) | [ver runs] |
| Emulador ATD x86_64 c/ áudio | Fluxo COMPLETO até `STARTUP_RESUME ok`; `STARTUP_AUDIO backend AAudio` → `failed AAudio_createStreamBuilder devolveu null` (gracioso, como projetado); processo VIVO; zero `[crash]` |
| **Realme C33** | **PENDENTE — critério decisivo, requer o usuário** |

## 8. Instruções para o Realme C33

1. Instalar `goni-p3.3-instrumented.apk` (por cima do anterior é OK).
2. Abrir o G.ONI e deixar o comportamento ocorrer (abrir/fechar).
3. Se fechar sozinho: abrir DE NOVO — o app exporta o
   `goni_crash.log` da execução morta imediatamente no início
   (mecanismo P3.2) para **Download/GONI/**.
4. Ler os arquivos (gerenciador de arquivos → Downloads → GONI):
   - `goni_startup.log` — agora com os marcos granulares: a ÚLTIMA
     linha mostra o trecho exato da morte (ex. `STARTUP_AUDIO backend`
     = dentro das chamadas AAudio);
   - `goni_crash.log` — agora com `[pc]`/`[fault.addr]`
     `module=… base=… off=…` e o `[bt]` — **nomeia a biblioteca e o
     offset exatos do crash**.
5. Enviar os dois arquivos — o offset de `libgoni.so` symboliza
   diretamente contra a cópia não-stripped preservada; libs de sistema
   identificam o suspeito (áudio vs surface vs ART) pelo nome do módulo.

## 9. Limitações

- O load-bias do crash ORIGINAL (APK P3.2) não foi registrado — a
  symbolização retroativa só pôde ser por eliminação (§2.3).
- ATD não tem volume externo (`external_primary` ausente) — o espelho
  público não é validável lá (limitação P3.2 conhecida); o pull usou
  root/run-as.
- No ATD o AAudio não passa de `createStreamBuilder` (retorna null) — o
  caminho com HAL REAL (o que interessa para o C33) só existe no device.
- A segunda sessão do ATD registrou `STARTUP_PROJECT failed parseJson`
  ao reler o projeto criado pela primeira (processo morto entre
  criação e relitura — sem [crash]): não relacionado às mudanças P3.3
  (nenhum código de projeto foi tocado); registrado como observação
  para investigação futura se reaparecer no device.
