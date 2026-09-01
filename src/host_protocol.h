#pragma once

// Wire protocol between the SAPI engine DLLs / Voice Manager and the
// Pocket TTS host process (host/pockettts_host.py). Framed TCP on
// 127.0.0.1: every frame is <uint32 type><uint32 payload_size><payload>,
// little-endian; strings inside payloads are UTF-8 and length-prefixed.

#include <stdint.h>

enum HostCommand : uint32_t {
    CMD_PING = 0,
    CMD_LIST_VOICES = 1,
    CMD_SPEAK = 2,          // <u16 voice_len><voice utf8><u32 text_len><text utf8>
    CMD_STOP = 3,
    CMD_CLONE = 4,          // <u16+name><u16+audio_path><u16+language_hex><u8 gender>
    CMD_SET_PUBLISHED = 5,  // <u16+name><u8 published>
    CMD_DELETE_VOICE = 6,   // <u16+name>
    CMD_UPDATE_MODELS = 7,  // <u16+hf_token (may be empty)>
    CMD_SHUTDOWN = 8,
    CMD_INFO = 9,
};

enum HostResponse : uint32_t {
    RESP_OK = 0,
    RESP_ERROR = 1,       // payload: utf8 message
    RESP_AUDIO = 2,       // payload: raw PCM, 16-bit mono 24000 Hz
    RESP_AUDIO_END = 3,
    RESP_VOICES = 4,      // <u32 count>{<u16+name><u8 female><u32 lcid><u8 published><u8 has_src>}
    RESP_PONG = 5,
    RESP_PROGRESS = 6,    // payload: utf8 message
    RESP_INFO = 7,        // payload: utf8 message
};

// Host candidate ports, tried in order; the active one is written to
// %LOCALAPPDATA%\PocketTTS\host.port.
constexpr unsigned short POCKETTTS_PORTS[] = {17853, 17854, 17855, 17856, 17857};
constexpr int POCKETTTS_PORT_COUNT = 5;

constexpr unsigned long POCKETTTS_SAMPLE_RATE = 24000;
constexpr unsigned short POCKETTTS_CHANNELS = 1;
constexpr unsigned short POCKETTTS_BITS = 16;
