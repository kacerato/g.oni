/// Diagnostics.cpp — P3.1→P3.5: implementação (ver Diagnostics.hpp).
///
/// Escrita de arquivo: cada linha é fprintf + fflush + fsync — o custo por
/// estágio é irrelevante (dezenas de eventos por processo) e o benefício é
/// total: evidência sobrevive à morte súbita.
///
/// Mapa das mudanças nesta revisão:
///  - mark(): thread-safe, timestamps duplos (wallclock+monotonic ms);
///  - espelho: FILA + thread de despacho dedicada (nenhuma thread que
///    marca toca JNI/MediaStore — T0/T1);
///  - watchdog (T3): arm/heartbeat/evaluate + poke SIGUSR1;
///  - forense (T3b): dump de TODAS as threads, [fp]+[scan], maps cru
///    verbatim, si_pid/gettid — tudo com open/read/write/getdents64.

#include "eng/editor/Diagnostics.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "eng/log/Macros.hpp"

#include <ucontext.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace eng::editor::diag {
namespace {

ENG_LOG_CATEGORY("editor.diag");

constexpr std::size_t kMaxPath = 288;
constexpr std::size_t kMaxStage = 64;
constexpr std::size_t kMaxDetail = 160;

/// Relógios (CLOCK_MONOTONIC é imune a ajustes de hora do usuário —
/// deltas entre micro-marks são confiáveis; wallclock correlaciona com
/// logcat/tombstones).
std::int64_t monoMs() noexcept
{
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 +
           static_cast<std::int64_t>(ts.tv_nsec) / 1000000;
}
std::int64_t wallMs() noexcept
{
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 +
           static_cast<std::int64_t>(ts.tv_nsec) / 1000000;
}

// ---------------------------------------------------------------------------
// Estado do tracer (P3.1; mutex desde P3.5 — mark é thread-safe)
// ---------------------------------------------------------------------------

struct TracerState {
    std::mutex mutex{};
    bool initialized{false};
    char dir[kMaxPath]{};
    char startupPath[kMaxPath]{};
    char crashPath[kMaxPath]{};
    /// último estágio (lido pelo crash handler — best-effort)
    char lastStage[kMaxStage]{'-'};
    char lastDetail[kMaxDetail]{};
    std::FILE* startupFile{nullptr};  ///< append stream (flush+fsync por linha)
};

TracerState& tracer() {
    static TracerState state;
    return state;
}

// ---------------------------------------------------------------------------
// Fila do espelho: UMA thread de despacho, SEM JNI na
// thread que marcou.
//
// Por que: o retry de áudio e qualquer thread nativa futura podem
// chamar mark(); o trampoline JNI do espelho exigiria AttachCurrentThread
// (T0). A fila isola: marks de QUALQUER thread apenas empurram um ticket;
// a thread de despacho (criada no primeiro setMirrorCallback com callback
// != null) drena e invoca o callback — o trampoline faz o attach UMA vez
// por thread (RAII thread_local) e o Kotlin apenas enfileira o I/O pesado
// (T1). O estado é um singleton "leaked" intencional: process-lifetime —
// destruição em atexit disputaria com a thread de despacho (UB).
// ---------------------------------------------------------------------------

struct MirrorState {
    std::mutex mutex{};
    std::condition_variable cv{};
    std::uint64_t enqueued{0};   ///< pedidos produzidos (por mark/request)
    std::uint64_t drained{0};    ///< "snapshot de enqueued" do último lote drenado
    bool inFlight{false};         ///< um callback está executando AGORA
    MirrorCallback callback{nullptr};
    void* userdata{nullptr};
    std::thread dispatchThread{};
    bool threadStarted{false};
};

MirrorState& mirror() {
    static MirrorState* state = new MirrorState{};  // leak intencional
    return *state;
}

void mirrorDispatchLoop() {
    MirrorState& m = mirror();
    for (;;) {
        std::unique_lock<std::mutex> lock{m.mutex};
        m.cv.wait(lock, [&m] { return m.drained < m.enqueued; });
        // Coalescing do LOTE: o espelho reescreve o arquivo COMPLETO —
        // pedidos acumulados desde o último drain são atendidos por UM
        // callback (o Kotlin coalesca de novo no MediaStore; a cópia
        // pública final sempre reflete o último estágio persistido).
        m.drained = m.enqueued;
        m.inFlight = true;
        const MirrorCallback cb = m.callback;
        void* const ud = m.userdata;
        lock.unlock();
        if (cb != nullptr) {
            cb(ud);  // contrato: nunca lança, nunca chama mark/init
        }
        {
            std::lock_guard<std::mutex> done{m.mutex};
            m.inFlight = false;
        }
        m.cv.notify_all();  // acorda waitMirrorIdle
    }
}

void ensureMirrorThread() {
    MirrorState& m = mirror();
    std::lock_guard<std::mutex> lock{m.mutex};
    if (!m.threadStarted) {
        m.dispatchThread = std::thread{&mirrorDispatchLoop};
        m.dispatchThread.detach();  // process-lifetime (ver MirrorState)
        m.threadStarted = true;
    }
}

/// Enfileira um pedido de espelho (chamado por mark/requestMirror —
/// qualquer thread, nunca em signal handler).
void enqueueMirror() {
    MirrorState& m = mirror();
    {
        std::lock_guard<std::mutex> lock{m.mutex};
        ++m.enqueued;
    }
    m.cv.notify_one();
}

// ---------------------------------------------------------------------------
// Globais do crash handler (P3.1; si_pid/tid desde P3.5)
// ---------------------------------------------------------------------------

int g_crashFd = -1;        ///< fd de goni_crash.log (aberto no install)
char g_crashPath[288]{};    ///< path p/ re-abrir o fd DENTRO do handler

struct CrashGlobals {
    /// Mark() é chamado de QUALQUER thread (retry de áudio) — as
    /// escritas do estágio último são serializadas. O crash handler lê
    /// SEM lock (contexto de sinal — locks proibidos): leitura possivel-
    /// mente tornada é o trade-off documentado desde o P3.1 (evidência
    //  best-effort de um processo morrendo).
    std::mutex mutex{};
    char lastStage[kMaxStage]{'-'};
    char lastDetail[kMaxDetail]{};
};
CrashGlobals& crashGlobals() {
    static CrashGlobals g;
    return g;
}

// ---------------------------------------------------------------------------
// P3.3/P3.5 — snapshot de /proc/self/maps (sem dladdr: os locks do
// dynamic linker deadlockam em crash durante dlopen). P3.5 acrescenta a
// flag EXECUTÁVEL (filtrar candidatos do [scan]) e o dump CRU verbatim.
// ---------------------------------------------------------------------------

struct MapEntry {
    std::uintptr_t start{0};
    std::uintptr_t end{0};
    bool readable{false};
    bool executable{false};
    char path[120]{};  ///< truncado se maior (suficiente p/ identificar)
};

constexpr std::size_t kMaxMapEntries = 2048;
MapEntry g_mapEntries[kMaxMapEntries]{};
std::size_t g_mapEntryCount = 0;
bool g_mapEntriesTruncated = false;

/// hex sem 0x (formato de /proc/self/maps) — avança p.
bool parseHexField(const char*& p, std::uintptr_t& value) {
    value = 0;
    bool any = false;
    while (*p != '\0') {
        const char c = *p;
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else {
            break;
        }
        value = (value << 4) | static_cast<std::uintptr_t>(digit);
        any = true;
        ++p;
    }
    return any;
}

/// Parse de UMA linha de maps (já NUL-terminada). Formato:
/// start-end perms offset dev inode path...
bool parseMapsLine(const char* line, MapEntry& out) {
    const char* p = line;
    if (!parseHexField(p, out.start) || *p != '-') {
        return false;
    }
    ++p;
    if (!parseHexField(p, out.end) || out.end <= out.start) {
        return false;
    }
    while (*p == ' ') {
        ++p;
    }
    // perms: rwx[spl]
    out.readable = p[0] == 'r';
    out.executable = p[2] == 'x';
    // pula até o 6º campo (path): perms, offset, dev, inode já contam 4.
    int spaces = 0;
    while (*p != '\0' && spaces < 5) {
        if (*p == ' ') {
            ++spaces;
            while (*p == ' ') {
                ++p;
            }
        } else {
            ++p;
        }
    }
    if (spaces < 5) {
        out.path[0] = '\0';  // linha sem path (anon) — ainda válida
    }
    std::size_t i = 0;
    while (p[i] != '\0' && p[i] != '\n' && i + 1 < sizeof out.path) {
        out.path[i] = p[i];
        ++i;
    }
    out.path[i] = '\0';
    return true;
}

void loadMapsSnapshot() {
    g_mapEntryCount = 0;
    g_mapEntriesTruncated = false;
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    char buf[4096];
    char carry[256];
    std::size_t carryLen = 0;
    std::size_t totalRead = 0;
    while (g_mapEntryCount < kMaxMapEntries && totalRead < (2u << 20)) {
        const auto got = ::read(fd, buf, sizeof buf);
        if (got <= 0) {
            break;
        }
        totalRead += static_cast<std::size_t>(got);
        std::size_t begin = 0;
        std::size_t pos = 0;
        while (pos < static_cast<std::size_t>(got) &&
               g_mapEntryCount < kMaxMapEntries) {
            if (buf[pos] == '\n') {
                // linha = carry + buf[begin, pos)
                if (carryLen + (pos - begin) + 1 <= sizeof carry) {
                    std::memcpy(carry + carryLen, buf + begin, pos - begin);
                    carry[carryLen + (pos - begin)] = '\0';
                    if (parseMapsLine(carry,
                                      g_mapEntries[g_mapEntryCount])) {
                        ++g_mapEntryCount;
                    }
                }
                carryLen = 0;
                begin = pos + 1;
            }
            ++pos;
        }
        // resto parcial → carry
        const std::size_t rest = static_cast<std::size_t>(got) - begin;
        if (rest > 0 && carryLen + rest < sizeof carry) {
            std::memcpy(carry + carryLen, buf + begin, rest);
            carryLen += rest;
        } else {
            carryLen = 0;  // linha gigante: descarta (não mapeia libs)
        }
    }
    ::close(fd);
    if (g_mapEntryCount >= kMaxMapEntries) {
        g_mapEntriesTruncated = true;
    }
}

const MapEntry* findMapFor(std::uintptr_t address) {
    for (std::size_t i = 0; i < g_mapEntryCount; ++i) {
        if (address >= g_mapEntries[i].start &&
            address < g_mapEntries[i].end) {
            return &g_mapEntries[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Escrita de linhas no fd do crash log (UMA write() atômica por linha —
// O_APPEND; handlers de threads diferentes podem intercalar LINHAS,
// nunca corromper uma linha).
// ---------------------------------------------------------------------------

/// Escreve "[tag] 0xADDR module=<path|?> base=0xB off=0xD" no fd.
void writeModuleLine(int fd, const char* tag, std::uintptr_t address) {
    const MapEntry* m = findMapFor(address);
    char line[320];
    int n;
    if (m != nullptr) {
        n = std::snprintf(line, sizeof line,
                          "[%s] 0x%llx module=%s base=0x%llx off=0x%llx\n",
                          tag, static_cast<unsigned long long>(address),
                          m->path[0] != '\0' ? m->path : "anon",
                          static_cast<unsigned long long>(m->start),
                          static_cast<unsigned long long>(address - m->start));
    } else {
        n = std::snprintf(line, sizeof line,
                          "[%s] 0x%llx module=? (fora do snapshot de maps%s)\n",
                          tag, static_cast<unsigned long long>(address),
                          g_mapEntriesTruncated
                              ? " — snapshot TRUNCADO"
                              : "");
    }
    if (n > 0) {
        const auto written =
            ::write(fd, line, static_cast<std::size_t>(n));
        (void)written;
    }
}

/// Uma linha formatada qualquer (uma write; snprintf é o mesmo trade-off
/// documentado desde P3.3 — prática-padrão de handlers no Android).
void writeLine(int fd, const char* text) {
    if (text != nullptr && text[0] != '\0') {
        const auto written = ::write(fd, text, std::strlen(text));
        (void)written;
    }
}

/// Frame pointer + PC + SP do contexto interrompido (arm64/x86_64).
struct FaultFrame {
    std::uintptr_t pc{0};
    std::uintptr_t fp{0};
    std::uintptr_t sp{0};
};
FaultFrame faultFrameOf(const void* context) {
    FaultFrame f;
    if (context == nullptr) {
        return f;
    }
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__aarch64__)
    f.pc = static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
    f.fp = static_cast<std::uintptr_t>(uc->uc_mcontext.regs[29]);
    f.sp = static_cast<std::uintptr_t>(uc->uc_mcontext.sp);
#elif defined(__x86_64__)
    f.pc = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    f.fp = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
    f.sp = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
#else
    (void)uc;
#endif
    return f;
}

/// [fp]: frame-pointer walk (cada frame validado contra o
/// snapshot de maps; formato module= base= off= para symbolização
/// offline contra o libgoni.so NÃO-STRIPPED da build exata).
void writeFpWalk(int fd, std::uintptr_t fp) {
    std::uintptr_t frame = fp;
    for (int i = 0; i < 24 && frame != 0; ++i) {
        if ((frame & 0xf) != 0 || frame < 0x1000 ||
            frame >= 0x800000000000ULL) {
            break;  // não alinhado/fora de usuário: cadeia quebrada
        }
        const MapEntry* m = findMapFor(frame);
        if (m == nullptr || !m->readable) {
            break;  // FP não aponta p/ memória legível: para AQUI
        }
        // [frame] = próximo FP; [frame+8] = endereço de retorno.
        const auto* slots =
            reinterpret_cast<const std::uintptr_t*>(frame);
        const std::uintptr_t next = slots[0];
        const std::uintptr_t ret = slots[1];
        char line[288];
        const MapEntry* rm = findMapFor(ret);
        int n;
        if (rm != nullptr) {
            n = std::snprintf(
                line, sizeof line, "[fp] %d 0x%llx module=%s base=0x%llx off=0x%llx\n",
                i, static_cast<unsigned long long>(ret),
                rm->path[0] != '\0' ? rm->path : "anon",
                static_cast<unsigned long long>(rm->start),
                static_cast<unsigned long long>(ret - rm->start));
        } else {
            n = std::snprintf(line, sizeof line, "[fp] %d 0x%llx ?\n", i,
                              static_cast<unsigned long long>(ret));
        }
        if (n > 0) {
            const auto written =
                ::write(fd, line, static_cast<std::size_t>(n));
            (void)written;
        }
        if (next <= frame) {
            break;  // cadeia deve SUBIR na pilha
        }
        frame = next;
    }
}

/// [scan]: caminhamento heurístico da pilha a partir da SP —
/// todo uintptr que cai em mapeamento EXECUTÁVEL é reportado como
/// candidato a endereço de código (cross-check offline contra o [fp];
/// captura frames que o fp-walk perde em binários de terceiro).
void writeScanWalk(int fd, std::uintptr_t sp) {
    if (sp == 0 || (sp & 0x7) != 0 || sp < 0x1000 ||
        sp >= 0x800000000000ULL) {
        return;
    }
    constexpr std::uintptr_t kMaxScanBytes = 256 * 1024;
    constexpr int kMaxCandidates = 64;
    int count = 0;
    char line[288];
    for (std::uintptr_t p = sp; p < sp + kMaxScanBytes && count < kMaxCandidates;
         p += sizeof(std::uintptr_t)) {
        const MapEntry* m = findMapFor(p);
        if (m == nullptr || !m->readable) {
            break;  // saiu da região de pilha legível
        }
        const std::uintptr_t value =
            *reinterpret_cast<const std::uintptr_t*>(p);
        const MapEntry* vm = findMapFor(value);
        if (vm != nullptr && vm->executable) {
            const int n = std::snprintf(
                line, sizeof line,
                "[scan] %d 0x%llx module=%s base=0x%llx off=0x%llx\n", count,
                static_cast<unsigned long long>(value),
                vm->path[0] != '\0' ? vm->path : "anon",
                static_cast<unsigned long long>(vm->start),
                static_cast<unsigned long long>(value - vm->start));
            if (n > 0) {
                const auto written =
                    ::write(fd, line, static_cast<std::size_t>(n));
                (void)written;
            }
            ++count;
        }
    }
}

// ---------------------------------------------------------------------------
// Dump forense COMPLETO de uma thread (contexto entregue ao
// handler) + TODAS as threads do processo + maps CRU.
// ---------------------------------------------------------------------------

/// Dump do CONTEXTO da própria thread sinalizada (pc/pilhas [fp]+[scan]).
void writeSelfDump(int fd, const char* tag, const siginfo_t* info,
                   void* context) {
    const FaultFrame frame = faultFrameOf(context);
    char header[192];
    const int n = std::snprintf(
        header, sizeof header,
        "[dump] tag=%s tid=%d signal=%d code=%d si_pid=%d\n",
        tag, static_cast<int>(::gettid()),
        info != nullptr ? info->si_signo : 0,
        info != nullptr ? info->si_code : 0,
        info != nullptr ? static_cast<int>(info->si_pid) : 0);
    if (n > 0) {
        const auto written = ::write(fd, header, static_cast<std::size_t>(n));
        (void)written;
    }
    if (frame.pc != 0) {
        writeModuleLine(fd, "pc", frame.pc);
    }
    if (frame.fp != 0) {
        writeFpWalk(fd, frame.fp);
    }
    if (frame.sp != 0) {
        writeScanWalk(fd, frame.sp);
    }
}

/// Entrada de diretório do getdents64 (layout estável do kernel —
/// declarado à mão: incluir <linux/dirent.h> conflita com <dirent.h>).
struct KernelDirent64 {
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

/// comm de um tid ("/proc/self/task/<tid>/comm" — 15 chars + NUL).
void readCommOf(pid_t tid, char* out, std::size_t cap) {
    out[0] = '?';
    out[1] = '\0';
    if (cap < 2) {
        return;
    }
    char path[64];
    const int n = std::snprintf(path, sizeof path,
                                "/proc/self/task/%d/comm",
                                static_cast<int>(tid));
    if (n <= 0) {
        return;
    }
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    const auto got = ::read(fd, out, cap - 1);
    ::close(fd);
    if (got <= 0) {
        return;
    }
    out[static_cast<std::size_t>(got) > cap - 1 ? cap - 1
                                               : static_cast<std::size_t>(got)] = '\0';
    // remove \n final
    for (std::size_t i = 0; out[i] != '\0'; ++i) {
        if (out[i] == '\n') {
            out[i] = '\0';
            break;
        }
    }
}

/// Enumera tids via getdents64 (async-signal-safe; opendir usaria malloc)
/// e envia SIGUSR2 a cada OUTRA thread — cada uma despeja a si mesma no
/// goni_crash.log (seção [dump] tag=other). Fire-and-forget: thread em
/// sono não interrompível simplesmente não responde (documentado).
/// NOTA UBSan: os registros do getdents64 NÃO são alinhados — os campos
/// são lidos por memcpy (acesso desalinhado direto é UB e o linux-debug
/// compila este TU com -fsanitize=undefined).
void pokeAllOtherThreads(int fd, pid_t selfTid) {
    const int dfd = ::open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return;
    }
    char buf[4096];
    for (;;) {
        const long got = ::syscall(SYS_getdents64, dfd, buf, sizeof buf);
        if (got <= 0) {
            break;
        }
        long off = 0;
        while (off < got) {
            unsigned short reclen = 0;
            std::memcpy(&reclen,
                        buf + off + offsetof(KernelDirent64, d_reclen),
                        sizeof reclen);
            const std::size_t kMinRecord = offsetof(KernelDirent64, d_name) + 2;
            if (reclen < kMinRecord || off + reclen > got) {
                break;  // registro truncado/inválido: fim da varredura
            }
            const char* name = buf + off + offsetof(KernelDirent64, d_name);
            off += reclen;
            // d_name = tid decimal
            long tid = 0;
            bool valid = name[0] >= '0' && name[0] <= '9';
            for (const char* c = name; *c != '\0'; ++c) {
                if (*c < '0' || *c > '9') {
                    valid = false;
                    break;
                }
                tid = tid * 10 + (*c - '0');
                if (tid > 1000000) {
                    valid = false;
                    break;
                }
            }
            if (!valid || tid <= 0 || static_cast<pid_t>(tid) == selfTid) {
                continue;
            }
            char comm[32];
            readCommOf(static_cast<pid_t>(tid), comm, sizeof comm);
            char line[128];
            const int n = std::snprintf(line, sizeof line,
                                       "[thread] tid=%ld comm=%s (ping SIGUSR2)\n",
                                       tid, comm);
            if (n > 0) {
                const auto written = ::write(fd, line,
                                             static_cast<std::size_t>(n));
                (void)written;
            }
            ::syscall(SYS_tgkill, ::getpid(), static_cast<pid_t>(tid), SIGUSR2);
        }
    }
    ::close(dfd);
}

/// Maps CRU verbatim (P3.5: valida OFFLINE a atribuição módulo/base do
/// parser contra a build exata — até 4096 entradas / 256 KiB).
void writeRawMaps(int fd) {
    writeLine(fd, "[maps.raw begin]\n");
    const int mfd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (mfd >= 0) {
        char buf[4096];
        std::size_t total = 0;
        constexpr std::size_t kCap = 256 * 1024;
        while (total < kCap) {
            const auto got = ::read(mfd, buf, sizeof buf);
            if (got <= 0) {
                break;
            }
            total += static_cast<std::size_t>(got);
            const auto written = ::write(fd, buf, static_cast<std::size_t>(got));
            (void)written;
        }
        ::close(mfd);
        if (total >= 256 * 1024) {
            writeLine(fd, "[maps.raw truncated]\n");
        }
    } else {
        writeLine(fd, "[maps.raw unavailable]\n");
    }
    writeLine(fd, "[maps.raw end]\n");
}

/// Garante um fd válido para o crash log dentro do handler (reciclagem de
/// descritores de terceiros — self-healing desde P3.1).
int ensureCrashFd() {
    if (g_crashFd < 0 && g_crashPath[0] != '\0') {
        g_crashFd = ::open(g_crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    return g_crashFd;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

struct sigaction_restore {
    bool installed = false;
    struct sigaction previous{};
};

// fatais: SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE (índices 0..4)
// diagnósticos: SIGUSR1 (watchdog, índice 5) / SIGUSR2 (dump other, 6)
sigaction_restore g_handlers[7];
constexpr int kHandledSignals[] = {SIGSEGV, SIGABRT, SIGBUS,
                                    SIGILL, SIGFPE, SIGUSR1, SIGUSR2};
constexpr int kIdxSegv = 0, kIdxAbrt = 1, kIdxBus = 2, kIdxIll = 3,
              kIdxFpe = 4, kIdxUsr1 = 5, kIdxUsr2 = 6;
static_assert(sizeof g_handlers / sizeof g_handlers[0] ==
              sizeof kHandledSignals / sizeof kHandledSignals[0]);

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    default: return "SIG?";
    }
}

void faultAddressString(const siginfo_t* info, const void* context,
                        char* out, std::size_t cap) {
    std::snprintf(out, cap, "%p",
                 info != nullptr ? info->si_addr : nullptr);
#if defined(__x86_64__)
    if (const auto* uc = static_cast<const ucontext_t*>(context); uc != nullptr) {
        std::snprintf(out, cap, "%p (pc=%p)", info != nullptr ? info->si_addr : nullptr,
                      reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]));
    }
#elif defined(__aarch64__)
    if (const auto* uc = static_cast<const ucontext_t*>(context); uc != nullptr) {
        std::snprintf(out, cap, "%p (pc=%p)", info != nullptr ? info->si_addr : nullptr,
                      reinterpret_cast<void*>(uc->uc_mcontext.pc));
    }
#else
    (void)context;
#endif
}

/// Encadeia ao handler anterior registrado (fatais: debuggerd/ART/tombone
/// seguem funcionando — o crash NUNCA é mascarado).
void chainToPrevious(int idx, int sig, siginfo_t* info, void* context) {
    if (g_handlers[idx].installed) {
        const struct sigaction& prev = g_handlers[idx].previous;
        const bool siginfo = (prev.sa_flags & SA_SIGINFO) != 0;
        const auto action = siginfo
            ? reinterpret_cast<std::uintptr_t>(prev.sa_sigaction)
            : reinterpret_cast<std::uintptr_t>(prev.sa_handler);
        const auto dfl = reinterpret_cast<std::uintptr_t>(SIG_DFL);
        const auto ign = reinterpret_cast<std::uintptr_t>(SIG_IGN);
        const auto err = reinterpret_cast<std::uintptr_t>(SIG_ERR);
        if (action != 0 && action != dfl && action != ign && action != err) {
            if (siginfo) {
                prev.sa_sigaction(sig, info, context);
            } else {
                prev.sa_handler(sig);
            }
            return;
        }
    }
    // Sem handler anterior útil: restaura o default e re-entrega — o
    // processo morre COM o sinal original (nada mascarado).
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

/// Handler FATAL: dump forense COMPLETO (self + todas as threads + maps
/// cru) e encadeamento ao anterior.
void crashHandler(int sig, siginfo_t* info, void* context) {
    const int fd = ensureCrashFd();
    if (fd >= 0) {
        char addr[64];
        faultAddressString(info, context, addr, sizeof addr);
        const CrashGlobals& g = crashGlobals();
        char line[kMaxStage + kMaxDetail * 2 + 192];
        int n = std::snprintf(
            line, sizeof line,
            "[crash] signal=%s(%d) code=%d addr=%s stage=%s detail=%s "
            "si_pid=%d tid=%d\n",
            signalName(sig), sig,
            info != nullptr ? info->si_code : 0, addr,
            g.lastStage, g.lastDetail,
            info != nullptr ? static_cast<int>(info->si_pid) : 0,
            static_cast<int>(::gettid()));
        if (n > 0) {
            const auto written =
                ::write(fd, line, static_cast<std::size_t>(n));
            (void)written;
        }
        // Forense P3.3+P3.5: módulo do pc/alvo, pilhas [fp]+[scan] da
        // thread sinalizada, TODAS as outras threads (SIGUSR2 → cada
        // uma despeja a si mesma) e maps cru verbatim.
        loadMapsSnapshot();
        writeSelfDump(fd, "fatal", info, context);
        if (info != nullptr &&
            reinterpret_cast<std::uintptr_t>(info->si_addr) != 0) {
            writeModuleLine(
                fd, "fault.addr",
                reinterpret_cast<std::uintptr_t>(info->si_addr));
        }
        writeLine(fd, "[threads begin]\n");
        pokeAllOtherThreads(fd, ::gettid());
        // janela para as outras threads despejarem (fire-and-forget:
        // quem não responde, não bloqueia a morte).
        struct timespec pause{0, 50 * 1000 * 1000};
        ::nanosleep(&pause, nullptr);
        writeLine(fd, "[threads end]\n");
        writeRawMaps(fd);
        (void)::fsync(fd);
    }
    const int idx = sig == SIGSEGV ? kIdxSegv
                 : sig == SIGABRT ? kIdxAbrt
                 : sig == SIGBUS  ? kIdxBus
                 : sig == SIGILL  ? kIdxIll
                                  : kIdxFpe;
    chainToPrevious(idx, sig, info, context);
}

/// Sentinel do poke do watchdog: distingue NOSSO SIGUSR1 (dump)
/// do SIGUSR1 do ART (suspensão de GC — DEVE chegar ao handler do ART).
std::atomic<pid_t> g_pendingWatchdogTid{-1};

/// Handler do SIGUSR1 (watchdog): quando o poke é NOSSO (sentinel casa
/// com o tid desta thread), despeja o contexto EXATO onde a MAIN está
/// presa e RETORNA — o processo continua vivo (hang diagnosticado, não
/// executado). Quando NÃO é nosso: encadeia ao handler anterior (o
/// suspend do ART) ou apenas engole (nunca mata por sinal diagnóstico).
void watchdogSignalHandler(int sig, siginfo_t* info, void* context) {
    const pid_t self = ::gettid();
    if (g_pendingWatchdogTid.load(std::memory_order_acquire) == self) {
        g_pendingWatchdogTid.store(-1, std::memory_order_release);
        const int fd = ensureCrashFd();
        if (fd >= 0) {
            loadMapsSnapshot();
            writeSelfDump(fd, "watchdog", info, context);
            writeLine(fd, "[threads begin]\n");
            pokeAllOtherThreads(fd, self);
            struct timespec pause{0, 50 * 1000 * 1000};
            ::nanosleep(&pause, nullptr);
            writeLine(fd, "[threads end]\n");
            writeRawMaps(fd);
            (void)::fsync(fd);
        }
        return;  // diagnóstico: o processo SEGUE VIVO
    }
    // Não é nosso: o ART usa SIGUSR1 para suspensão de GC — encadear é
    // OBRIGATÓRIO no Android. Sem handler real anterior (Linux/tests):
    // engole (sinal diagnóstico NUNCA mata o processo).
    if (g_handlers[kIdxUsr1].installed) {
        const struct sigaction& prev = g_handlers[kIdxUsr1].previous;
        const bool siginfo = (prev.sa_flags & SA_SIGINFO) != 0;
        const auto action = siginfo
            ? reinterpret_cast<std::uintptr_t>(prev.sa_sigaction)
            : reinterpret_cast<std::uintptr_t>(prev.sa_handler);
        const auto dfl = reinterpret_cast<std::uintptr_t>(SIG_DFL);
        const auto ign = reinterpret_cast<std::uintptr_t>(SIG_IGN);
        const auto err = reinterpret_cast<std::uintptr_t>(SIG_ERR);
        if (action != 0 && action != dfl && action != ign && action != err) {
            if (siginfo) {
                prev.sa_sigaction(sig, info, context);
            } else {
                prev.sa_handler(sig);
            }
        }
    }
}

/// Handler do SIGUSR2 (dump de OUTRA thread): despeja a si mesma com
/// tag=other e retorna. Sem sentinel: ninguém além do G.ONI envia
/// SIGUSR2 a si próprio aqui; um eventual disparo de terceiro produz um
/// dump extra (inofensivo) — nunca morte.
void otherThreadSignalHandler(int sig, siginfo_t* info, void* context) {
    (void)sig;
    const int fd = ensureCrashFd();
    if (fd >= 0) {
        loadMapsSnapshot();
        writeSelfDump(fd, "other", info, context);
    }
}

// ---------------------------------------------------------------------------
// Watchdog de hang da main thread
// ---------------------------------------------------------------------------

struct WatchdogState {
    std::atomic<bool> armed{false};
    pthread_t mainPthread{};
    pid_t mainTid{-1};
    std::atomic<std::int64_t> lastBeatMs{0};
    std::atomic<std::int64_t> thresholdMs{8000};
    std::atomic<std::int64_t> graceMs{15000};
    std::atomic<std::int64_t> armedAtMs{0};
};
WatchdogState& wd() {
    static WatchdogState* state = new WatchdogState{};  // leak intencional
    return *state;
}

/// Instala os handlers de sinal: fatais (reinstalação incondicional —
/// terceiros podem ter sobrescrito) + diagnósticos SIGUSR1/SIGUSR2.
void installAllSignalHandlers() {
    struct sigaction sa{};
    sa.sa_sigaction = &crashHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i <= kIdxFpe; ++i) {
        if (::sigaction(kHandledSignals[i], &sa,
                        &g_handlers[i].previous) == 0) {
            g_handlers[i].installed = true;
        }
    }
    // Diagnósticos: SEM SA_RESETHAND (podem disparar repetidas vezes) e
    // com SIGUSR1+SIGUSR2 mascarados durante o próprio dump.
    struct sigaction sd{};
    sd.sa_sigaction = &watchdogSignalHandler;
    sd.sa_flags = SA_SIGINFO;
    sigemptyset(&sd.sa_mask);
    sigaddset(&sd.sa_mask, SIGUSR1);
    sigaddset(&sd.sa_mask, SIGUSR2);
    if (::sigaction(SIGUSR1, &sd, &g_handlers[kIdxUsr1].previous) == 0) {
        g_handlers[kIdxUsr1].installed = true;
    }
    struct sigaction so{};
    so.sa_sigaction = &otherThreadSignalHandler;
    so.sa_flags = SA_SIGINFO;
    sigemptyset(&so.sa_mask);
    sigaddset(&so.sa_mask, SIGUSR1);
    sigaddset(&so.sa_mask, SIGUSR2);
    if (::sigaction(SIGUSR2, &so, &g_handlers[kIdxUsr2].previous) == 0) {
        g_handlers[kIdxUsr2].installed = true;
    }
}

void appendFileLine(const char* path, const char* text) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    if (int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644); fd >= 0) {
        const auto written = ::write(fd, text, std::strlen(text));
        const auto synced = ::fsync(fd);
        (void)written;
        (void)synced;
        ::close(fd);
    }
}

}  // namespace

// =============================================================================
// API
// =============================================================================

void init(const char* dir) {
    TracerState& t = tracer();
    const std::lock_guard<std::mutex> lock{t.mutex};
    if (t.initialized || dir == nullptr || dir[0] == '\0') {
        return;
    }
    std::snprintf(t.dir, sizeof t.dir, "%s", dir);
    std::snprintf(t.startupPath, sizeof t.startupPath, "%s/goni_startup.log",
                  dir);
    std::snprintf(t.crashPath, sizeof t.crashPath, "%s/goni_crash.log", dir);
    t.startupFile = std::fopen(t.startupPath, "a");
    if (t.startupFile == nullptr) {
        ENG_WARN("diag: não abriu {} (sem diagnóstico persistente)",
                 t.startupPath);
        return;  // sem arquivo — marks continuam no logcat apenas
    }
    t.initialized = true;
    // P3.1/P3.5: abre goni_crash.log (fd pré-aberto p/ o handler —
    // g_crashPath/g_crashFd) e instala TODOS os handlers (fatais +
    // diagnósticos SIGUSR1/SIGUSR2).
    installCrashHandler();

    // Header de sessão (separa execuções no arquivo cumulativo).
    char header[128];
    const long now = static_cast<long>(::time(nullptr));
    std::snprintf(header, sizeof header,
                  "[session] begin pid=%ld time=%ld\n",
                  static_cast<long>(::getpid()), now);
    (void)std::fputs(header, t.startupFile);
    std::fflush(t.startupFile);
    (void)::fsync(::fileno(t.startupFile));
    ENG_INFO("diag: startup tracing em {}", t.startupPath);

    // P3.2/P3.5: cópia pública da sessão ANTES de qualquer estágio —
    // enfileirada (a thread de despacho faz o I/O pesado; o init retorna
    // imediatamente).
    enqueueMirror();
}

void mark(const char* stage, const char* status, const char* detail) {
    if (stage == nullptr || stage[0] == '\0') {
        return;
    }
    const char* st = status != nullptr ? status : "ok";
    const char* dt = detail != nullptr ? detail : "";

    // Atualiza os globais ANTES de qualquer I/O: se o processo morrer
    // dentro do mark, o crash handler ainda vê o estágio que começou.
    {
        TracerState& t = tracer();
        const std::lock_guard<std::mutex> lock{t.mutex};
        std::snprintf(t.lastStage, sizeof t.lastStage, "%s", stage);
        std::snprintf(t.lastDetail, sizeof t.lastDetail, "%s", dt);
    }
    // cópia signal-reader-friendly (escrita serializada — T0/P3.5)
    {
        CrashGlobals& g = crashGlobals();
        const std::lock_guard<std::mutex> lock{g.mutex};
        std::snprintf(g.lastStage, sizeof g.lastStage, "%s", stage);
        std::snprintf(g.lastDetail, sizeof g.lastDetail, "%s", dt);
    }

    // Timestamps duplos — wallclock (correlação com logcat) +
    // monotônico (deltas confiáveis entre micro-marks; resolução ms).
    char line[kMaxStage + kMaxDetail + 160];
    const int n = std::snprintf(line, sizeof line,
                                "[wt=%lld mo=%lld] %s %s %s\n",
                                static_cast<long long>(wallMs()),
                                static_cast<long long>(monoMs()), stage,
                                st, dt);
    if (n <= 0) {
        return;
    }
    {
        TracerState& t = tracer();
        const std::lock_guard<std::mutex> lock{t.mutex};
        if (t.startupFile != nullptr) {
            (void)std::fputs(line, t.startupFile);
            std::fflush(t.startupFile);
            (void)::fsync(::fileno(t.startupFile));
        }
    }
    // Espelho VIA FILA — a thread que marcou NUNCA toca
    // JNI/MediaStore; a cópia pública é feita pela thread de despacho.
    enqueueMirror();
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "GONI", "[STARTUP] %s %s %s",
                        stage, st, dt);
#endif
    ENG_INFO("[STARTUP] {} {} {}", stage, st, dt);
}

