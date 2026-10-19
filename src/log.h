#pragma once
#include <string>

// Directory containing RCT3.exe, with trailing backslash.
const std::wstring& GameDir();

void Log(const char* fmt, ...);
