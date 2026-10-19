#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

const std::wstring& GameDir()
{
    static std::wstring dir = [] {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p = path;
        return p.substr(0, p.find_last_of(L"\\/") + 1);
    }();
    return dir;
}

void Log(const char* fmt, ...)
{
    static std::mutex mutex;
    static FILE* file = nullptr;

    std::lock_guard<std::mutex> lock(mutex);
    if (!file) {
        file = _wfopen((GameDir() + L"rct3vr.log").c_str(), L"w");
        if (!file)
            return;
    }

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);

    fputc('\n', file);
    fflush(file);
}
