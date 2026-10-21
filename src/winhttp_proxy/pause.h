#pragma once

#include <string>

// Watches the game engine's forced-pause byte (WRC10.exe 1.6.32.2 only), logs
// changes to <dir>wrc10mp_pause.log and lifts it through the engine's own setter
// on the main thread ([steam] clear_forced_pause).
void InstallPauseWatch(const std::wstring& dir);
