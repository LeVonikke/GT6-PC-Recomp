#include "../runtime/syscalls/sys_ppu_thread.h"
#include "../runtime/ppu/ppu_context.h"
#include "cellGame.h"
#include "ps3emu/guest_call.h"

#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {
uint32_t ppu_load_elf(const char* path);
void ppu_recomp_register(void);
void ppu_hle_init(void);
void ppu_sysprx_register(void);
void lv2_init_syscalls(void);
int ppu_run(uint32_t entry_opd, uint32_t stack_top);
void gt6_register_hle(void);

uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size; // owned by ppu_loader.cpp
extern PPU_TLS ppu_context* g_active_ctx;
extern uint32_t g_last_hle_nid;
extern const char* g_last_hle_name;
extern void ps3_hle_dump_recent_calls(void);
extern void ps3_hle_dump_main_calls(void);
ppu_context* g_main_ppu_ctx = nullptr;
extern void ppu_dump_guest_stack(ppu_context* ctx, const char* tag);

/* Guest callbacks and RSX are driven by the host, just as the PS3's display
 * interrupt would.  Leaving these un-driven lets GT6 finish setup then wait
 * forever for its first vblank / RSX fence. */
extern ps3_guest_caller_fn g_ps3_guest_caller;
extern uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3);
extern void cellGcmTickVBlank(void);
extern void cellGcmTickFlip(void);
extern void cellGcm_rsx_process_fifo(void);
extern int cellGcm_take_flip_pending(void);
extern int rsx_d3d12_backend_init(uint32_t width, uint32_t height,
                                  const char* title);
extern void rsx_d3d12_backend_present(void);
extern int rsx_d3d12_backend_pump_messages(void);
/* Non-Windows fallback: no real window yet (libs/video/rsx_null_backend.c
 * has only a Win32 GDI implementation and a POSIX stub that just returns
 * failure) -- see rsx_ok handling in gt6_vblank_ticker below, which already
 * tolerates the backend being unavailable and keeps ticking VBlank/FIFO. */
extern int rsx_null_backend_init(uint32_t width, uint32_t height, const char* title);
extern void rsx_null_backend_shutdown(void);
extern int rsx_null_backend_pump_messages(void);
}

/* MSVC reports the useful "vector<T> too long" message immediately before
 * aborting, but the stock implementation loses the active guest registers.
 * Interpose only to record that state, then preserve normal C++ semantics by
 * throwing the same length_error. */
namespace std {
[[noreturn]] void _Xlength_error(const char* message)
{
    std::fprintf(stderr, "[GT6 XLENGTH] %s ctx=%p", message ? message : "<null>",
                 static_cast<void*>(g_active_ctx));
    if (g_active_ctx) {
        std::fprintf(stderr, " cia=%08X lr=%08X sp=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r30=%08X r31=%08X\\n",
                     static_cast<uint32_t>(g_active_ctx->cia),
                     static_cast<uint32_t>(g_active_ctx->lr),
                     static_cast<uint32_t>(g_active_ctx->gpr[1]),
                     static_cast<uint32_t>(g_active_ctx->gpr[2]),
                     static_cast<uint32_t>(g_active_ctx->gpr[3]),
                     static_cast<uint32_t>(g_active_ctx->gpr[4]),
                     static_cast<uint32_t>(g_active_ctx->gpr[5]),
                     static_cast<uint32_t>(g_active_ctx->gpr[6]),
                     static_cast<uint32_t>(g_active_ctx->gpr[30]),
                     static_cast<uint32_t>(g_active_ctx->gpr[31]));
        ppu_dump_guest_stack(g_active_ctx, "xlength");
    } else {
        std::fprintf(stderr, "\\n");
    }
    std::fflush(stderr);
    throw std::length_error(message ? message : "length error");
}
}

