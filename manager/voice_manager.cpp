// Pocket TTS Voice Manager
//
// Accessible Win32 dialog application for cloning, testing, publishing and
// removing Pocket TTS SAPI5 voices, and for updating the AI models. All
// functionality is reachable with the keyboard; only standard dialog
// controls are used so screen readers announce everything correctly.

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>

#include <atomic>
#include <string>
#include <vector>

#include "../src/host_client.h"
#include "../src/utils.hpp"
#include "../src/debug_log.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr UINT WM_APP_STATUS = WM_APP + 1;   // lparam: std::wstring* (owned)
constexpr UINT WM_APP_DONE = WM_APP + 2;     // wparam: 1 success, lparam: std::wstring* error (owned, may be null)
constexpr UINT WM_APP_VOICES = WM_APP + 3;   // lparam: std::vector<HostVoice>* (owned)

HINSTANCE g_instance = nullptr;
HostClient g_client;
std::vector<HostVoice> g_voices;
std::wstring g_selectedVoice;
bool g_busy = false;

void post_status(HWND dlg, const std::wstring& text)
{
    PostMessageW(dlg, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
}

void set_text(HWND dlg, int id, const std::wstring& text)
{
    SetDlgItemTextW(dlg, id, text.c_str());
}

void append_log(HWND dlg, int id, const std::wstring& line)
{
    HWND edit = GetDlgItem(dlg, id);
    const int len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, len, len);
    std::wstring with_break = line + L"\r\n";
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(with_break.c_str()));
}

