#pragma once

#include <string>
#include "voice_store.hpp"

namespace PocketTts {
namespace sapi {

// SAPI attribute view over a voice-store entry. Voices are dynamic: they
// come from <data_dir>\voices\voices.ini, which the Voice Manager and the
// host process maintain.
class voice_attributes
{
public:
    voice_attributes() = default;

    explicit voice_attributes(store::voice_info info)
        : info_(std::move(info))
    {
    }

    [[nodiscard]] std::wstring get_name() const
    {
        return info_.name;
    }

    [[nodiscard]] std::wstring get_age() const
    {
        return L"Adult";
    }

    [[nodiscard]] std::wstring get_gender() const
    {
        return info_.gender.empty() ? L"Male" : info_.gender;
    }

    [[nodiscard]] std::wstring get_language() const
    {
        return info_.language.empty() ? L"409" : info_.language;
    }

private:
    store::voice_info info_;
};

}
}