[[noreturn]] static void gt6_terminate_handler() noexcept
{
    const std::exception_ptr pending = std::current_exception();
    const char* what = "<non-std exception>";
    try {
        if (pending) std::rethrow_exception(pending);
    } catch (const std::exception& ex) {
        what = ex.what();
    } catch (...) {
    }
    std::fprintf(stderr, "[GT6 TERMINATE] %s ctx=%p", what,
                 static_cast<void*>(g_active_ctx));
    if (g_active_ctx) {
        std::fprintf(stderr, " tid=%llu cia=%08X lr=%08X sp=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r30=%08X r31=%08X\\n",
                     static_cast<unsigned long long>(g_active_ctx->thread_id),
                     static_cast<uint32_t>(g_active_ctx->cia),
                     static_cast<uint32_t>(g_active_ctx->lr),
                     static_cast<uint32_t>(g_active_ctx->gpr[1]),
                     static_cast<uint32_t>(g_active_ctx->gpr[2]),
                     static_cast<uint32_t>(g_active_ctx->gpr[3]),
                     static_cast<uint32_t>(g_active_ctx->gpr[4]),
                     static_cast<uint32_t>(g_active_ctx->gpr[5]),
                     static_cast<uint32_t>(g_active_ctx->gpr[6]),
                     static_cast<uint32_t>(g_active_ctx->gpr[30]),
                     static_cast<uint32_t>(g_active_ctx->gpr[31]));
        ppu_dump_guest_stack(g_active_ctx, "terminate");
    } else {
        std::fputc('\n', stderr);
    }
    std::fflush(stderr);
    std::abort();
}

/* Size of the reserved (not committed) PS3 guest address space: the full
 * 32-bit range plus a little slack. Shared by both the Windows VirtualAlloc
 * path and the POSIX mmap path below. */
static constexpr uintptr_t kGuestVmSize = 0x100010000ull;

#ifdef _WIN32
static DWORD g_main_host_tid = 0;
static DWORD g_vblank_host_tid = 0;
static uintptr_t g_host_image_base = 0;

