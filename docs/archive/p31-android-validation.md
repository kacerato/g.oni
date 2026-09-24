# P3.1 — Validação de runtime Android: ambiente, emulador e diagnóstico

> Escopo: transformar o ambiente Linux de desenvolvimento em um ambiente
> capaz de `BUILD → APK → INSTALL → LAUNCH → OBSERVE → DIAGNOSE → FAIL/PASS`
> sem depender de logcat manual, mais o sistema de diagnóstico de startup
> persistente + crash handler nativo dentro do G.ONI. Base: `3bfc6c3`.

## 1. Ambiente Linux auditado (FASE 1)

| Componente | Valor | Status |
|---|---|---|
| CPU | 2 núcleos x86_64 | limitante principal |
| RAM | 3.9 GB (sem swap) | limitante principal |
| Disco | 9.9 GB (overlayfs) | limitado — gerenciado com staging em FUSE |
| KVM | **AUSENTE** (`/dev/kvm` não existe) | emulador em TCG (software) |
| JDK | Temurin 17.0.20.1 (`/home/z/jdk`) | o JDK do sistema era JRE |
| Android SDK | cmdline-tools 12.0, platform-tools 37.0.1, build-tools 34.0.0, platform 34 | completo |
| NDK | 27.0.12077973 | igual ao CI |
| CMake | 3.31.6 (SDK) | igual ao CI |
| Emulador | 37.1.11 | veja §3 |
| System image | `android-31;aosp_atd;x86_64` (ATD) | API 31 = Android 12 do Realme C33 |

Limitações estruturais do ambiente (documentadas, não contornáveis sem
root): sem KVM, sem swap, processo em background morre ao fim de cada
chamada da tool (SIGKILL — testado com `setsid`/`nohup`/trap), overlayfs
não devolve espaço de arquivos pré-instalados deletados.

## 2. Decisões de infraestrutura

1. **AVD em storage FUSE** (`/tmp/my-project` — quota de rede, não disco
   local): userdata/qcow2 crescem sem esgotar o rootfs.
2. **Image staging via FUSE**: o zip da system image é baixado e extraído
   no FUSE quando o rootfs não comporta zip+extração simultâneos
   (sdkmanager faz os dois no mesmo disco e falhava com ENOSPC).
3. **`LD_PRELOAD=/home/z/my-project/scripts/mmap_norescue.so`**: o
   emulator 37 reserva 4 GB anônimos (pool gfxstream/SwiftShader) SEM
   `MAP_NORESERVE`; com overcommit heurístico e sem swap o kernel recusa
   (ENOMEM) e o emulator aborta com `Insufficient RAM free for launching
   emulator` — **mesmo com 3.1 GB livres**. O shim reexecuta a reserva com
   `MAP_NORESERVE` (modo correto para pool de endereço). Sem ele o
   emulador não sobe de forma alguma neste host.
4. **Cold boot por ciclo**: restore de snapshot falha sob TCG
   (`Error -22 while loading VM state`) — documentado; boot frio ~210-240s.
5. **Teardown graceful**: `adb emu kill` antes de SIGTERM/SIGKILL — o
   SIGKILL no QEMU perde o flush do userdata (o app "sumia" do AVD).

## 3. Configuração estável do emulador

```
emulator -avd goni_p31 -no-window -no-accel -gpu swiftshader_indirect \
  -no-audio -no-boot-anim -memory 2048 -cores 4 -no-snapshot -port 5554
LD_PRELOAD=<shim> ANDROID_AVD_HOME=/tmp/my-project/avd
```

- `-cores 4`: com 2 vCPU o guest entra em livelock de escalonamento
  (kernel `livelock: sample`) e o `networkstack` ANRa (`bg anr`) →
  system_server se mata (`Lost network stack`). Com 4 vCPU a maior parte
  dos ciclos sobrevive.
- `-memory 2048`: viável APÓS o shim do item §2.3 (o limite anterior era
  o bug de reserva, não a RAM real).

## 4. Scripts (um comando por ciclo completo)

| Script | Papel |
|---|---|
| `scripts/p31_install_sdk.sh` | instala SDK completo (idempotente) |
| `scripts/p31_boot_snapshot.sh` | boot completo + snapshot (referência) |
| `scripts/p31_emu_cycle.sh <apk> [observe] [backend] [activity]` | **ciclo completo**: cold boot → install (3 tentativas) → launch → observação com pull incremental dos estágios → coleta (logcat full/crash-buffer, goni_startup.log, goni_crash.log, screenshot, getprop) → verdict |

O ciclo distingue: `PASS` (processo vivo na janela), `FAIL_CRASH_GONI`
(crash nativo no processo do app), `FAIL_PROCESS_DEAD`,
`ENV_SYSTEM_DEATH_*` (o ATD/TCG morre sozinho — documentado; control test
com Settings morre igual).

## 5. Diagnóstico dentro do G.ONI (FASES 4/5/6)

