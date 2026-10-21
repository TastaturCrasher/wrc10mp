// wrc10mp - forced-pause fix (Steam mode).
//
// Opening the Steam overlay (e.g. to join a friend's lobby) makes the engine
// set its "forced pause" through the virtual setter at vtable slot 0x168
// (WRC override 0x140dc5820). With 1 it stops the game clock (slot 0x5e8,
// 0x140dc3aa0), with 0 it restarts it (slot 0x5e0, 0x140dc3b10); the state is
// the byte engine + 0x1ae (engine pointer at 0x14192bf50). After joining an
// online lobby nothing resets it: WrcMenuManager::OnManage (0x140510ac0)
// reopens the pause menu right after every "Continue", and the game clock
// stays stopped, so the car never leaves the start line.
//
// A background thread logs every change of the byte to wrc10mp_pause.log.
// With [steam] clear_forced_pause=1 (default) a hook on PeekMessageA, which
// the game's main thread calls every frame, calls the engine's own setter with
// 0 whenever the byte is set, so the clock is restarted too. Only active for
// the WRC10.exe build this was analysed on (PE timestamp 0x62948268).

#include <windows.h>

#include <cstdio>
#include <string>

#include "lan.h"
#include "pause.h"

namespace {

const DWORD kExpectedTimestamp = 0x62948268;  // WRC10.exe 1.6.32.2
const ULONG_PTR kEnginePtrRva = 0x192bf50;    // 0x14192bf50 - 0x140000000
const ULONG_PTR kForcedPauseOffset = 0x1ae;
const ULONG_PTR kSetterSlot = 0x168;
const ULONG_PTR kSetterRva = 0xdc5820;  // 0x140dc5820, WRC override of the setter

using SetForcedPauseFn = void(__fastcall*)(void* engine, char on);
using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

std::wstring g_dir;
BYTE* g_exe = nullptr;
DWORD g_mainThread = 0;
PeekMessageAFn g_realPeekMessageA = nullptr;

void Log(const char* fmt, unsigned long long a = 0, unsigned b = 0) {
    FILE* f = nullptr;
    _wfopen_s(&f, (g_dir + L"wrc10mp_pause.log").c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    fprintf(f, fmt, a, b);
    fputc('\n', f);
    fclose(f);
}

// Reads game memory that may not exist yet; any fault just means "not now".
bool ReadByte(const BYTE* p, BYTE* out) {
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPtr(const ULONG_PTR* p, ULONG_PTR* out) {
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

ULONG_PTR Engine() {
    ULONG_PTR engine = 0;
    ReadPtr((const ULONG_PTR*)(g_exe + kEnginePtrRva), &engine);
    return engine;
}

// Main thread, once per frame: lift a forced pause through the engine's setter.
void ClearForcedPause() {
    ULONG_PTR engine = Engine();
    BYTE on = 0;
    if (!engine || !ReadByte((BYTE*)engine + kForcedPauseOffset, &on) || !on) return;
    ULONG_PTR vtable = 0, setter = 0;
    if (!ReadPtr((const ULONG_PTR*)engine, &vtable) || !ReadPtr((const ULONG_PTR*)(vtable + kSetterSlot), &setter))
        return;
    if (setter != (ULONG_PTR)(g_exe + kSetterRva)) {
        static bool warned = false;
        if (!warned) Log("unexpected forced pause setter 0x%llx, not clearing", setter - (ULONG_PTR)g_exe);
        warned = true;
        return;
    }
    ((SetForcedPauseFn)setter)((void*)engine, 0);
    static int lifted = 0;
    if (++lifted <= 50) Log("forced pause lifted (engine setter, game clock restarted)");
}

BOOL WINAPI HookPeekMessageA(LPMSG msg, HWND wnd, UINT min, UINT max, UINT remove) {
    if (GetCurrentThreadId() == g_mainThread) {
        static bool seen = false;
        if (!seen) Log("main thread message loop reached");
        seen = true;
        ClearForcedPause();
    }
    return g_realPeekMessageA(msg, wnd, min, max, remove);
}

DWORD WINAPI WatchThread(LPVOID) {
    ULONG_PTR lastEngine = 0;
    int last = -1;
    for (;;) {
        Sleep(20);
        ULONG_PTR engine = Engine();
        if (!engine) continue;
        if (engine != lastEngine) {
            Log("engine object at 0x%llx", engine);
            lastEngine = engine;
            last = -1;
        }
        BYTE v = 0;
        if (!ReadByte((BYTE*)engine + kForcedPauseOffset, &v)) continue;
        if (v != last) {
            Log("forced pause flag (engine+0x1ae) = %llu", v);
            last = v;
        }
    }
}

}  // namespace

void InstallPauseWatch(const std::wstring& dir) {
    g_dir = dir;
    std::wstring ini = dir + L"wrc10mp.ini";
    if (!GetPrivateProfileIntW(L"steam", L"pause_watch", 1, ini.c_str())) return;
    bool clear = GetPrivateProfileIntW(L"steam", L"clear_forced_pause", 1, ini.c_str()) != 0;

    g_exe = (BYTE*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)g_exe;
    auto* nt = (IMAGE_NT_HEADERS*)(g_exe + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kExpectedTimestamp) {
        Log("pause watch off: unknown WRC10.exe build (timestamp 0x%llx)", nt->FileHeader.TimeDateStamp);
        return;
    }
    if (clear) {
        // winhttp.dll is a static import of WRC10.exe, so DllMain runs on the main thread.
        g_mainThread = GetCurrentThreadId();
        g_realPeekMessageA = (PeekMessageAFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "PeekMessageA");
        clear = g_realPeekMessageA &&
                PatchImport((HMODULE)g_exe, "USER32.dll", (void*)g_realPeekMessageA, (void*)&HookPeekMessageA);
    }
    Log(clear ? "pause watch on, lifting forced pause on the main thread" : "pause watch on, logging only");
    CloseHandle(CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr));
}