std::wstring get_text(HWND dlg, int id)
{
    HWND ctrl = GetDlgItem(dlg, id);
    const int len = GetWindowTextLengthW(ctrl);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(ctrl, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

// ---------------------------------------------------------------------------
// Streaming waveOut player for voice tests
// ---------------------------------------------------------------------------

class Player {
public:
    Player() { InitializeCriticalSection(&cs_); }
    ~Player() { close(); DeleteCriticalSection(&cs_); }

    bool open()
    {
        close();
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = POCKETTTS_CHANNELS;
        fmt.nSamplesPerSec = POCKETTTS_SAMPLE_RATE;
        fmt.wBitsPerSample = POCKETTTS_BITS;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        return waveOutOpen(&handle_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
    }

    void feed(const char* data, uint32_t size)
    {
        if (!handle_ || size == 0) {
            return;
        }
        EnterCriticalSection(&cs_);
        reap_done();
        auto* hdr = new WAVEHDR{};
        hdr->lpData = new char[size];
        memcpy(hdr->lpData, data, size);
        hdr->dwBufferLength = size;
        if (waveOutPrepareHeader(handle_, hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR &&
            waveOutWrite(handle_, hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            headers_.push_back(hdr);
        } else {
            delete[] hdr->lpData;
            delete hdr;
        }
        LeaveCriticalSection(&cs_);
    }

    bool playing()
    {
        EnterCriticalSection(&cs_);
        reap_done();
        const bool active = !headers_.empty();
        LeaveCriticalSection(&cs_);
        return active;
    }

    void stop()
    {
        EnterCriticalSection(&cs_);
        if (handle_) {
            waveOutReset(handle_);
            reap_done();
        }
        LeaveCriticalSection(&cs_);
    }

    void close()
    {
        EnterCriticalSection(&cs_);
        if (handle_) {
            waveOutReset(handle_);
            reap_done();
            waveOutClose(handle_);
            handle_ = nullptr;
        }
        LeaveCriticalSection(&cs_);
    }

private:
    void reap_done()
    {
        for (size_t i = 0; i < headers_.size();) {
            WAVEHDR* hdr = headers_[i];
            if (hdr->dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(handle_, hdr, sizeof(WAVEHDR));
                delete[] hdr->lpData;
                delete hdr;
                headers_.erase(headers_.begin() + i);
            } else {
                ++i;
            }
        }
    }

    HWAVEOUT handle_ = nullptr;
    std::vector<WAVEHDR*> headers_;
    CRITICAL_SECTION cs_;
};

// ---------------------------------------------------------------------------
// Main dialog: voice list
// ---------------------------------------------------------------------------

void refresh_voices_async(HWND dlg)
{
    post_status(dlg, L"Connecting to the Pocket TTS engine (the first start can take a minute)...");
    CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        HWND dlg = static_cast<HWND>(param);
        auto* voices = new std::vector<HostVoice>();
        std::wstring error;
        if (!g_client.listVoices(*voices, error)) {
            DEBUG_LOG("manager: listVoices failed: %S", error.c_str());
            delete voices;
            voices = nullptr;
            post_status(dlg, L"Error: " + error);
        } else {
            DEBUG_LOG("manager: listVoices returned %u voices", (unsigned)voices->size());
        }
        PostMessageW(dlg, WM_APP_VOICES, 0, reinterpret_cast<LPARAM>(voices));
        return 0;
    }, dlg, 0, nullptr);
}

void fill_voice_list(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_VOICE_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& v : g_voices) {
        std::wstring item = v.name;
        item += v.female ? L" (female, " : L" (male, ";
        item += v.published ? L"published)" : L"not published)";
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    if (!g_voices.empty()) {
        int select = 0;
        for (size_t i = 0; i < g_voices.size(); ++i) {
            if (g_voices[i].name == g_selectedVoice) {
                select = static_cast<int>(i);
                break;
            }
        }
        SendMessageW(list, LB_SETCURSEL, select, 0);
    }
}

const HostVoice* selected_voice(HWND dlg)
{
    const LRESULT index = SendDlgItemMessageW(dlg, IDC_VOICE_LIST, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR || static_cast<size_t>(index) >= g_voices.size()) {
        return nullptr;
    }
    return &g_voices[static_cast<size_t>(index)];
}

// ---------------------------------------------------------------------------
// Clone dialog
// ---------------------------------------------------------------------------

struct CloneParams {
    HWND dlg;
    std::wstring name;
    std::wstring file;
    bool female;
};

INT_PTR CALLBACK CloneDlgProc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    static bool cloning = false;

    switch (msg) {
    case WM_INITDIALOG: {
        cloning = false;
        HWND gender = GetDlgItem(dlg, IDC_GENDER);
        SendMessageW(gender, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Male"));
        SendMessageW(gender, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Female"));
        SendMessageW(gender, CB_SETCURSEL, 0, 0);
        return TRUE;
    }
    case WM_APP_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lparam);
        append_log(dlg, IDC_LOG, *text);
        delete text;
        return TRUE;
    }
    case WM_APP_DONE: {
        auto* error = reinterpret_cast<std::wstring*>(lparam);
        cloning = false;
        EnableWindow(GetDlgItem(dlg, IDOK), TRUE);
        if (wparam) {
            MessageBoxW(dlg,
                        L"The voice was cloned. It is not published yet: select it in the "
                        L"list and choose Publish to SAPI to make it available to other "
                        L"applications, or Test Voice to hear it first.",
                        L"Voice cloned", MB_OK | MB_ICONINFORMATION);
            EndDialog(dlg, IDOK);
        } else {
            MessageBoxW(dlg, error ? error->c_str() : L"Cloning failed.",
                        L"Cloning failed", MB_OK | MB_ICONERROR);
        }
        delete error;
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_BROWSE: {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = dlg;
            ofn.lpstrFilter = L"Audio files (*.wav;*.mp3;*.flac;*.ogg)\0*.wav;*.mp3;*.flac;*.ogg\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrTitle = L"Choose an audio sample of the voice";
            if (GetOpenFileNameW(&ofn)) {
                set_text(dlg, IDC_FILE, file);
            }
            return TRUE;
        }
        case IDOK: {
            if (cloning) {
                return TRUE;
            }
            auto* params = new CloneParams();
            params->dlg = dlg;
            params->name = get_text(dlg, IDC_NAME);
            params->file = get_text(dlg, IDC_FILE);
            params->female = SendDlgItemMessageW(dlg, IDC_GENDER, CB_GETCURSEL, 0, 0) == 1;
            if (params->name.empty() || params->file.empty()) {
                MessageBoxW(dlg, L"Please enter a voice name and choose an audio file.",
                            L"Missing information", MB_OK | MB_ICONWARNING);
                delete params;
                return TRUE;
            }
            cloning = true;
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            append_log(dlg, IDC_LOG, L"Starting the clone...");
            CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                auto* params = static_cast<CloneParams*>(p);
                DEBUG_LOG("manager: cloning \"%S\" from \"%S\" (%s)",
                          params->name.c_str(), params->file.c_str(),
                          params->female ? "female" : "male");
                std::wstring error;
                const bool ok = g_client.cloneVoice(
                    params->name, params->file, params->female, L"409",
                    [](const std::wstring& message, void* user) {
                        post_status(static_cast<HWND>(user), message);
                    },
                    params->dlg, error);
                DEBUG_LOG("manager: clone %s%S%s", ok ? "succeeded" : "failed: ",
                          ok ? L"" : error.c_str(), "");
                PostMessageW(params->dlg, WM_APP_DONE, ok ? 1 : 0,
                             reinterpret_cast<LPARAM>(ok ? nullptr : new std::wstring(error)));
                delete params;
                return 0;
            }, params, 0, nullptr);
            return TRUE;
        }
        case IDCANCEL:
            if (!cloning) {
                EndDialog(dlg, IDCANCEL);
            }
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Test dialog
// ---------------------------------------------------------------------------

struct TestContext {
    HWND dlg = nullptr;
    Player player;
    std::atomic<bool> stop{false};
    std::atomic<bool> speaking{false};
    std::wstring voice;
};

TestContext* g_test = nullptr;

bool test_audio_callback(const char* pcm, uint32_t size, void* user)
{
    auto* ctx = static_cast<TestContext*>(user);
    if (ctx->stop.load()) {
        return false;
    }
    ctx->player.feed(pcm, size);
    return true;
}

INT_PTR CALLBACK TestDlgProc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    static bool closePending = false;

    switch (msg) {
    case WM_INITDIALOG: {
        closePending = false;
        g_test->dlg = dlg;
        std::wstring caption = L"Test Voice: " + g_test->voice;
        SetWindowTextW(dlg, caption.c_str());
        std::wstring sample = L"Hello! This is " + g_test->voice +
            L" speaking through Kyutai Pocket TTS. I run completely on this computer.";
        set_text(dlg, IDC_TEXT, sample);
        return TRUE;
    }
    case WM_APP_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lparam);
        set_text(dlg, IDC_STATUS, *text);
        delete text;
        return TRUE;
    }
    case WM_APP_DONE:
        g_test->speaking.store(false);
        if (closePending) {
            // The user closed the dialog mid-speech; the worker has now
            // finished with the context, so it is safe to tear down.
            g_test->player.close();
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        EnableWindow(GetDlgItem(dlg, IDC_SPEAK), TRUE);
        set_text(dlg, IDC_STATUS, wparam ? L"Finished." : L"Speech failed; is the engine running?");
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_SPEAK: {
            if (g_test->speaking.load()) {
                return TRUE;
            }
            const std::wstring text = get_text(dlg, IDC_TEXT);
            if (text.empty()) {
                return TRUE;
            }
            g_test->stop.store(false);
            g_test->speaking.store(true);
            EnableWindow(GetDlgItem(dlg, IDC_SPEAK), FALSE);
            set_text(dlg, IDC_STATUS, L"Generating speech...");
            auto* textCopy = new std::wstring(text);
            CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                auto* text = static_cast<std::wstring*>(p);
                TestContext* ctx = g_test;
                bool ok = false;
                if (ctx->player.open()) {
                    ok = g_client.speak(
                        PocketTts::utils::wstring_to_string(ctx->voice),
                        PocketTts::utils::wstring_to_string(*text),
                        test_audio_callback, ctx);
                    // Let the queued audio finish before closing the device.
                    while (!ctx->stop.load() && ctx->player.playing()) {
                        Sleep(100);
                    }
                    ctx->player.close();
                }
                PostMessageW(ctx->dlg, WM_APP_DONE, ok ? 1 : 0, 0);
                delete text;
                return 0;
            }, textCopy, 0, nullptr);
            return TRUE;
        }
        case IDC_STOPBTN:
            g_test->stop.store(true);
            g_test->player.stop();
            set_text(dlg, IDC_STATUS, L"Stopped.");
            return TRUE;
        case IDCANCEL:
            g_test->stop.store(true);
            if (g_test->speaking.load()) {
                // Let the worker thread finish with the context first;
                // WM_APP_DONE completes the close.
                closePending = true;
                g_test->player.stop();
                set_text(dlg, IDC_STATUS, L"Stopping...");
                return TRUE;
            }
            g_test->player.close();
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Update dialog
// ---------------------------------------------------------------------------

INT_PTR CALLBACK UpdateDlgProc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    static bool updating = false;

    switch (msg) {
    case WM_INITDIALOG:
        updating = false;
        return TRUE;
    case WM_APP_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lparam);
        append_log(dlg, IDC_LOG, *text);
        delete text;
        return TRUE;
    }
    case WM_APP_DONE: {
        auto* error = reinterpret_cast<std::wstring*>(lparam);
        updating = false;
        EnableWindow(GetDlgItem(dlg, IDOK), TRUE);
        if (wparam) {
            MessageBoxW(dlg, L"The AI models are up to date and all voices were rebuilt.",
                        L"Update complete", MB_OK | MB_ICONINFORMATION);
            EndDialog(dlg, IDOK);
        } else {
            MessageBoxW(dlg, error ? error->c_str() : L"The update failed.",
                        L"Update failed", MB_OK | MB_ICONERROR);
        }
        delete error;
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDOK: {
            if (updating) {
                return TRUE;
            }
            updating = true;
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            append_log(dlg, IDC_LOG, L"Starting the model update...");
            auto* token = new std::pair<HWND, std::wstring>(dlg, get_text(dlg, IDC_TOKEN));
            CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                auto* params = static_cast<std::pair<HWND, std::wstring>*>(p);
                DEBUG_LOG("manager: model update started (token %s)",
                          params->second.empty() ? "absent" : "provided");
                std::wstring error;
                const bool ok = g_client.updateModels(
                    params->second,
                    [](const std::wstring& message, void* user) {
                        post_status(static_cast<HWND>(user), message);
                    },
                    params->first, error);
                PostMessageW(params->first, WM_APP_DONE, ok ? 1 : 0,
                             reinterpret_cast<LPARAM>(ok ? nullptr : new std::wstring(error)));
                delete params;
                return 0;
            }, token, 0, nullptr);
            return TRUE;
        }
        case IDCANCEL:
            if (!updating) {
                EndDialog(dlg, IDCANCEL);
            }
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Simple worker for publish / unpublish / delete
// ---------------------------------------------------------------------------