const char* lastStage() noexcept {
    return tracer().lastStage;
}

void setMirrorCallback(MirrorCallback callback, void* userdata) {
    {
        MirrorState& m = mirror();
        std::lock_guard<std::mutex> lock{m.mutex};
        m.callback = callback;
        m.userdata = callback != nullptr ? userdata : nullptr;
        if (callback != nullptr) {
            // Pedidos feitos ENQUANTO ninguém escutava são órfãos do
            // listener ANTERIOR — o novo callback não os vê (o primeiro
            // lote dele nasce do próximo mark/requestMirror real).
            m.drained = m.enqueued;
        }
    }
    if (callback != nullptr) {
        ensureMirrorThread();
    }
}

void requestMirror() {
    enqueueMirror();
}

bool waitMirrorIdle(std::uint32_t timeoutMs) {
    MirrorState& m = mirror();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeoutMs};
    std::unique_lock<std::mutex> lock{m.mutex};
    if (!m.threadStarted) {
        return true;  // nada a drenar (sem thread: sem pedidos pendentes)
    }
    const bool done = m.cv.wait_until(
        lock, deadline,
        [&m] { return !m.inFlight && m.drained >= m.enqueued; });
    return done;
}

void installCrashHandler() {
    const TracerState& t = tracer();
    if (!t.initialized) {
        return;  // sem dir não há arquivo — não instala (no-op)
    }
    // O append em goni_crash.log precisa ser viável DE DENTRO do handler:
    // fd pré-aberto, somente write() (sem fopen dentro do signal).
    std::snprintf(g_crashPath, sizeof g_crashPath, "%s", t.crashPath);
    if (g_crashFd < 0) {
        g_crashFd = ::open(t.crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_crashFd < 0) {
            ENG_WARN("diag: crash log não abriu ({})", t.crashPath);
            return;
        }
        char note[128];
        const long now = static_cast<long>(::time(nullptr));
        std::snprintf(note, sizeof note,
                      "[handler] crash handler instalado time=%ld\n", now);
        appendFileLine(t.crashPath, note);
    }
    // (RE)instalação INCONDICIONAL: qualquer terceiro pode ter
    // sobrescrito os handlers (frameworks de teste, ART, libs gráficas).
    installAllSignalHandlers();
}