static bool gt6_guest_readable(uint32_t address)
{
    if (!vm_base)
        return false;
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(vm_base + address, &memory, sizeof(memory)) &&
           memory.State == MEM_COMMIT &&
           !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

static uint32_t gt6_guest_be32(uint32_t address)
{
    if (!gt6_guest_readable(address))
        return 0;
    const uint8_t* p = vm_base + address;
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static void gt6_guest_string_preview(uint32_t address, char* output, size_t capacity)
{
    if (!capacity)
        return;
    size_t i = 0;
    if (gt6_guest_readable(address)) {
        for (; i + 1 < capacity; ++i) {
            const uint8_t ch = vm_base[address + i];
            if (!ch)
                break;
            output[i] = ch >= 0x20 && ch <= 0x7E ? static_cast<char>(ch) : '.';
        }
    }
    output[i] = '\0';
}

/* Capture the point at which every host thread is parked without requiring a
 * debugger.  The PPU table identifies guest workers, while the host stack scan
 * also covers the main PPU thread (which is intentionally not in that table).
 * Module RVAs can be resolved directly against GT6MainRecomp.map. */
static void gt6_dump_thread_snapshot(void)
{
    const uintptr_t image_base =
        reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        image_base + static_cast<uintptr_t>(dos->e_lfanew));
    const uintptr_t image_end = image_base + nt->OptionalHeader.SizeOfImage;
    const DWORD process_id = GetCurrentProcessId();
    uintptr_t main_rip = 0;
    uintptr_t stack_rvas[10]{};
    unsigned stack_rva_count = 0;
    ppu_context main_context{};
    bool have_host_context = false;
    bool have_main_context = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);

    if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process_id ||
                entry.th32ThreadID != g_main_host_tid)
                continue;
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                                       FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                CloseHandle(thread);
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(thread, &context)) {
                have_host_context = true;
                main_rip = static_cast<uintptr_t>(context.Rip);
                if (g_main_ppu_ctx) {
                    main_context = *g_main_ppu_ctx;
                    have_main_context = true;
                }
                const uintptr_t stack_pointer =
                    static_cast<uintptr_t>(context.Rsp);
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(reinterpret_cast<const void*>(stack_pointer),
                                 &memory, sizeof(memory)) &&
                    memory.State == MEM_COMMIT &&
                    !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                    const uintptr_t region_end =
                        reinterpret_cast<uintptr_t>(memory.BaseAddress) +
                        memory.RegionSize;
                    size_t words = (region_end - stack_pointer) / sizeof(uintptr_t);
                    if (words > 0x10000 / sizeof(uintptr_t))
                        words = 0x10000 / sizeof(uintptr_t);
                    const uintptr_t* stack =
                        reinterpret_cast<const uintptr_t*>(stack_pointer);
                    for (size_t j = 0; j < words && stack_rva_count < 10; ++j) {
                        const uintptr_t value = stack[j];
                        if (value >= image_base && value < image_end) {
                            stack_rvas[stack_rva_count++] = value - image_base;
                        }
                    }
                }
            }
            ResumeThread(thread);
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(snapshot);

    /* Never print while the sampled thread is suspended: it may own the CRT's
     * stderr lock. */
    if (have_main_context) {
        std::fprintf(stderr,
            "[GT6 THREADS] main-ppu cia(stale)=%08X lr=%08X sp=%08X toc=%08X "
            "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r11=%08X "
            "r13=%08X r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X ctr=%08X\n",
            static_cast<uint32_t>(main_context.cia),
            static_cast<uint32_t>(main_context.lr),
            static_cast<uint32_t>(main_context.gpr[1]),
            static_cast<uint32_t>(main_context.gpr[2]),
            static_cast<uint32_t>(main_context.gpr[3]),
            static_cast<uint32_t>(main_context.gpr[4]),
            static_cast<uint32_t>(main_context.gpr[5]),
            static_cast<uint32_t>(main_context.gpr[6]),
            static_cast<uint32_t>(main_context.gpr[7]),
            static_cast<uint32_t>(main_context.gpr[11]),
            static_cast<uint32_t>(main_context.gpr[13]),
            static_cast<uint32_t>(main_context.gpr[27]),
            static_cast<uint32_t>(main_context.gpr[28]),
            static_cast<uint32_t>(main_context.gpr[29]),
            static_cast<uint32_t>(main_context.gpr[30]),
            static_cast<uint32_t>(main_context.gpr[31]),
            static_cast<uint32_t>(main_context.ctr));
        ppu_dump_guest_stack(&main_context, "main-watchdog");

        /* func_00C0825C walks a name-index linked list at LR 0x00C08570.
         * Preserve its node topology and compared strings so a self-loop or
         * missing pager data is obvious from one smoke run. */
        if (static_cast<uint32_t>(main_context.lr) == 0x00C08570u) {
            const uint32_t node = static_cast<uint32_t>(main_context.gpr[30]);
            const uint32_t target = static_cast<uint32_t>(main_context.gpr[31]);
            const uint32_t next = gt6_guest_be32(node + 4);
            const uint32_t marker = gt6_guest_be32(node + 8);
            const uint32_t name = gt6_guest_be32(node + 0x18);
            char node_text[65]{};
            char target_text[65]{};
            gt6_guest_string_preview(name, node_text, sizeof(node_text));
            gt6_guest_string_preview(target, target_text, sizeof(target_text));
            std::fprintf(stderr,
                "[GT6 THREADS] name-list node=%08X next=%08X marker=%08X "
                "name=%08X '%s' target=%08X '%s'\n",
                node, next, marker, name, node_text, target, target_text);
        }
    }
    if (have_host_context) {
        if (main_rip >= image_base && main_rip < image_end)
            std::fprintf(stderr, "[GT6 THREADS] main-host rip-rva=%llX stack-rvas:",
                static_cast<unsigned long long>(main_rip - image_base));
        else
            std::fprintf(stderr, "[GT6 THREADS] main-host rip=%p stack-rvas:",
                reinterpret_cast<void*>(main_rip));
        for (unsigned i = 0; i < stack_rva_count; ++i)
            std::fprintf(stderr, " %llX",
                static_cast<unsigned long long>(stack_rvas[i]));
        std::fputc('\n', stderr);
    }

    std::fprintf(stderr, "[GT6 THREADS] PPU worker snapshot\n");
    for (unsigned i = 0; i < PPU_THREAD_MAX; ++i) {
        const ppu_thread_info& thread = g_ppu_threads[i];
        if (thread.state == PPU_THREAD_STATE_FREE)
            continue;
        std::fprintf(stderr,
            "[GT6 THREADS] ppu=%u host=%lu state=%d name='%s' entry=%08llX "
            "cia(stale)=%08X lr=%08X sp=%08X r3=%08X\n",
            i + 1, static_cast<unsigned long>(thread.host_tid), thread.state,
            thread.name, static_cast<unsigned long long>(thread.entry_addr),
            static_cast<uint32_t>(thread.ctx.cia),
            static_cast<uint32_t>(thread.ctx.lr),
            static_cast<uint32_t>(thread.ctx.gpr[1]),
            static_cast<uint32_t>(thread.ctx.gpr[3]));
    }
}