struct SimpleOp {
    HWND dlg;
    std::wstring voice;
    int op;  // 0 publish, 1 unpublish, 2 delete
};

void run_simple_op(HWND dlg, const std::wstring& voice, int op)
{
    g_busy = true;
    post_status(dlg, L"Working...");
    auto* params = new SimpleOp{dlg, voice, op};
    CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* params = static_cast<SimpleOp*>(p);
        DEBUG_LOG("manager: op %d on voice \"%S\"", params->op, params->voice.c_str());
        std::wstring error;
        bool ok = false;
        switch (params->op) {
        case 0: ok = g_client.setPublished(params->voice, true, error); break;
        case 1: ok = g_client.setPublished(params->voice, false, error); break;
        case 2: ok = g_client.deleteVoice(params->voice, error); break;
        }
        if (!ok) {
            DEBUG_LOG("manager: op %d failed: %S", params->op, error.c_str());
        }
        if (ok) {
            const wchar_t* done = params->op == 0
                ? L"Published. The voice is now available to all SAPI applications."
                : (params->op == 1 ? L"Unpublished. The voice was removed from the SAPI voice list."
                                   : L"The voice was deleted.");
            post_status(params->dlg, done);
        } else {
            post_status(params->dlg, L"Error: " + error);
        }
        PostMessageW(params->dlg, WM_APP_DONE, ok ? 1 : 0, 0);
        delete params;
        return 0;
    }, params, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Main dialog proc
