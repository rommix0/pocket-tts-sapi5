"""Pocket TTS SAPI5 host process.

Keeps the Kyutai pocket-tts model warm in memory and serves speech,
voice-cloning and voice-management requests over a framed TCP protocol on
localhost. Both the 32-bit and 64-bit SAPI5 engine DLLs and the Voice
Manager utility are clients of this process.

Protocol: every frame is  <uint32 type><uint32 payload_size><payload>,
little-endian, strings are UTF-8 length-prefixed inside payloads.
"""

import ctypes
import logging
import os
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
from logging.handlers import RotatingFileHandler
from pathlib import Path

# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------

def _data_dir() -> Path:
    override = os.environ.get("POCKETTTS_DATA_DIR")
    if override:
        return Path(override)
    program_data = os.environ.get("ProgramData", r"C:\ProgramData")
    return Path(program_data) / "PocketTTS"


def _local_dir() -> Path:
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        local = str(Path.home() / "AppData" / "Local")
    d = Path(local) / "PocketTTS"
    d.mkdir(parents=True, exist_ok=True)
    return d


DATA_DIR = _data_dir()
MODELS_DIR = DATA_DIR / "models"
VOICES_DIR = DATA_DIR / "voices"
VOICES_SRC_DIR = VOICES_DIR / "src"
VOICES_INI = VOICES_DIR / "voices.ini"
CONFIG_YAML = MODELS_DIR / "english" / "config.yaml"

PORTS = (17853, 17854, 17855, 17856, 17857)
PORT_FILE = _local_dir() / "host.port"
LOG_FILE = _local_dir() / "host.log"

# Commands
CMD_PING = 0
CMD_LIST_VOICES = 1
CMD_SPEAK = 2
CMD_STOP = 3
CMD_CLONE = 4
CMD_SET_PUBLISHED = 5
CMD_DELETE_VOICE = 6
CMD_UPDATE_MODELS = 7
CMD_SHUTDOWN = 8
CMD_INFO = 9

# Responses
RESP_OK = 0
RESP_ERROR = 1
RESP_AUDIO = 2
RESP_AUDIO_END = 3
RESP_VOICES = 4
RESP_PONG = 5
RESP_PROGRESS = 6
RESP_INFO = 7

logger = logging.getLogger("pockettts_host")


# ---------------------------------------------------------------------------
# Voice store (voices.ini, UTF-16-LE so GetPrivateProfileStringW reads it)
# ---------------------------------------------------------------------------