const char* startupLogPath() noexcept {
    const TracerState& t = tracer();
    return t.initialized ? t.startupPath : "-";
}

const char* crashLogPath() noexcept {
    const TracerState& t = tracer();
    return t.initialized ? t.crashPath : "-";
}

bool describeAddress(std::uintptr_t address, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = '\0';
    loadMapsSnapshot();
    const MapEntry* m = findMapFor(address);
    if (m == nullptr) {
        return false;
    }
    std::snprintf(out, cap, "%s+0x%llx", m->path[0] != '\0' ? m->path : "anon",
                  static_cast<unsigned long long>(address - m->start));
    return true;
}

bool hasPreviousCrashReport() {
    const TracerState& t = tracer();
    if (!t.initialized) {
        return false;
    }
    // A nota benigna "[handler] instalado" NÃO conta — um crash
    // real é uma linha "[crash] ..." escrita pelo signal handler.
    std::FILE* f = std::fopen(t.crashPath, "r");
    if (f == nullptr) {
        return false;
    }
    bool found = false;
    char line[256];
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::strncmp(line, "[crash]", 7) == 0) {
            found = true;
            break;
        }
    }
    std::fclose(f);
    return found;
}

// =============================================================================
// Watchdog
// =============================================================================

namespace watchdog {

void arm(std::int64_t thresholdMs, std::int64_t graceMs) {
    WatchdogState& w = wd();
    w.mainPthread = ::pthread_self();
    w.mainTid = ::gettid();
    w.thresholdMs.store(thresholdMs < 0 ? 8000 : thresholdMs);
    w.graceMs.store(graceMs < 0 ? 15000 : graceMs);
    const std::int64_t now = monoMs();
    w.armedAtMs.store(now);
    w.lastBeatMs.store(now);
    w.armed.store(true);
    // Handlers diagnósticos garantidos (init pode não ter rodado — ex.:
    // tests que armam direto).
    installAllSignalHandlers();
}

void heartbeat() noexcept {
    if (wd().armed.load(std::memory_order_relaxed)) {
        wd().lastBeatMs.store(monoMs(), std::memory_order_release);
    }
}

bool evaluate() noexcept {
    WatchdogState& w = wd();
    if (!w.armed.load(std::memory_order_relaxed)) {
        return false;
    }
    const std::int64_t now = monoMs();
    const std::int64_t last = w.lastBeatMs.load(std::memory_order_acquire);
    // Graça inicial: startup pesado (onCreate) antes do primeiro
    // heartbeat não pode disparar falso positivo.
    const std::int64_t threshold =
        (now - w.armedAtMs.load(std::memory_order_relaxed)) <
                w.graceMs.load(std::memory_order_relaxed)
            ? w.graceMs.load(std::memory_order_relaxed)
            : w.thresholdMs.load(std::memory_order_relaxed);
    if (now - last < threshold) {
        return false;
    }
    // HANG: uma arma por episódio (o próximo dispara após +threshold se
    // a main continuar muda — cada dump é evidência de um episódio).
    w.lastBeatMs.store(now, std::memory_order_release);
    const int fd = ensureCrashFd();
    if (fd >= 0) {
        char line[192];
        const int n = std::snprintf(
            line, sizeof line,
            "[watchdog] main tid=%d sem heartbeat por %lld ms "
            "(limite %lld ms) — SIGUSR1 para dump forense\n",
            static_cast<int>(w.mainTid),
            static_cast<long long>(now - last),
            static_cast<long long>(threshold));
        if (n > 0) {
            const auto written = ::write(fd, line,
                                         static_cast<std::size_t>(n));
            (void)written;
        }
        (void)::fsync(fd);
    }
    // Sentinel ANTES do poke: o handler casa o tid para saber que é
    // NOSSO sinal (o SIGUSR1 do ART segue fluindo pelo encadeamento).
    g_pendingWatchdogTid.store(w.mainTid, std::memory_order_release);
    (void)::pthread_kill(w.mainPthread, SIGUSR1);
    return true;
}

std::int64_t thresholdMs() noexcept { return wd().thresholdMs.load(); }
std::int64_t graceMs() noexcept { return wd().graceMs.load(); }

}  // namespace watchdog

}  // namespace eng::editor::diag
