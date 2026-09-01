#pragma once

// Always-on diagnostic logging for the SAPI engine DLLs and the Voice
// Manager. Each process writes %LOCALAPPDATA%\PocketTTS\logs\<exe>-<arch>.log
// so a 32-bit NVDA and a 64-bit application never fight over one file.
//
// The file is opened once per process with _SH_DENYNO (other processes can
// read and append concurrently) and in plain "a" mode: ccs= translated
// modes make fprintf fail-fast the whole process on encoding errors.
// Set POCKETTTS_LOG=0 in the environment to silence logging.

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <share.h>
#include <string>

namespace PocketTts {
namespace logging {

inline FILE* open_log_file()
{
    wchar_t env[8];
    const DWORD n = GetEnvironmentVariableW(L"POCKETTTS_LOG", env, 8);
    if (n > 0 && n < 8 && wcscmp(env, L"0") == 0) {
        return nullptr;
    }

    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) {
        return nullptr;
    }
    std::wstring dir(folder);
    CoTaskMemFree(folder);
    dir += L"\\PocketTTS";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);

    wchar_t exe_path[MAX_PATH] = L"unknown";
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const wchar_t* base = wcsrchr(exe_path, L'\\');
    base = base ? base + 1 : exe_path;

#ifdef _WIN64
    const wchar_t* arch = L"x64";
#else
    const wchar_t* arch = L"x86";
#endif

    std::wstring path = dir + L"\\" + base + L"-" + arch + L".log";

    // Start over when the log grows past 5 MB.
    const wchar_t* mode = L"a";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow > 5u * 1024 * 1024) {
        mode = L"w";
    }
    return _wfsopen(path.c_str(), mode, _SH_DENYNO);
}

inline void log(const char* format, ...)
{
    static FILE* file = open_log_file();
    if (!file) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(file, "[%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fprintf(file, "\n");
    fflush(file);
}

}
}

#define DEBUG_LOG(...) PocketTts::logging::log(__VA_ARGS__)
#define DEBUG_LOG_CLEAR() ((void)0)
