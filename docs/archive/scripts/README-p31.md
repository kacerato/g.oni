# Scripts P3.1 — validação Android automatizada (ver docs/p31-android-validation.md)

- `p31_install_sdk.sh` — instala o Android SDK completo do zero (idempotente).
- `p31_boot_snapshot.sh` — boot completo do emulador + snapshot de referência
  (snapshot restore NÃO funciona sob TCG — documentado; ciclos usam cold boot).
- `p31_emu_cycle.sh <apk> [observe_s] [backend] [editor|runtime|settings]` —
  CICLO COMPLETO em um comando: cold boot (4 vCPU/2048MB + LD_PRELOAD do shim
  mmap) → install (3 tentativas) → launch → observação com pull incremental
  de goni_startup.log → coleta completa de evidências → verdict
  (PASS / FAIL_CRASH_GONI / FAIL_PROCESS_DEAD / ENV_SYSTEM_DEATH_*).
- `p31_mmap_norescue.c` — fonte do LD_PRELOAD que reexecuta a reserva de
  4GB do gfxstream/SwiftShader com MAP_NORESERVE (sem ele o emulator 37
  aborta com "Insufficient RAM free" em hosts sem swap/KVM).
  Build: `gcc -shared -fPIC -o mmap_norescue.so p31_mmap_norescue.c -ldl`

Variáveis: CYCLE_MEM (default 2048), CYCLE_CORES (default 4).
