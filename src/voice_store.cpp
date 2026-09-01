#include "voice_store.hpp"

#include <windows.h>
#include <shlobj.h>

namespace PocketTts {
namespace store {

std::wstring data_dir()
{
    wchar_t env[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"POCKETTTS_DATA_DIR", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::wstring(env, n);
    }

    PWSTR folder = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &folder))) {
        result = folder;
        result += L"\\PocketTTS";
    } else {
        result = L"C:\\ProgramData\\PocketTTS";
    }
    if (folder) {
        CoTaskMemFree(folder);
    }
    return result;
}

std::vector<voice_info> read_published_voices()
{
    std::vector<voice_info> voices;
    const std::wstring ini = data_dir() + L"\\voices\\voices.ini";

    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return voices;
    }

    // Section names = voice names. 32KB covers hundreds of voices.
    std::vector<wchar_t> sections(32768, L'\0');
    const DWORD len = GetPrivateProfileSectionNamesW(
        sections.data(), static_cast<DWORD>(sections.size()), ini.c_str());
    if (len == 0) {
        return voices;
    }

    for (const wchar_t* p = sections.data(); *p; p += wcslen(p) + 1) {
        wchar_t value[256];

        GetPrivateProfileStringW(p, L"published", L"0", value, 256, ini.c_str());
        if (wcscmp(value, L"1") != 0) {
            continue;
        }

        voice_info v;
        v.name = p;
        v.published = true;

        GetPrivateProfileStringW(p, L"gender", L"Male", value, 256, ini.c_str());
        v.gender = (_wcsicmp(value, L"female") == 0) ? L"Female" : L"Male";

        GetPrivateProfileStringW(p, L"language", L"409", value, 256, ini.c_str());
        v.language = value;

        voices.push_back(std::move(v));
    }
    return voices;
}

}
}
