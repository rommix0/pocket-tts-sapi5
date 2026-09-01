"""Builds the default-voice payload the installer bundles.

Downloads a handful of permissively licensed voice samples from Kyutai's
public kyutai/tts-voices repository, embeds each one with the *bundled*
model weights (so the embeddings always match the shipped model), and
writes them plus voices.ini into installer/staging/voices.

Run with the dev venv from the repository root:
    %USERPROFILE%\\.pockettts\\venv\\Scripts\\python.exe installer\\prepare_voices.py
"""

import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STAGING = ROOT / "installer" / "staging"
MODEL_CONFIG = Path(r"C:/ProgramData/PocketTTS/models/english/config.yaml")

# name -> (hf path in kyutai/tts-voices, gender)
DEFAULT_VOICES = {
    "Alba": ("alba-mackenna/casual.wav", "Female"),
    "Jane": ("vctk/p339_023_enhanced.wav", "Female"),
    "George": ("vctk/p315_023_enhanced.wav", "Male"),
    "Michael": ("vctk/p360_023_enhanced.wav", "Male"),
}


def main():
    from huggingface_hub import hf_hub_download
    from pocket_tts import TTSModel
    from pocket_tts.models.model_state import export_model_state

    voices_dir = STAGING / "voices"
    src_dir = voices_dir / "src"
    src_dir.mkdir(parents=True, exist_ok=True)

    print("Loading the bundled model...")
    model = TTSModel.load_model(config=str(MODEL_CONFIG))

    ini_lines = []
    for name, (hf_path, gender) in DEFAULT_VOICES.items():
        print(f"Embedding {name} from {hf_path} ...")
        wav = Path(hf_hub_download(repo_id="kyutai/tts-voices", filename=hf_path))
        state = model.get_state_for_audio_prompt(wav, truncate=True)
        export_model_state(state, voices_dir / f"{name}.safetensors")
        src_copy = src_dir / (name + wav.suffix.lower())
        shutil.copy(wav, src_copy)
        ini_lines += [
            f"[{name}]",
            f"file={name}.safetensors",
            f"gender={gender}",
            "language=409",
            "published=1",
            f"source=src/{src_copy.name}",
            "",
        ]

    with open(voices_dir / "voices.ini", "w", encoding="utf-16", newline="") as f:
        f.write("\r\n".join(ini_lines))

    print("Staging the model files...")
    models_dir = STAGING / "models" / "english"
    models_dir.mkdir(parents=True, exist_ok=True)
    for filename in ("config.yaml", "model.safetensors", "tokenizer.model"):
        shutil.copy(MODEL_CONFIG.parent / filename, models_dir / filename)

    # The bundled config carries C:/ProgramData/PocketTTS paths; the host
    # rewrites them at startup if the data directory differs on a machine.
    print("Done. Staged payload in", STAGING)


if __name__ == "__main__":
    sys.exit(main())
