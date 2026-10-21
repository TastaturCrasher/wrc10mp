// wrc10mp - crash logger.
//
// The game's own crash reporter (CrashRpt) deletes its dump when its dialog is
// closed, and WRC10.log stops before the crash. This vectored exception
// handler writes the faulting address, registers and the unwound call stack
// (as module+offset, matching the addresses in a disassembler at image base
// 0x140000000) to wrc10mp_crash.log, plus a minidump of the first crash.
// Only fatal exception codes are recorded; the game keeps handling them as
// before.

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "crash.h"

namespace {

std::wstring g_dir;
std::mutex g_crashMutex;
int g_events = 0;
bool g_dumped = false;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

bool IsFatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_BREAKPOINT:  // release-build asserts end in int3
            return true;
        default:
            return false;
    }
}

// Preferred image base from the module file on disk. The loader overwrites
// OptionalHeader.ImageBase in memory with the actual (ASLR) base.
DWORD64 PreferredBase(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BYTE head[4096];
    DWORD got = 0;
    BOOL ok = ReadFile(f, head, sizeof(head), &got, nullptr);
    CloseHandle(f);
    if (!ok || got < sizeof(IMAGE_DOS_HEADER)) return 0;
    LONG peOff = ((IMAGE_DOS_HEADER*)head)->e_lfanew;
    if (peOff <= 0 || peOff + sizeof(IMAGE_NT_HEADERS64) > got) return 0;
    auto* nt = (IMAGE_NT_HEADERS64*)(head + peOff);
    return nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? nt->OptionalHeader.ImageBase : 0;
}

// "WRC10.exe+0xf98350 (0x140f98350)" style, the second value rebased to the
// module's preferred image base so it matches static analysis.
std::string Where(DWORD64 addr) {
    HMODULE mod = nullptr;
    char buf[MAX_PATH + 64];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod)) {
        snprintf(buf, sizeof(buf), "0x%llx", addr);
        return buf;
    }
    char path[MAX_PATH];
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    DWORD64 rva = addr - (DWORD64)mod;
    DWORD64 preferred = PreferredBase(path);
    if (preferred)
        snprintf(buf, sizeof(buf), "%s+0x%llx (0x%llx)", name, rva, preferred + rva);
    else
        snprintf(buf, sizeof(buf), "%s+0x%llx", name, rva);
    return buf;
}

void WriteDump(EXCEPTION_POINTERS* ep) {
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto write = dbghelp ? (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
    if (!write) return;
    HANDLE file = CreateFileW((g_dir + L"wrc10mp_crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
    write(GetCurrentProcess(), GetCurrentProcessId(), file,
          (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &info, nullptr, nullptr);
    CloseHandle(file);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsFatal(code)) return EXCEPTION_CONTINUE_SEARCH;
    // Faults inside this DLL are guarded reads of game memory (pause.cpp).
    HMODULE self = nullptr, at = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&OnException, &self);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &at);
    if (at && at == self) return EXCEPTION_CONTINUE_SEARCH;
    std::lock_guard<std::mutex> lock(g_crashMutex);
    if (g_events >= 20) return EXCEPTION_CONTINUE_SEARCH;
    ++g_events;

    FILE* log = nullptr;
    _wfopen_s(&log, (g_dir + L"wrc10mp_crash.log").c_str(), L"ab");
    if (log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        CONTEXT* c = ep->ContextRecord;
        fprintf(log, "==== [%04d-%02d-%02d %02d:%02d:%02d.%03d] exception 0x%08lx at %s, thread %lu\n", t.wYear, t.wMonth,
                t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, code, Where(c->Rip).c_str(), GetCurrentThreadId());
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            fprintf(log, "  %s address 0x%llx\n",
                    ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute",
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        fprintf(log, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", c->Rax, c->Rbx, c->Rcx, c->Rdx);
        fprintf(log, "  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
        fprintf(log, "  r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n", c->R8, c->R9, c->R10, c->R11);
        fprintf(log, "  r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n", c->R12, c->R13, c->R14, c->R15);

        // Unwind a copy of the faulting context with the x64 unwind tables.
        CONTEXT ctx = *c;
        for (int frame = 0; frame < 32 && ctx.Rip; ++frame) {
            fprintf(log, "  #%02d %s\n", frame, Where(ctx.Rip).c_str());
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {  // leaf function: return address is at rsp
                ctx.Rip = *(DWORD64*)ctx.Rsp;
                ctx.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
        if (!g_dumped) fprintf(log, "  minidump: wrc10mp_crash.dmp\n");
        fclose(log);
    }
    if (!g_dumped) {
        g_dumped = true;
        WriteDump(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashLogger(const std::wstring& dir) {
    g_dir = dir;
    if (GetPrivateProfileIntW(L"general", L"crash_log", 1, (dir + L"wrc10mp.ini").c_str()))
        AddVectoredExceptionHandler(1, OnException);
}