- `eng::editor::diag` (`editor/src/Diagnostics.cpp`):
  - `goni_startup.log` em app-private storage — **cada estágio é gravado
    na hora** (append + fflush + fsync): sobrevive à morte do processo;
  - estágios: `STARTUP_{NATIVE_LIBRARY, JNI, APPLICATION, ACTIVITY,
    EDITOR_HOST, FILESYSTEM, PROJECT, RHI, VULKAN|GLES, SHADER_CORE,
    MATERIAL, EDITOR_DOCUMENT, EDITOR_UI, COMPLETE}` + `failed` com erro;
  - espelho no logcat (`[GONI][STARTUP] ...`) para validação via adb;
  - crash handler nativo (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE):
    `goni_crash.log` com sinal, código, endereço, PC, **último estágio**,
    e **encadeamento ao handler anterior** (debuggerd/ART/tombstone) — o
    crash NÃO é mascarado (testado: filho morre com o sinal; ASan segue
    imprimindo seu relatório);
  - handler self-healing: reabre o fd se terceiros fecharem, e
    `installCrashHandler()` pode ser rechamado (frameworks sobrescrevem
    handlers — Catch2 fazia isso entre casos de teste);
- Export sem logcat (FASE 6): menu `Projeto → Exportar diagnóstico
  (arquivos)…` grava `goni-diagnostics.zip` (startup+crash) via SAF; e
  **na abertura seguinte a um crash**, a Activity oferece o export ANTES
  de qualquer carga (o arquivo persiste entre execuções).

## 6. Evidência coletada (FASE 7 — reprodução e diagnóstico)

APK base `3bfc6c3` (arm64-v8a original): o emulador x86_64 não o executa
(`INSTALL_FAILED_NO_MATCHING_ABIS`) — adicionada a ABI `x86_64` ao MESMO
artefato (nada removido; o APK continua arm64 para o Realme C33).

Com o APK dual-ABI do MESMO código (`sha256 1b1611e3…`):

1. **Demo runtime** (GoniActivity, mesmo APK): janela nativa →
   `libvulkan.so` carregado → `VkInstance` → GPU `SwiftShader Device
   (Subzero)` → swapchain 2280x1080 (4 imagens, MAILBOX) →
   **`First frame submitted` / `First frame presented`** — pipeline de
   render Vulkan completo funcionando no Android; processo vivo por toda
   a janela de observação.
2. **Editor** (EditorActivity): `STARTUP_NATIVE_LIBRARY → JNI →
   APPLICATION → ACTIVITY → EDITOR_HOST → FILESYSTEM → EDITOR_DOCUMENT →
   EDITOR_UI → PROJECT(MeuJogo) → MATERIAL` em ~9 s — processo vivo
   (>300 s) sem crash próprio. Os estágios de render do editor não foram
   alcançados NA JANELA do emulador: sob TCG a primeira travessia de UI
   do editor leva minutos (kernel reporta livelock) — o caminho de render
   em si está validado pela demo runtime (mesma engine/RHI/shader).
3. **Control test** (Settings): o guest ATD/TCG morre sozinho ~55-120 s
   pós-boot (`networkstack` `bg anr` → `Lost network stack`) — instância
   documentada de morte AMBIENTAL que não é culpa do app.

### Bug real encontrado e corrigido (pela própria FASE 4)

`copyJString`/`jniToString` (EditorJni.cpp e GoniJni.cpp) passavam o
comprimento em **bytes MUTF-8** para `GetStringUTFRegion`, que espera
**unidades UTF-16** → `StringIndexOutOfBoundsException` (crash do app)
em **qualquer string acentuada** cruzando a fronteira JNI — descoberto
ao marcar `STARTUP_EDITOR_UI ok "UI construída"`: 14 bytes vs 13
unidades. Corrigido nos 4 pontos; afetava desde P2 (nome de projeto
"Ação", conteúdo de script com acento etc.).

## 7. Limitações restantes (FASE 13 — Realme C33)

- **DEVICE VALIDATION: NOT AVAILABLE** — sem o dispositivo físico, o
  SoC Unisoc T612 / GPU Mali / driver Vulkan real / lifecycle real
  permanecem POR VALIDAR. O APK final desta fase (dual-ABI, com
  diagnóstico) leva o crash handler e os estágios persistentes: quando
  instalado no Realme C33, o `goni_startup.log`/`goni_crash.log` serão
  extraíveis pelo menu `Exportar diagnóstico` ou
  `adb shell run-as com.goni.runtime cat files/goni_startup.log`.
- O crash original reportado no Realme C33 (app fecha 2-3 s após o
  splash, sem logcat) NÃO foi reproduzido no emulador (o app dual-ABI do
  mesmo código passou por todos os estágios até MATERIAL e a demo de
  render apresentou frames). O bug de acento JNI é um candidato plausível
  a essa morte súbita em PT-BR, mas **sem evidência do device não se
  declara causa raiz** — o objetivo do diagnóstico persistente é obtê-la
  na próxima execução física.
- Snapshot/quickboot não funciona sob TCG (Error -22) — ciclos são
  cold-boot (~210-240 s cada).