// ---------------------------------------------------------------------------

INT_PTR CALLBACK MainDlgProc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_INITDIALOG:
        refresh_voices_async(dlg);
        return TRUE;
    case WM_APP_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lparam);
        set_text(dlg, IDC_STATUS, *text);
        delete text;
        return TRUE;
    }
    case WM_APP_VOICES: {
        auto* voices = reinterpret_cast<std::vector<HostVoice>*>(lparam);
        if (voices) {
            g_voices = std::move(*voices);
            delete voices;
            fill_voice_list(dlg);
            wchar_t status[64];
            swprintf_s(status, L"%zu voice(s).", g_voices.size());
            set_text(dlg, IDC_STATUS, status);
        }
        return TRUE;
    }
    case WM_APP_DONE:
        g_busy = false;
        refresh_voices_async(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_CLONE:
            if (!g_busy) {
                if (DialogBoxW(g_instance, MAKEINTRESOURCEW(IDD_CLONE), dlg, CloneDlgProc) == IDOK) {
                    refresh_voices_async(dlg);
                }
            }
            return TRUE;
        case IDC_TEST: {
            const HostVoice* voice = selected_voice(dlg);
            if (!voice) {
                MessageBoxW(dlg, L"Please select a voice in the list first.",
                            L"No voice selected", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (!g_busy) {
                TestContext ctx;
                ctx.voice = voice->name;
                g_test = &ctx;
                DialogBoxW(g_instance, MAKEINTRESOURCEW(IDD_TEST), dlg, TestDlgProc);
                g_test = nullptr;
            }
            return TRUE;
        }
        case IDC_PUBLISH:
        case IDC_UNPUBLISH: {
            const HostVoice* voice = selected_voice(dlg);
            if (!voice) {
                MessageBoxW(dlg, L"Please select a voice in the list first.",
                            L"No voice selected", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (!g_busy) {
                g_selectedVoice = voice->name;
                run_simple_op(dlg, voice->name, LOWORD(wparam) == IDC_PUBLISH ? 0 : 1);
            }
            return TRUE;
        }
        case IDC_DELETE: {
            const HostVoice* voice = selected_voice(dlg);
            if (!voice) {
                MessageBoxW(dlg, L"Please select a voice in the list first.",
                            L"No voice selected", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (!g_busy) {
                std::wstring prompt = L"Delete the voice \"" + voice->name +
                    L"\" permanently? This also removes it from the SAPI voice list.";
                if (MessageBoxW(dlg, prompt.c_str(), L"Delete voice",
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                    g_selectedVoice.clear();
                    run_simple_op(dlg, voice->name, 2);
                }
            }
            return TRUE;
        }
        case IDC_UPDATE:
            if (!g_busy) {
                if (DialogBoxW(g_instance, MAKEINTRESOURCEW(IDD_UPDATE), dlg, UpdateDlgProc) == IDOK) {
                    refresh_voices_async(dlg);
                }
            }
            return TRUE;
        case IDC_REFRESH:
            if (!g_busy) {
                refresh_voices_async(dlg);
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    g_instance = instance;
    DEBUG_LOG("manager: starting");
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    const INT_PTR result = DialogBoxW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainDlgProc);
    if (result == -1) {
        wchar_t message[128];
        swprintf_s(message, L"The main window could not be created (error %lu).", GetLastError());
        MessageBoxW(nullptr, message, L"Pocket TTS Voice Manager", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
