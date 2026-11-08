#pragma once

// CommonLibSSE hard-errors if any Windows API header lands before its own
// headers, so <windows.h> must never appear here or in any TU that includes
// this file first.  Everything below is portable standard library code.

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
