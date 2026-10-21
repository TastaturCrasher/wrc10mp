#pragma once

#include <windows.h>

#include <string>

// Hooks sendto/recvfrom in WRC10.exe so LAN lobby search reaches the host set
// in <dir>wrc10mp.ini ([lan] host_ip). `dir` ends with a backslash.
void InstallLanHooks(const std::wstring& dir);

// Replaces the import of `target` from `dll` in `module`'s import table.
bool PatchImport(HMODULE module, const char* dll, void* target, void* hook);