static void gt6_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                             uint64_t a2, uint64_t a3)
{
    ppu_guest_call(opd, a0, a1, a2, a3);
}

static LONG WINAPI report_unhandled_exception(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* record = ep->ExceptionRecord;
    const uintptr_t rip = static_cast<uintptr_t>(ep->ContextRecord->Rip);
    const uintptr_t target = record->NumberParameters >= 2
        ? static_cast<uintptr_t>(record->ExceptionInformation[1]) : 0;
    const uintptr_t image_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const char* operation = record->NumberParameters >= 1 && record->ExceptionInformation[0]
        ? "write" : "read";

    std::fprintf(stderr,
        "[GT6 CRASH] code=0x%08lX %s target=%p host-rip=%p rva=0x%llX\n",
        static_cast<unsigned long>(record->ExceptionCode), operation,
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(rip),
        static_cast<unsigned long long>(rip - image_base));
    std::fprintf(stderr, "[GT6 CRASH] last-hle=0x%08X %s\n", g_last_hle_nid,
                 g_last_hle_name ? g_last_hle_name : "<none>");
    void* frames[12]{};
    const USHORT frame_count = RtlCaptureStackBackTrace(0, 12, frames, nullptr);
    std::fprintf(stderr, "[GT6 CRASH] image-base=%p host-stack:",
                 reinterpret_cast<void*>(image_base));
    for (USHORT i = 0; i < frame_count; ++i)
        std::fprintf(stderr, " %p", frames[i]);
    std::fputc('\n', stderr);
    if (g_active_ctx) {
        std::fprintf(stderr,
            "[GT6 CRASH] guest cia=%08X lr=%08X sp=%08X r3=%08X r4=%08X r5=%08X\n",
            static_cast<uint32_t>(g_active_ctx->cia),
            static_cast<uint32_t>(g_active_ctx->lr),
            static_cast<uint32_t>(g_active_ctx->gpr[1]),
            static_cast<uint32_t>(g_active_ctx->gpr[3]),
            static_cast<uint32_t>(g_active_ctx->gpr[4]),
            static_cast<uint32_t>(g_active_ctx->gpr[5]));
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* A graphics/runtime DLL may replace the process unhandled-exception filter
 * after startup.  Keep a last vectored observer behind the guest demand-pager
 * so a genuine host AV still leaves a symbolizable RIP in redirected logs.
 * Guest page faults are consumed by commit_guest_page before this runs. */
static LONG WINAPI report_host_access_violation(EXCEPTION_POINTERS* ep)
{
    constexpr DWORD kMsvcCppException = 0xE06D7363u;
    const DWORD exception_code = ep->ExceptionRecord->ExceptionCode;
    if (exception_code != EXCEPTION_ACCESS_VIOLATION && exception_code != kMsvcCppException)
        return EXCEPTION_CONTINUE_SEARCH;

    static volatile LONG reports = 0;
    if (InterlockedIncrement(&reports) <= 8) {
        /* Avoid the CRT here: the observed crash is inside ntdll's heap/lock
         * machinery, where recursively entering fprintf can fault again before
         * it emits a byte. */
        char line[2048];
        char* out = line;
        const char* const end = line + sizeof(line) - 2;
        const auto text = [&out, end](const char* value) {
            while (*value && out < end) *out++ = *value++;
        };
        const auto hex = [&out, end](uint64_t value, unsigned digits) {
            static constexpr char chars[] = "0123456789ABCDEF";
            if (out + digits > end) return;
            for (unsigned i = 0; i < digits; ++i) {
                const unsigned shift = (digits - 1 - i) * 4;
                *out++ = chars[(value >> shift) & 0xFu];
            }
        };

        const EXCEPTION_RECORD* record = ep->ExceptionRecord;
        const CONTEXT* context = ep->ContextRecord;
        const uintptr_t target = record->NumberParameters >= 2
            ? static_cast<uintptr_t>(record->ExceptionInformation[1]) : 0;
        text("\n[GT6 VEH] code=0x"); hex(record->ExceptionCode, 8);
        if (record->ExceptionCode == kMsvcCppException) text(" (MSVC-C++ throw)");
        text(" tid=0x"); hex(GetCurrentThreadId(), 8);
        text(" op=0x");
        hex(record->NumberParameters ? record->ExceptionInformation[0] : 0, 2);
        text(" target=0x"); hex(target, 16);
        text(" rip=0x"); hex(context->Rip, 16);
        text(" image-rva=0x");
        hex(context->Rip - g_host_image_base, 16);
        text(" rsp=0x"); hex(context->Rsp, 16);
        text(" last-nid=0x"); hex(g_last_hle_nid, 8);
        text(" ctx=0x"); hex(reinterpret_cast<uintptr_t>(g_active_ctx), 16);
        /* C++ throws are expected during ordinary container probing, but the
         * current guest context identifies the one that later escapes.  Read
         * it under SEH so diagnostics never turn an exception into an AV. */
        __try {
            if (g_active_ctx) {
                text(" guest-tid=0x"); hex(g_active_ctx->thread_id, 8);
                text(" cia=0x"); hex(g_active_ctx->cia, 8);
                text(" lr=0x"); hex(g_active_ctx->lr, 8);
                text(" sp=0x"); hex(g_active_ctx->gpr[1], 8);
                text(" r3=0x"); hex(g_active_ctx->gpr[3], 8);
                text(" r4=0x"); hex(g_active_ctx->gpr[4], 8);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            text(" guest=<unreadable>");
        }

        /* Raw return-address candidates remain useful even when normal stack
         * unwinding cannot cross KiUserExceptionDispatch. */
        __try {
            const uintptr_t* stack = reinterpret_cast<const uintptr_t*>(context->Rsp);
            text(" stack:");
            for (unsigned i = 0; i < 24 && out + 18 < end; ++i) {
                text(" "); hex(stack[i], 16);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            text(" <unreadable>");
        }
        *out++ = '\n';
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), line,
                  static_cast<DWORD>(out - line), &written, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI commit_guest_page(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !vm_base)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
    const uintptr_t base = reinterpret_cast<uintptr_t>(vm_base);
    if (fault < base || fault >= base + kGuestVmSize)
        return EXCEPTION_CONTINUE_SEARCH;

    // Commit lazily in allocation-granularity units as ELF segments are loaded.
    void* page = reinterpret_cast<void*>(fault & ~static_cast<uintptr_t>(0xFFFF));
    return VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE)
        ? EXCEPTION_CONTINUE_EXECUTION
        : EXCEPTION_CONTINUE_SEARCH;
}
#else /* !_WIN32 */

/* Diagnostic thread/register snapshot on a stall. Full parity with the
 * Windows Toolhelp32-based version (which suspends and inspects every host
 * thread's register state) isn't implemented yet on POSIX; this is a stub
 * so the smoke-test watchdog still runs, not a claim of equivalent detail. */
static void gt6_dump_thread_snapshot(void)
{
    std::fprintf(stderr, "[GT6 THREADS] snapshot not implemented on this platform\n");
}

static void gt6_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                             uint64_t a2, uint64_t a3)
{
    ppu_guest_call(opd, a0, a1, a2, a3);
}

/* POSIX equivalent of commit_guest_page above: vm_base is reserved with
 * PROT_NONE (mmap, no upfront commit of ~4 GiB), so any touch faults with
 * SIGSEGV. If the fault is inside the guest VM range, make that 64 KiB page
 * readable/writable and retry the faulting instruction; otherwise this is a
 * genuine host bug, so restore the default handler and re-raise so a normal
 * core dump / diagnostic still happens. */
static void gt6_posix_segv_handler(int sig, siginfo_t* info, void* uctx)
{
    (void)uctx;
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(vm_base);
    if (vm_base && fault >= base && fault < base + kGuestVmSize) {
        void* page = reinterpret_cast<void*>(fault & ~static_cast<uintptr_t>(0xFFFF));
        if (mprotect(page, 0x10000, PROT_READ | PROT_WRITE) == 0)
            return; /* retry the faulting instruction */
    }
    std::fprintf(stderr, "[GT6 CRASH] SIGSEGV addr=%p last-hle=0x%08X %s\n",
                 info->si_addr, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "<none>");
    if (g_active_ctx) {
        std::fprintf(stderr,
            "[GT6 CRASH] guest cia=%08X lr=%08X sp=%08X r3=%08X r4=%08X r5=%08X\n",
            static_cast<uint32_t>(g_active_ctx->cia), static_cast<uint32_t>(g_active_ctx->lr),
            static_cast<uint32_t>(g_active_ctx->gpr[1]), static_cast<uint32_t>(g_active_ctx->gpr[3]),
            static_cast<uint32_t>(g_active_ctx->gpr[4]), static_cast<uint32_t>(g_active_ctx->gpr[5]));
    }
    std::fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void gt6_install_posix_segv_handler(void)
{
    struct sigaction sa{};
    sa.sa_sigaction = gt6_posix_segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
}
#endif

/* Own the display/RSX pump on this thread and emulate the two display
 * interrupts that libgcm expects.  The driver starts before game entry, but
 * every routine below is a no-op until cellGcmInit and the handlers have
 * been registered. Deliberately outside the big #ifdef _WIN32/#else block
 * above (unlike that block, everything here is meant to run on both
 * platforms; see the inner #ifdefs for the D3D12/null-backend split). */
void gt6_vblank_tick_safe() {
#ifdef _WIN32
    __try {
        cellGcmTickVBlank();
        cellGcmTickFlip();
    } __except(1) {
        std::fprintf(stderr, "[GT6 RSX] CRASH in VBlank/Flip thread! Exception code: 0x%08X\n", GetExceptionCode());
        ExitProcess(1);
    }
#else
    cellGcmTickVBlank();
    cellGcmTickFlip();
#endif
}

/* Portable across Windows (D3D12) and non-Windows (null/headless backend --
 * no real window yet, see the rsx_null_backend note above); only the actual
 * backend calls differ, so this body is written once. Launched via
 * std::thread from main() on both platforms. */
static void gt6_vblank_ticker()
{
#ifdef _WIN32
    int rsx_ok = rsx_d3d12_backend_init(1280, 720, "Gran Turismo 6 (ps3recomp)") == 0;
#else
    int rsx_ok = rsx_null_backend_init(1280, 720, "Gran Turismo 6 (ps3recomp)") == 0;
#endif
    std::fprintf(stderr, "[GT6 RSX] backend %s\n", rsx_ok ? "ready" : "unavailable");

    using clock = std::chrono::steady_clock;
    auto next_tick = clock::now();
    auto next_boot_present = next_tick;
    const auto tick_period = std::chrono::milliseconds(16);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const auto now = clock::now();
        int fired = 0;
        while (now >= next_tick && fired++ < 8) {
            gt6_vblank_tick_safe();
            next_tick += tick_period;
        }
        if (fired >= 8)
            next_tick = now + tick_period;

        if (!rsx_ok)
            continue;

        /* Drain at a short cadence: GT6 waits on RSX labels between setup
         * passes, so servicing them only once per vblank causes a false stall. */
        cellGcm_rsx_process_fifo();
#ifdef _WIN32
        if (cellGcm_take_flip_pending()) {
            rsx_d3d12_backend_present();
        } else if (now >= next_boot_present) {
            /* Make the live graphics surface visible even before GT6 submits
             * its first flip. This is deliberately a backend clear only, not
             * a fabricated game frame. */
            rsx_d3d12_backend_present();
            /* Keep the host surface paced to the PS3's 60 Hz vblank even
             * while the title has not submitted a flip yet.  The old 500 ms
             * fallback made the valid first draw appear as a 2 FPS window. */
            next_boot_present = now + tick_period;
        }
        if (rsx_d3d12_backend_pump_messages() != 0)
            rsx_ok = 0;
#else
        /* No real present/pump on the headless POSIX stub yet -- just keep
         * draining the FIFO so guest-side flip bookkeeping still advances. */
        (void)cellGcm_take_flip_pending();
        if (rsx_null_backend_pump_messages() != 0)
            rsx_ok = 0;
#endif
    }
}

#ifdef _WIN32
#include <windows.h>
extern int g_s_depth;
uint32_t g_crash_code = 0;
uint64_t g_crash_rip = 0;
uint64_t g_crash_rsp = 0;

int crash_filter(PEXCEPTION_POINTERS ep) {
    g_crash_code = ep->ExceptionRecord->ExceptionCode;
    g_crash_rip = ep->ContextRecord->Rip;
    g_crash_rsp = ep->ContextRecord->Rsp;
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int ppu_run_safe(uint32_t entry) {
#ifdef _WIN32
    __try {
        return ppu_run(entry, 0x0FF00000u);
    } __except(crash_filter(GetExceptionInformation())) {
        _resetstkoflw();
        std::fprintf(stderr, "[GT6Recomp] CRASH in main thread! Exception code: 0x%08X (depth: %d)\n",
            g_crash_code, g_s_depth);
        std::fprintf(stderr, "[GT6Recomp] CRASH RIP: 0x%016llX\n", (unsigned long long)g_crash_rip);
        std::fprintf(stderr, "[GT6Recomp] CRASH RSP: 0x%016llX\n", (unsigned long long)g_crash_rsp);
        char* base = (char*)GetModuleHandleA(0);
        std::fprintf(stderr, "[GT6Recomp] CRASH RIP module offset: 0x%016llX\n", (unsigned long long)(g_crash_rip - (uint64_t)base));
        
        void* fr[48];
        unsigned short n = RtlCaptureStackBackTrace(0, 48, fr, 0);
        char* base_stack = (char*)GetModuleHandleA(0);
        for(unsigned short i = 0; i < n; i++) {
            std::fprintf(stderr, "  frame %u: host RVA 0x%llX\n", i, (unsigned long long)((char*)fr[i]-base_stack));
        }
        ExitProcess(1);
    }
#else
    return ppu_run(entry, 0x0FF00000u);
#endif
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <decrypted-EBOOT.elf>\n", argv[0]);
        return 64;
    }

#ifdef _WIN32
    g_main_host_tid = GetCurrentThreadId();
    g_host_image_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    // Reserve the 32-bit PS3 guest address space without committing 4 GiB of RAM.
    vm_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kGuestVmSize,
                                                  MEM_RESERVE, PAGE_READWRITE));
#else
    // POSIX equivalent: reserve the range with PROT_NONE (no upfront commit),
    // then gt6_posix_segv_handler() below faults pages in on first touch.
    void* reserved = mmap(nullptr, kGuestVmSize, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (reserved != MAP_FAILED) ? static_cast<uint8_t*>(reserved) : nullptr;
#endif
    if (!vm_base) {
        std::fprintf(stderr, "Unable to reserve the PS3 guest address space.\n");
        return 1;
    }

#ifdef _WIN32
    // Keep a compact host-side crash location in the smoke-test log. Guest VM
    // page faults are handled below; all other access violations are real
    // host-side faults worth preserving for the next HLE iteration.
    SetUnhandledExceptionFilter(report_unhandled_exception);
    AddVectoredExceptionHandler(1, commit_guest_page);
    AddVectoredExceptionHandler(0, report_host_access_violation);
#else
    gt6_install_posix_segv_handler();
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);

    const uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        std::fprintf(stderr, "Failed to load decrypted ELF: %s\n", argv[1]);
        return 1;
    }

    ppu_recomp_register();
    ppu_hle_init();
    /* The supplied executable and content are the PSN NPUA81049 build.  The
     * game selects its PDIPFS loader only when firmware reports an HDD boot,
     * rather than the disc GT.VOL path. */
    cellGame_set_title_id("NPUA81049");
    cellGame_set_title("Gran Turismo 6");
    /* GT6_RPCS3_STORAGE overrides the dev machine's hardcoded Windows path
     * below (e.g. on Linux: /mnt/dados/Emulation/storage/rpcs3) -- must
     * match the root passed to cellfs_set_root_path in gt6_hle.cpp. */
    {
        const char* storage_env = std::getenv("GT6_RPCS3_STORAGE");
        const std::string storage = storage_env ? storage_env : "E:/Emulation/storage/rpcs3";
        cellGame_init_from_paramsfo((storage + "/dev_hdd0/game/NPUA81049/PARAM.SFO").c_str());
        cellGame_set_content_path((storage + "/dev_hdd0/game").c_str());
    }
    cellGame_set_game_type(CELL_GAME_GAMETYPE_HDD);
    gt6_register_hle();
    ppu_sysprx_register();
    lv2_init_syscalls();
    std::set_terminate(gt6_terminate_handler);

    /* Install callback dispatch before GT6 registers its vblank/flip OPDs,
     * then run the synthetic RSX display loop on its own thread. */
    g_ps3_guest_caller = gt6_guest_caller;
    std::thread(gt6_vblank_ticker).detach();

    /* Opt-in smoke-test watchdog.  It makes a long-running logical stall
     * observable in CI/terminal runs without changing normal execution. */
    if (const char* timeout_env = std::getenv("GT6_SMOKE_TIMEOUT_MS")) {
        const long timeout_ms = std::strtol(timeout_env, nullptr, 10);
        if (timeout_ms > 0) {
            std::thread([timeout_ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
                std::fprintf(stderr, "[GT6 SMOKE] watchdog after %ld ms\n", timeout_ms);
                gt6_dump_thread_snapshot();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                gt6_dump_thread_snapshot();
                ps3_hle_dump_main_calls();
                ps3_hle_dump_recent_calls();
                std::fflush(stderr);
#ifdef _WIN32
                ExitProcess(0);
#endif
            }).detach();
        }
    }

    std::fprintf(stderr, "[GT6Recomp] dispatching entry OPD 0x%08X\n", entry);
    return ppu_run_safe(entry);
}
