# Pocket TTS SAPI5

A native Windows SAPI5 wrapper for **[Pocket TTS](https://github.com/kyutai-labs/pocket-tts)**, the neural text-to-speech engine by **[Kyutai](https://kyutai.org)** — with voice cloning built in. It makes Pocket TTS voices, including voices you clone yourself from a WAV or MP3 sample, available to every SAPI5 application on Windows: screen readers (NVDA, JAWS, Windows Narrator), reading tools (Balabolka, Bookworm), and anything else that speaks through SAPI5.

> **Note:** This is early-stage software under active development. Please report any problems you encounter by [opening an issue](../../issues).

## About Pocket TTS

Pocket TTS is a lightweight neural text-to-speech model created by [Kyutai](https://kyutai.org), a non-profit AI research lab in Paris. All credit for the speech engine and the AI models goes to Kyutai and the Pocket TTS authors: **Manu Orsini, Simon Rouard, Gabriel De Marmiesse, Václav Volhejn, Neil Zeghidour, and Alexandre Défossez**.

- 100-million-parameter model that runs entirely on your CPU — no cloud, no GPU, no API keys
- ~200 ms latency to the first audio chunk, several times faster than realtime on a modern CPU
- Voice cloning from a short audio sample
- [Pocket TTS on GitHub](https://github.com/kyutai-labs/pocket-tts) (MIT license) · [Model card on Hugging Face](https://huggingface.co/kyutai/pocket-tts) (weights CC-BY-4.0) · [Tech report](https://kyutai.org/blog/2026-01-13-pocket-tts) · [Paper](https://arxiv.org/abs/2509.06926)

This project wraps that engine in a native SAPI5 interface; the wrapper itself is an independent community project and is not affiliated with or endorsed by Kyutai.

## Features

- **32-bit and 64-bit SAPI5 support** from a single always-warm engine process
- **Fast**: ~100 ms to first audio once warm; speech cancellation in ~20 ms (screen-reader friendly)
- **Voice cloning**: clone any voice from 10+ seconds of clean audio using the accessible Voice Manager
- **Instant publish / unpublish / delete** of voices in the SAPI voice list — no reboots, no registry hacking
- **AI model updates** from Hugging Face inside the Voice Manager, with all cloned voices rebuilt automatically
- **Rate (0.33x–3x), pitch, and volume** control via [sonic](https://github.com/waywardgeek/sonic) time-stretching, applied instantly even mid-utterance
- **Fully accessible**: the Voice Manager uses only standard Win32 dialogs, every control in the tab order
- **No Python required** for end users — the installer bundles a self-contained runtime
- **Detailed logs** for debugging (see [Logs](#logs-and-troubleshooting))

## Download and install

**Download `PocketTTS_SAPI5_Setup.exe` from the [Releases](../../releases) page.**

The installer bundles everything: both SAPI5 engine DLLs, the Voice Manager, the English Pocket TTS model, four default voices (Alba, Jane, George, Michael), and a self-contained Python runtime. During setup you can choose to put the Voice Manager on your desktop and to start the speech engine automatically at sign-in (recommended — speech then starts instantly).

After installation the Pocket TTS voices appear in every SAPI5 application. In NVDA: NVDA menu → Preferences → Settings → Speech → synthesizer **SAPI5**, then pick a Pocket TTS voice.

> **Why is the installer required?** GitHub does not allow files over 100 MB in a repository, and the AI model (~220 MB) and the bundled Python runtime (~600 MB) are far past that. Cloning this repository gives you all source code, the compiled wrapper binaries, and the default voice files — the model and runtime ship in the installer, or can be fetched with the scripts in `installer\` (see [Building from source](#building-from-source)).

## Cloning your own voices

Open the **Pocket TTS Voice Manager**, choose **Clone New Voice**, pick a WAV or MP3 with at least 10 seconds (up to 30 seconds is used) of clean, single-speaker speech, name the voice, and clone. Test it with **Test Voice**, then **Publish to SAPI** to make it available to all applications. Unpublish or delete it at any time.

> **Important — consent:** Kyutai's use policy prohibits voice impersonation or cloning without the speaker's **explicit and lawful consent**. Only clone voices you have permission to clone. See the [prohibited-use section](https://github.com/kyutai-labs/pocket-tts#prohibited-use) of the Pocket TTS README.

## Updating the AI model (and getting a Hugging Face token)

The Voice Manager's **Update AI Models** button downloads the newest Pocket TTS English model from Hugging Face and automatically rebuilds all of your cloned voices for it. The full model with voice cloning is free, but Kyutai asks you to accept its terms first, so the update needs a Hugging Face access token. Here is exactly how to get one:

1. **Create a free Hugging Face account** at [huggingface.co/join](https://huggingface.co/join) (skip if you have one) and sign in.
2. **Accept the model terms**: open the model page at [huggingface.co/kyutai/pocket-tts](https://huggingface.co/kyutai/pocket-tts). Near the top there is a form titled "You need to agree to share your contact information to access this model". Fill in the two fields (your organization or just "Personal", and what you want to use it for) and press **Agree and access repository**. The page will then say "You have been granted access to this model".
3. **Create a read token**: go to [huggingface.co/settings/tokens](https://huggingface.co/settings/tokens), choose **Create new token**, pick the **Read** token type, give it any name (for example "pocket-tts"), and press **Create token**. Copy the token — it starts with `hf_`.
4. **Run the update**: in the Voice Manager, press **Update AI Models**, paste the token into the "Hugging Face access token" field, and press **Update Now**. Progress is announced in the dialog; when it finishes, all your voices — cloned and default — have been rebuilt for the new model.

The token stays on Hugging Face's side of the connection; this software only uses it for the download and does not store it.

## Training your own model

Kyutai has released the full training code, so you can train a brand-new Pocket TTS model (for example for a new language or a specialized domain) and use it with this wrapper:

1. Start with Kyutai's training guide: [`training/README.md`](https://github.com/kyutai-labs/pocket-tts/blob/main/training/README.md) in the Pocket TTS repository. Training requires a GPU machine and a dataset of transcribed speech; the guide covers data preparation (manifest files with forced alignment), the training configuration, and running the trainer.
2. Training produces a **config YAML** plus **model weights** (a `.safetensors` export, or a `.pt` checkpoint you can export). The Pocket TTS CLI can test them directly: `pocket-tts generate --config your_model.yaml`.
3. To use a custom model with this wrapper, replace the files in `C:\ProgramData\PocketTTS\models\english\`:
   - `model.safetensors` — your trained weights
   - `tokenizer.model` — the sentencepiece tokenizer used in training
   - `config.yaml` — your model config, with `weights_path` and `lookup_table.tokenizer_path` pointing at those two files (see the shipped `config.yaml` for the expected shape)
   
   Then restart the engine (sign out and back in, or end `PocketTTSHost.exe` in Task Manager — it restarts on demand) and **re-clone your voices from their audio samples**: voice embeddings are tied to the model weights, so voices made with a different model will not work.
4. Community-trained models can also be published on Hugging Face and shared — see [Models trained by the community](https://github.com/kyutai-labs/pocket-tts#models-trained-by-the-community) for how Kyutai lists them.

## Building from source

Requirements: Windows 10+, Visual Studio 2022 (or Build Tools) with C++, CMake 3.15+, Python 3.10 x64, Inno Setup 6.

1. Clone [kyutai-labs/pocket-tts](https://github.com/kyutai-labs/pocket-tts) into `bin\pocket-tts`.
2. Create a Python 3.10 venv at `%USERPROFILE%\.pockettts\venv` and run `pip install "bin\pocket-tts[audio]"`.
3. Place the English model in `C:\ProgramData\PocketTTS\models\english\` (`config.yaml`, `model.safetensors`, `tokenizer.model`). The weights come from Hugging Face — accept the terms at [kyutai/pocket-tts](https://huggingface.co/kyutai/pocket-tts) first (see the token guide above); the tokenizer is in [kyutai/pocket-tts-without-voice-cloning](https://huggingface.co/kyutai/pocket-tts-without-voice-cloning).
4. Build:

```batch
build_all.bat
build_installer.bat
```

`build_all.bat` produces the SAPI DLLs and the Voice Manager in `output\`. `build_installer.bat` assembles the embedded Python runtime (`installer\prepare_runtime.ps1`), stages the default voices (`installer\prepare_voices.py`), and compiles `output\PocketTTS_SAPI5_Setup.exe`.

## Architecture

```
SAPI application (32- or 64-bit)
  └─ PocketTTSSAPI.dll        SAPI5 engine + dynamic voice enumerator (TokenEnums)
       └─ TCP 127.0.0.1       framed protocol, host launched on demand
            └─ PocketTTSHost.exe (embedded Python + pocket-tts, model kept warm in RAM)
                 ├─ C:\ProgramData\PocketTTS\models\english\   model weights
                 └─ C:\ProgramData\PocketTTS\voices\           voices.ini + voice states
```

Voices live in `voices.ini`; the enumerator DLL reads it directly, so publishing or removing a voice takes effect immediately with no registry changes. Rate, pitch, and volume are applied on the client side with sonic, so the model always speaks at its natural pace and rate changes are instant.

## Logs and troubleshooting

Everything logs, to make bug reports useful:

| Component | Log location |
|---|---|
| SAPI engine DLLs & Voice Manager | `%LOCALAPPDATA%\PocketTTS\logs\<application>-<x86/x64>.log` |
| Engine host process | `%LOCALAPPDATA%\PocketTTS\host.log` (set `POCKETTTS_LOG_LEVEL=DEBUG` for more) |
| Installer | `C:\ProgramData\PocketTTS\logs\setup.log` |

When reporting an issue, please include your Windows version, whether the application is 32- or 64-bit, and the relevant logs.

## Credits and licenses

- **Pocket TTS engine and models**: [Kyutai](https://kyutai.org) — Manu Orsini, Simon Rouard, Gabriel De Marmiesse, Václav Volhejn, Neil Zeghidour, Alexandre Défossez. Code MIT, model weights CC-BY-4.0.
- **Default voice samples**: from [kyutai/tts-voices](https://huggingface.co/kyutai/tts-voices); see that repository for per-voice licenses.
- **Time-stretching**: [sonic](https://github.com/waywardgeek/sonic) by Bill Cox (Apache-2.0).
- **SAPI5 wrapper, engine host, and Voice Manager**: this repository.
