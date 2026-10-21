#pragma once

#include <string>

// Logs fatal exceptions of the game to <dir>wrc10mp_crash.log and writes a
// minidump of the first one to <dir>wrc10mp_crash.dmp. `dir` ends with a
// backslash. Disabled with [general] crash_log=0 in wrc10mp.ini.
void InstallCrashLogger(const std::wstring& dir);