class VoiceStore:
    """voices.ini: one section per voice.

    Keys: file (relative safetensors), gender (Male/Female), language (hex
    LCID, e.g. 409), published (0/1), source (relative audio in src/ used to
    re-embed the voice after a model update; may be missing).
    """

    def __init__(self):
        self.lock = threading.Lock()

    def read(self) -> dict:
        voices = {}
        if not VOICES_INI.exists():
            return voices
        try:
            text = VOICES_INI.read_text(encoding="utf-16")
        except UnicodeError:
            text = VOICES_INI.read_text(encoding="utf-8-sig")
        section = None
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith(";"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                voices[section] = {}
            elif "=" in line and section is not None:
                key, _, value = line.partition("=")
                voices[section][key.strip().lower()] = value.strip()
        return voices

    def write(self, voices: dict) -> None:
        lines = []
        for name, props in voices.items():
            lines.append(f"[{name}]")
            for key, value in props.items():
                lines.append(f"{key}={value}")
            lines.append("")
        content = "\r\n".join(lines)
        fd, tmp = tempfile.mkstemp(dir=str(VOICES_DIR), suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-16", newline="") as f:
                f.write(content)
            os.replace(tmp, VOICES_INI)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise

    def update(self, mutate) -> None:
        with self.lock:
            voices = self.read()
            mutate(voices)
            self.write(voices)


# ---------------------------------------------------------------------------
# Engine wrapper with cancellable streaming generation
# ---------------------------------------------------------------------------

class Engine:
    def __init__(self):
        self.model = None
        self.gen_lock = threading.Lock()   # pocket-tts is not thread-safe
        self.state_cache = {}              # voice name -> model state
        self.state_cache_order = []
        self.load_error = None

    def load(self):
        try:
            from pocket_tts import TTSModel
            if not CONFIG_YAML.exists():
                raise FileNotFoundError(
                    f"Model config not found: {CONFIG_YAML}. "
                    "Reinstall or run a model update.")
            self._fix_config_paths()
            t0 = time.monotonic()
            self.model = TTSModel.load_model(config=str(CONFIG_YAML))
            logger.info("model loaded in %.1fs (voice cloning: %s)",
                        time.monotonic() - t0, self.model.has_voice_cloning)
            self.load_error = None
        except Exception as exc:
            logger.exception("model load failed")
            self.load_error = str(exc)

    def _fix_config_paths(self):
        """The bundled config references C:/ProgramData/PocketTTS; rewrite it
        when the data directory lives elsewhere (relocated ProgramData or a
        POCKETTTS_DATA_DIR override)."""
        canonical = "C:/ProgramData/PocketTTS"
        actual = str(DATA_DIR).replace("\\", "/")
        if actual.lower() == canonical.lower():
            return
        try:
            text = CONFIG_YAML.read_text(encoding="utf-8")
            if canonical in text:
                CONFIG_YAML.write_text(
                    text.replace(canonical, actual), encoding="utf-8")
                logger.info("rewrote model config paths to %s", actual)
        except OSError:
            logger.warning("could not rewrite config paths", exc_info=True)

    def warm_up(self):
        """First generation initialises lazy torch state; do it off the
        critical path so the first real utterance is fast."""
        try:
            voices = STORE.read()
            name = next(iter(voices), None)
            if name is None:
                return
            state = self.get_voice_state(name, voices)
            with self.gen_lock:
                for _ in self._stream_chunks(state, "Ready.", threading.Event()):
                    pass
            logger.info("warm-up generation done")
        except Exception:
            logger.exception("warm-up failed (non-fatal)")

    def get_voice_state(self, name: str, voices: dict):
        if name in self.state_cache:
            return self.state_cache[name]
        props = voices.get(name)
        if props is None:
            raise KeyError(f"unknown voice: {name}")
        path = VOICES_DIR / props.get("file", name + ".safetensors")
        if not path.exists():
            raise FileNotFoundError(f"voice file missing: {path}")
        state = self.model.get_state_for_audio_prompt(path)
        self.state_cache[name] = state
        self.state_cache_order.append(name)
        while len(self.state_cache_order) > 6:
            evicted = self.state_cache_order.pop(0)
            self.state_cache.pop(evicted, None)
        return state

    def drop_cached_state(self, name: str):
        self.state_cache.pop(name, None)
        if name in self.state_cache_order:
            self.state_cache_order.remove(name)

    def clear_cache(self):
        self.state_cache.clear()
        self.state_cache_order.clear()

    # -- generation ---------------------------------------------------------

    def _stream_chunks(self, model_state, text, cancel):
        """Yield int16 PCM byte chunks; honours `cancel` between model steps
        so screen-reader interruptions take effect within ~one frame."""
        from pocket_tts.default_parameters import MAX_TOKEN_PER_CHUNK
        from pocket_tts.models.text_chunking import (
            prepare_text_prompt, split_into_best_sentences)

        model = self.model
        chunks = split_into_best_sentences(
            model.flow_lm.conditioner.tokenizer, text, MAX_TOKEN_PER_CHUNK,
            model.pad_with_spaces_for_short_inputs,
            remove_semicolons=model.remove_semicolons)
        for chunk in chunks:
            if cancel.is_set():
                return
            prompt, guess = prepare_text_prompt(
                chunk, model.pad_with_spaces_for_short_inputs,
                model.remove_semicolons)
            frames_after_eos = model.model_recommended_frames_after_eos
            if frames_after_eos is None:
                frames_after_eos = guess + 2
            yield from self._stream_one(model_state, prompt,
                                        frames_after_eos, cancel)

    def _stream_one(self, model_state, text, frames_after_eos, cancel):
        """Cancellable re-implementation of pocket-tts
        _generate_audio_stream_short_text (pinned to pocket-tts 3.0.2):
        latents are produced on this thread and decoded by a worker so
        throughput matches upstream, but both loops watch `cancel`."""
        import copy as _copy
        import queue as _queue

        import torch
        from pocket_tts.modules.stateful_module import (
            increment_steps, init_states)

        model = self.model
        model_state = _copy.deepcopy(model_state)

        prepared = model.flow_lm.conditioner.prepare(text)
        token_count = prepared.tokens.shape[1]
        max_gen_len = model._estimate_max_gen_len(token_count)
        mimi_steps = int(model.mimi.encoder_frame_rate / model.mimi.frame_rate)
        mimi_seq_len = max_gen_len * mimi_steps

        current_end = model._flow_lm_current_end(model_state)
        model._expand_kv_cache(
            model_state, sequence_length=current_end + token_count + max_gen_len)
        model._run_flow_lm_and_increment_step(
            model_state=model_state, text_tokens=prepared.tokens)

        latents_q = _queue.Queue()
        out_q = _queue.Queue()

        def decoder():
            try:
                mimi_state = init_states(model.mimi, batch_size=1,
                                         sequence_length=mimi_seq_len)
                while True:
                    latent = latents_q.get()
                    if latent is None or cancel.is_set():
                        break
                    decoding_input = (latent * model.flow_lm.emb_std
                                      + model.flow_lm.emb_mean)
                    frame = model.mimi.decode_from_latent(
                        decoding_input, mimi_state)
                    increment_steps(model.mimi, mimi_state,
                                    increment=mimi_steps)
                    pcm = (frame[0, 0].clamp(-1, 1) * 32767).short()
                    out_q.put(("chunk", pcm.numpy().tobytes()))
            except Exception as exc:   # noqa: BLE001
                out_q.put(("error", exc))
            finally:
                out_q.put(("done", None))

        decoder_thread = threading.Thread(target=decoder, daemon=True)
        decoder_thread.start()

        def producer():
            try:
                with torch.no_grad():
                    backbone = torch.full(
                        (1, 1, model.flow_lm.ldim), float("NaN"),
                        device=next(iter(model.flow_lm.parameters())).device,
                        dtype=model.flow_lm.dtype)
                    eos_step = None
                    for step in range(max_gen_len):
                        if cancel.is_set():
                            break
                        next_latent, is_eos = \
                            model._run_flow_lm_and_increment_step(
                                model_state=model_state,
                                backbone_input_latents=backbone)
                        if is_eos.item() and eos_step is None:
                            eos_step = step
                        if (eos_step is not None
                                and step >= eos_step + frames_after_eos):
                            break
                        latents_q.put(next_latent)
                        backbone = next_latent
            except Exception as exc:   # noqa: BLE001
                out_q.put(("error", exc))
            finally:
                latents_q.put(None)

        producer_thread = threading.Thread(target=producer, daemon=True)
        producer_thread.start()

        try:
            while True:
                kind, value = out_q.get()
                if kind == "chunk":
                    if not cancel.is_set():
                        yield value
                elif kind == "done":
                    break
                else:
                    raise value
        except GeneratorExit:
            cancel.set()
            raise
        finally:
            producer_thread.join(timeout=10)
            decoder_thread.join(timeout=10)

    def speak(self, voice_name, text, cancel, emit):
        """Generate `text` with `voice_name`, calling emit(pcm_bytes)."""
        if self.model is None:
            raise RuntimeError(self.load_error or "model not loaded")
        t0 = time.monotonic()
        voices = STORE.read()
        state = self.get_voice_state(voice_name, voices)
        emitted = 0
        with self.gen_lock:
            if cancel.is_set():
                logger.debug("speak cancelled before generation started")
                return
            for pcm in self._stream_chunks(state, text, cancel):
                if cancel.is_set():
                    break
                emitted += len(pcm)
                emit(pcm)
        logger.info(
            "speak: voice %r, %d chars -> %d ms audio in %d ms%s",
            voice_name, len(text), int(emitted / 2 / 24000 * 1000),
            int((time.monotonic() - t0) * 1000),
            " (cancelled)" if cancel.is_set() else "")

    # -- cloning ------------------------------------------------------------

    def clone(self, name, audio_path, gender, language, progress):
        if self.model is None:
            raise RuntimeError(self.load_error or "model not loaded")
        if not self.model.has_voice_cloning:
            raise RuntimeError(
                "The installed model does not support voice cloning.")
        src = Path(audio_path)
        if not src.exists():
            raise FileNotFoundError(f"audio file not found: {src}")
        safe = _sanitize_name(name)
        if not safe:
            raise ValueError("voice name is empty or invalid")
        voices = STORE.read()
        if safe in voices:
            raise ValueError(f'a voice named "{safe}" already exists')

        from pocket_tts.models.model_state import export_model_state
        progress("Analyzing the audio sample (this can take a minute)...")
        with self.gen_lock:
            state = self.model.get_state_for_audio_prompt(src, truncate=True)
        progress("Saving the voice...")
        VOICES_DIR.mkdir(parents=True, exist_ok=True)
        VOICES_SRC_DIR.mkdir(parents=True, exist_ok=True)
        export_model_state(state, VOICES_DIR / f"{safe}.safetensors")
        src_copy = VOICES_SRC_DIR / (safe + src.suffix.lower())
        shutil.copy(src, src_copy)

        def mutate(voices):
            voices[safe] = {
                "file": f"{safe}.safetensors",
                "gender": "Female" if gender == 1 else "Male",
                "language": language or "409",
                "published": "0",
                "source": f"src/{src_copy.name}",
            }
        STORE.update(mutate)
        self.state_cache[safe] = state
        self.state_cache_order.append(safe)
        return safe

    # -- model update -------------------------------------------------------

    def update_models(self, token, progress):
        from huggingface_hub import hf_hub_download

        token = token or None
        english = MODELS_DIR / "english"
        english.mkdir(parents=True, exist_ok=True)

        progress("Downloading the latest English model from Hugging Face...")
        model_file = hf_hub_download(
            repo_id="kyutai/pocket-tts",
            filename="languages/english/model.safetensors", token=token)
        progress("Downloading the tokenizer...")
        tok_file = hf_hub_download(
            repo_id="kyutai/pocket-tts-without-voice-cloning",
            filename="languages/english/tokenizer.model", token=token)

        progress("Installing the new model files...")
        shutil.copy(model_file, english / "model.safetensors.new")
        os.replace(english / "model.safetensors.new",
                   english / "model.safetensors")
        shutil.copy(tok_file, english / "tokenizer.model.new")
        os.replace(english / "tokenizer.model.new",
                   english / "tokenizer.model")

        progress("Loading the new model...")
        with self.gen_lock:
            self.clear_cache()
            self.load()
        if self.model is None:
            raise RuntimeError(f"new model failed to load: {self.load_error}")

        # Voice states are tied to the model weights, so re-embed every
        # voice that still has its source audio.
        voices = STORE.read()
        from pocket_tts.models.model_state import export_model_state
        for name, props in voices.items():
            source = props.get("source", "")
            if not source:
                progress(f'Voice "{name}" has no source audio; leaving as is.')
                continue
            src = VOICES_DIR / source
            if not src.exists():
                progress(f'Source audio for "{name}" is missing; leaving as is.')
                continue
            progress(f'Rebuilding voice "{name}" for the new model...')
            with self.gen_lock:
                state = self.model.get_state_for_audio_prompt(
                    src, truncate=True)
            export_model_state(
                state, VOICES_DIR / props.get("file", name + ".safetensors"))
        self.clear_cache()
        progress("Model update complete.")


def _sanitize_name(name: str) -> str:
    bad = set('[]=;\r\n\t"\\/|<>:*?')
    cleaned = "".join(c for c in name if c not in bad).strip()
    return cleaned[:60]


STORE = VoiceStore()
ENGINE = Engine()


# ---------------------------------------------------------------------------
# Wire helpers
# ---------------------------------------------------------------------------

def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        part = sock.recv(n - len(buf))
        if not part:
            raise ConnectionError("peer closed")
        buf += part
    return buf


def _read_str16(payload, offset):
    (length,) = struct.unpack_from("<H", payload, offset)
    offset += 2
    value = payload[offset:offset + length].decode("utf-8", "replace")
    return value, offset + length


def _read_str32(payload, offset):
    (length,) = struct.unpack_from("<I", payload, offset)
    offset += 4
    value = payload[offset:offset + length].decode("utf-8", "replace")
    return value, offset + length


class Connection:
    def __init__(self, sock, addr, server):
        self.sock = sock
        self.server = server
        self.send_lock = threading.Lock()
        self.cancel = threading.Event()
        self.speak_thread = None
        self.addr = addr

    def send(self, resp_type, payload=b""):
        frame = struct.pack("<II", resp_type, len(payload)) + payload
        with self.send_lock:
            self.sock.sendall(frame)

    def send_text(self, resp_type, text):
        self.send(resp_type, text.encode("utf-8"))

    def run(self):
        try:
            while True:
                header = _recv_exact(self.sock, 8)
                cmd, size = struct.unpack("<II", header)
                payload = _recv_exact(self.sock, size) if size else b""
                if not self.dispatch(cmd, payload):
                    break
        except (ConnectionError, OSError):
            pass
        finally:
            self.cancel.set()
            if self.speak_thread and self.speak_thread.is_alive():
                self.speak_thread.join(timeout=5)
            try:
                self.sock.close()
            except OSError:
                pass

    def dispatch(self, cmd, payload):
        if cmd == CMD_PING:
            self.send(RESP_PONG)
        elif cmd == CMD_STOP:
            logger.debug("STOP received from %s", self.addr)
            self.cancel.set()
            self.send(RESP_OK)
        elif cmd == CMD_SPEAK:
            self.handle_speak(payload)
        elif cmd == CMD_LIST_VOICES:
            self.handle_list()
        elif cmd == CMD_CLONE:
            self.handle_clone(payload)
        elif cmd == CMD_SET_PUBLISHED:
            self.handle_set_published(payload)
        elif cmd == CMD_DELETE_VOICE:
            self.handle_delete(payload)
        elif cmd == CMD_UPDATE_MODELS:
            self.handle_update(payload)
        elif cmd == CMD_INFO:
            self.handle_info()
        elif cmd == CMD_SHUTDOWN:
            self.send(RESP_OK)
            self.server.shutdown_requested.set()
            return False
        else:
            self.send_text(RESP_ERROR, f"unknown command {cmd}")
        return True

    def handle_speak(self, payload):
        try:
            voice, offset = _read_str16(payload, 0)
            text, _ = _read_str32(payload, offset)
        except (struct.error, IndexError):
            self.send_text(RESP_ERROR, "malformed SPEAK payload")
            return
        # A previous utterance on this connection must be finished or
        # cancelled before a new one starts.
        if self.speak_thread and self.speak_thread.is_alive():
            self.cancel.set()
            self.speak_thread.join(timeout=15)
        self.cancel = threading.Event()
        cancel = self.cancel

        def work():
            try:
                ENGINE.speak(voice, text, cancel,
                             lambda pcm: self.send(RESP_AUDIO, pcm))
                self.send(RESP_AUDIO_END)
            except Exception as exc:   # noqa: BLE001
                logger.exception("speak failed")
                try:
                    self.send_text(RESP_ERROR, str(exc))
                    self.send(RESP_AUDIO_END)
                except OSError:
                    pass

        self.speak_thread = threading.Thread(target=work, daemon=True)
        self.speak_thread.start()

    def handle_list(self):
        voices = STORE.read()
        parts = [struct.pack("<I", len(voices))]
        for name, props in voices.items():
            encoded = name.encode("utf-8")[:255]
            gender = 1 if props.get("gender", "").lower() == "female" else 0
            try:
                lang = int(props.get("language", "409"), 16)
            except ValueError:
                lang = 0x409
            published = 1 if props.get("published", "0") == "1" else 0
            has_src = 1 if props.get("source") else 0
            parts.append(struct.pack("<H", len(encoded)) + encoded
                         + struct.pack("<BIBB", gender, lang, published,
                                       has_src))
        self.send(RESP_VOICES, b"".join(parts))

    def handle_clone(self, payload):
        try:
            name, offset = _read_str16(payload, 0)
            path, offset = _read_str16(payload, offset)
            lang, offset = _read_str16(payload, offset)
            (gender,) = struct.unpack_from("<B", payload, offset)
        except (struct.error, IndexError):
            self.send_text(RESP_ERROR, "malformed CLONE payload")
            return
        try:
            ENGINE.clone(name, path, gender, lang,
                         lambda msg: self.send_text(RESP_PROGRESS, msg))
            self.send(RESP_OK)
        except Exception as exc:   # noqa: BLE001
            logger.exception("clone failed")
            self.send_text(RESP_ERROR, str(exc))

    def handle_set_published(self, payload):
        try:
            name, offset = _read_str16(payload, 0)
            (published,) = struct.unpack_from("<B", payload, offset)
        except (struct.error, IndexError):
            self.send_text(RESP_ERROR, "malformed payload")
            return

        found = []

        def mutate(voices):
            if name in voices:
                voices[name]["published"] = "1" if published else "0"
                found.append(True)
        try:
            STORE.update(mutate)
        except Exception as exc:   # noqa: BLE001
            self.send_text(RESP_ERROR, str(exc))
            return
        if found:
            self.send(RESP_OK)
        else:
            self.send_text(RESP_ERROR, f'no voice named "{name}"')

    def handle_delete(self, payload):
        try:
            name, _ = _read_str16(payload, 0)
        except (struct.error, IndexError):
            self.send_text(RESP_ERROR, "malformed payload")
            return
        removed = {}

        def mutate(voices):
            if name in voices:
                removed.update(voices.pop(name))
        try:
            STORE.update(mutate)
        except Exception as exc:   # noqa: BLE001
            self.send_text(RESP_ERROR, str(exc))
            return
        if not removed:
            self.send_text(RESP_ERROR, f'no voice named "{name}"')
            return
        ENGINE.drop_cached_state(name)
        for rel in (removed.get("file"), removed.get("source")):
            if rel:
                try:
                    (VOICES_DIR / rel).unlink(missing_ok=True)
                except OSError:
                    logger.warning("could not delete %s", rel)
        self.send(RESP_OK)

    def handle_update(self, payload):
        try:
            token, _ = _read_str16(payload, 0)
        except (struct.error, IndexError):
            token = ""
        try:
            ENGINE.update_models(
                token, lambda msg: self.send_text(RESP_PROGRESS, msg))
            self.send(RESP_OK)
        except Exception as exc:   # noqa: BLE001
            logger.exception("model update failed")
            hint = ""
            text = str(exc)
            if "401" in text or "403" in text or "gated" in text.lower():
                hint = ("  The Kyutai model requires a (free) Hugging Face "
                        "account: accept the terms at "
                        "https://huggingface.co/kyutai/pocket-tts and paste "
                        "a read token into the update dialog.")
            self.send_text(RESP_ERROR, text + hint)

    def handle_info(self):
        import pocket_tts
        version = getattr(pocket_tts, "__version__", "3.x")
        state = "ready" if ENGINE.model is not None else (
            f"model not loaded: {ENGINE.load_error}")
        info = (f"pocket-tts {version}; engine {state}; "
                f"data dir {DATA_DIR}; sample rate 24000 Hz")
        self.send_text(RESP_INFO, info)


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------

class Server:
    def __init__(self):
        self.shutdown_requested = threading.Event()
        self.listener = None
        self.port = None

    def bind(self):
        for port in PORTS:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            try:
                sock.bind(("127.0.0.1", port))
                sock.listen(8)
            except OSError:
                sock.close()
                continue
            self.listener = sock
            self.port = port
            return True
        return False

    def serve(self):
        PORT_FILE.write_text(str(self.port), encoding="ascii")
        logger.info("listening on 127.0.0.1:%d", self.port)
        self.listener.settimeout(1.0)
        while not self.shutdown_requested.is_set():
            try:
                sock, addr = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn = Connection(sock, addr, self)
            threading.Thread(target=conn.run, daemon=True).start()
        logger.info("shutting down")
        try:
            self.listener.close()
        except OSError:
            pass


def _already_running() -> bool:
    """Another host instance owns one of our ports and answers PING."""
    candidates = []
    try:
        candidates.append(int(PORT_FILE.read_text(encoding="ascii").strip()))
    except (OSError, ValueError):
        pass
    candidates.extend(p for p in PORTS if p not in candidates)
    for port in candidates:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3) as s:
                s.sendall(struct.pack("<II", CMD_PING, 0))
                header = _recv_exact(s, 8)
                resp, _ = struct.unpack("<II", header)
                if resp == RESP_PONG:
                    return True
        except OSError:
            continue
    return False


def main():
    handler = RotatingFileHandler(LOG_FILE, maxBytes=2_000_000, backupCount=2,
                                  encoding="utf-8")
    level = getattr(logging,
                    os.environ.get("POCKETTTS_LOG_LEVEL", "INFO").upper(),
                    logging.INFO)
    logging.basicConfig(
        level=level, handlers=[handler],
        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    logger.info("host starting; data dir %s; python %s", DATA_DIR, sys.version)

    if _already_running():
        logger.info("another host instance is already running; exiting")
        return 0

    server = Server()
    if not server.bind():
        logger.error("all ports busy but no host answered PING; exiting")
        return 1

    # Give this process a recognisable description for Task Manager users.
    try:
        ctypes.windll.kernel32.SetConsoleTitleW("Pocket TTS engine host")
    except OSError:
        pass

    ENGINE.load()
    threading.Thread(target=ENGINE.warm_up, daemon=True).start()
    server.serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
