# P3.2 — Export automático de diagnósticos sem ADB (Download/GONI)

> Problema real: o APK fecha sozinho no Realme C33 após o splash e o
> dispositivo **não expõe** `run-as` (`Permission denied` no Termux), nem
> `/data/data`, nem logcat externo, nem ADB/Wireless Debugging. O
> diagnóstico persistente do P3.1 (filesDir/goni_startup.log +
> goni_crash.log) estava preso no armazenamento privado — invisível para
> o usuário.
>
> Solução: o próprio G.ONI copia os diagnósticos para
> **Download/GONI/** (armazenamento compartilhado, acessível pelo
> gerenciador de arquivos e via cabo USB/MTP) ANTES de qualquer possível
> crash — sem depender do Editor UI, sem diálogo, sem permissões no
> Android 10+.

## 1. Arquitetura

```
onCreate (antes de qualquer subsistema)
  ├─ DiagnosticsMirror.init(this)
  ├─ try { EditorJni.bootstrap(this) }  catch (Throwable)
  │     └─ recordBootstrapFailure(t)   ← falha de dlopen/libgoni.so
  │        grava causa+stack em Java puro e espelha
  ├─ hasPreviousCrashReport() == true → exportCrashLogIfPresent()
  │     ← crash REAL da execução ANTERIOR exportado IMEDIATAMENTE
  └─ ... criação do editor ...

C++ diag::mark(estágio)  [a cada estágio]
  ├─ persiste em filesDir/goni_startup.log (flush+fsync — P3.1)
  └─ notifyMirror() → trampoline JNI → DiagnosticsMirror
       .onNativeDiagnosticsChanged() → REESCREVE Download/GONI/goni_startup.log
```

Decisões (contratos em `Diagnostics.hpp`):

1. **Cópia continuamente atualizada**: cada estágio persistido dispara a
   reescrita da cópia pública COMPLETA — o último estágio concluído antes
   de uma morte súbita permanece visível. Se o espelho de um estágio
   falhar (volume não pronto), o próximo estágio tenta de novo
   (auto-cura; evidência: primeiras mirrors falharam com "Volume
   external_primary not found" ~1-2min pós-boot no ATD e as seguintes
   recuperaram).
2. **Crash log: export na execução SEGUINTE**. O signal handler
   (P3.1) continua gravando APENAS via `write(2)` no arquivo privado —
   nenhuma operação complexa em contexto de sinal. No próximo início, o
   `onCreate` exporta `goni_crash.log` ANTES de qualquer carga. Em um
   crash-loop, a execução N sempre exporta o crash da N-1 antes de morrer.
3. **Janela pré-diagnóstico coberta**: se `libgoni.so` nem carregar
   (dlopen/`UnsatisfiedLinkError`/`ExceptionInInitializerError`), o
   `catch` registra causa + stack completa em Java puro e espelha — a
   saída continua visível (toast + `finish()`), o crash não é engolido.
4. **Sem permissões no Android 10+**: `MediaStore.Downloads` +
   `RELATIVE_PATH=Download/GONI` (scoped storage; o app contribui com os
   PRÓPRIOS arquivos). Android 9-: escrita direta em
   `Environment.DIRECTORY_DOWNLOADS` (WRITE_EXTERNAL_STORAGE
   maxSdkVersion=28) com fallback app-external.
5. **Best-effort total**: falha de espelho nunca derruba o app (exceções
   engolidas nos dois lados da fronteira JNI; trampoline limpa pending
   exceptions).

## 2. Bugs reais encontrados e corrigidos (pela própria validação)

| # | Bug | Evidência | Correção |
|---|-----|-----------|----------|
| 1 | `hasPreviousCrashReport()` do P3.1 usava `stat size > 0`, mas o handler grava uma nota benigna `[handler] instalado` em TODO init → "crash anterior" (diálogo + export) dispararia em TODA execução após a primeira instalação, mesmo sem crash | teste Linux + análise do fluxo | gatilho agora é linha `[crash]` real escrita pelo signal handler; testes positivo (crash real → true) e negativo (nota sozinha → false, probe em processo filho) |
| 2 | MediaStore reescreve o nome pelo MIME: `goni_startup.log` + `text/plain` → `goni_startup.log.txt`; a busca por nome exato nunca achava a própria linha → CADA estágio INSERIA uma nova → **22 duplicatas** `goni_startup.log (N).txt` no backing store | probe no ATD (evidência em `download/p32/`) | MIME `application/octet-stream` (nome preservado) + busca por PREFIXO de nome + dono |
| 3 | `RELATIVE_PATH` é gravado NORMALIZADO (`Download/GONI/` com barra final): a linha criada pelo processo anterior nunca era achada no relaunch → nascia `goni_startup (1).log` na 2ª execução | contagem pós-relaunch no backing store | casamento por OWNER + prefixo de nome (imune à normalização) + limpeza de duplicatas próprias + cache do Uri no processo |

## 3. Verificação automatizada — `scripts/p32_verify_export.sh`

Um comando por ciclo (cold boot → espera do volume → install → launch →
4 testes → teardown graceful). Evidências + verdict em
`/home/z/my-project/download/p32/export_<stamp>/`.

Resultado final (APK `sha256 5b9eb045…`, ATD API 31 x86_64, TCG):

```
verdict: PASS
mediastore_row: 1            ← linha no MediaStore com owner do app
backing_stages: 1            ← bytes REAIS em /data/media/0/Download/GONI/
no_duplicates: 1 (files=1)   ← UM goni_startup.log após 2 execuções + relaunch
crash_export_next_launch: 1  ← crash injetado exportado na execução seguinte
process: ALIVE
mirror_logcat_lines: 23      ← espelhos com sucesso
mirror_errors: 0
```

Conteúdo verificado do `Download/GONI/goni_startup.log` (backing store):

```
[session] begin pid=1610 time=1789857792
STARTUP_NATIVE_LIBRARY ok libgoni.so loaded
STARTUP_JNI ok /data/user/0/com.goni.runtime/files
STARTUP_APPLICATION ok process
STARTUP_ACTIVITY ok EditorActivity
STARTUP_EDITOR_HOST begin
STARTUP_FILESYSTEM ok /data/user/0/com.goni.runtime/files/projects
STARTUP_EDITOR_DOCUMENT ok
STARTUP_EDITOR_UI ok UI construída
STARTUP_PROJECT ok MeuJogo
STARTUP_MATERIAL ok MeuJogo
```

O ciclo P3.1 (`p31_emu_cycle.sh`, mesmo APK) também passou:
processo vivo na janela de observação, todos os estágios até MATERIAL —
sem regressão do runtime Android (uma execução intermédio morreu de
morte AMBIENTAL do ATD/TCG documentada — DeadSystemException no
com.android.providers.media.module, não no app).

## 4. Limitações

- **Visibilidade no emulador ATD**: o shell não atravessa o FUSE de
  /sdcard (Permission denied — particularidade da imagem); a verificação
  usa o MediaStore (o que gerenciadores de arquivos/MTP consultam) e o
  backing store via `adb root`. Em dispositivos reais, gerenciadores e
  MTP leem via MediaProvider.
- **Linhas órfãs de desinstalação**: após desinstalar/reinstalar, as
  linhas antigas ficam com `owner_package_name=NULL` (não são mais
  nossas); o app cria a própria linha e o MediaStore deduplica com
  sufixo. As linhas órfãs só são visíveis se o usuário NÃO apagar
  Downloads/GONI manualmente entre reinstalações.
- **Morte sem sinal** (SIGKILL/OOM-kill): nenhum handler é chamado — não
  há linha `[crash]`; o goni_startup.log público ainda mostra o último
  estágio concluído.
- **Espelho síncrono na UI thread**: cada estágio faz uma reescrita
  ContentResolver de poucos KB (23 espelhos, 0 erros, custo imperceptível
  — medido). Escolha deliberada: ordenação garantida do último estágio
  prevalece sobre assincronismo.
- O `goni_startup.log` público é o conteúdo CUMULATIVO do privado
  (todas as sessões) — por design (evidência entre execuções).

## 5. Como o usuário encontra os arquivos no Realme C33 (Android 12)

1. Abra o aplicativo **Arquivos** (gerenciador de arquivos do sistema,
   realmeUI) ou qualquer gerenciador instalado;
2. Navegue até **Downloads → GONI** (ou Armazenamento interno →
   Download → GONI);
3. Os arquivos `goni_startup.log` e `goni_crash.log` podem ser abertos
   com qualquer visualizador de texto ou compartilhados (ex.: para o
   e-mail/WhatsApp do desenvolvedor);
4. Alternativa por cabo USB (MTP): conecte ao PC → explorador de
   arquivos → armazenamento do telefone → pasta **Download/GONI**.

Nenhuma permissão precisa ser concedida — os arquivos são criados pelo
próprio aplicativo via MediaStore (scoped storage, compatível com
Android 12/13).
