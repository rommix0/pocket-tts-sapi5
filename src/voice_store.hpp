#pragma once

#include <string>
#include <vector>

namespace PocketTts {
namespace store {

struct voice_info
{
    std::wstring name;      // display name == section name in voices.ini
    std::wstring gender;    // L"Male" / L"Female"
    std::wstring language;  // hex LCID string, e.g. L"409"
    bool published = false;
};

// %ProgramData%\PocketTTS, or the POCKETTTS_DATA_DIR override.
[[nodiscard]] std::wstring data_dir();

// Voices marked published=1 in <data_dir>\voices\voices.ini.
[[nodiscard]] std::vector<voice_info> read_published_voices();

}
}
