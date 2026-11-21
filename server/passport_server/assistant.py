"""Voice assistant pipeline: speech recognition -> language model -> speech synthesis.

Providers (config.toml):
  [asr] provider = "mlx_whisper" | "faster_whisper" | "openai"   (default mlx_whisper)
  [llm] provider = "anthropic" | "openai_compatible"              (default anthropic)
  [tts] provider = "say" | "openai" | "none"                       (default say, macOS built-in)

Audio in and out is 16 kHz mono 16-bit PCM; the device side is IMA ADPCM (adpcm.py).
"""

from __future__ import annotations

import asyncio
import logging
import os
import re
import shutil
import subprocess
import tempfile
import time
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import aiohttp

from . import adpcm

log = logging.getLogger("assistant")
RATE = 16000

SYSTEM_PROMPT = (
    "你是一个装在随身小设备里的中文语音助手,回答会被朗读出来,并显示在 240x320 的小屏幕上。"
    "用简体中文口语回答,先给结论,通常一到三句话、不超过 120 个字;"
    "用户明确要求详细时可以多说,但也控制在 300 字以内。"
    "不要使用 Markdown、列表符号、表格、表情或代码块。"
    "听写结果可能有错别字或同音字,按最合理的意思理解。"
    "需要实时信息(新闻、股价等)而你不知道时,直接说明你无法获取最新信息。"
)

CONVERSATION_IDLE_SECONDS = 600   # 超过 10 分钟没说话就开始新对话
CONVERSATION_MAX_TURNS = 8        # 达到后开始新对话(只追加、不截断历史)


# ---------------------------------------------------------------------------
# Speech recognition
# ---------------------------------------------------------------------------
def write_wav(path: Path, pcm: list[int]) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(adpcm.pcm_to_bytes(pcm))


def to_simplified_hint() -> str:
    return "以下是普通话的句子,使用简体中文。"


class ASR:
    def __init__(self, cfg: dict[str, Any]) -> None:
        self.provider = cfg.get("provider", "mlx_whisper")
        self.model = cfg.get("model", "")
        self.base_url = cfg.get("base_url", "https://api.openai.com/v1").rstrip("/")
        self.api_key = cfg.get("api_key") or os.environ.get(cfg.get("api_key_env", "OPENAI_API_KEY"), "")
        self._local: Any = None

    def _transcribe_local(self, path: Path) -> str:
        if self.provider == "mlx_whisper":
            import mlx_whisper  # type: ignore[import-not-found]

            result = mlx_whisper.transcribe(
                str(path), path_or_hf_repo=self.model or "mlx-community/whisper-large-v3-turbo",
                language="zh", initial_prompt=to_simplified_hint())
            return str(result.get("text", "")).strip()
        from faster_whisper import WhisperModel  # type: ignore[import-not-found]

        if self._local is None:
            self._local = WhisperModel(self.model or "small", compute_type="int8")
        segments, _ = self._local.transcribe(str(path), language="zh",
                                              initial_prompt=to_simplified_hint())
        return "".join(s.text for s in segments).strip()

    async def transcribe(self, pcm: list[int], session: aiohttp.ClientSession) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "in.wav"
            write_wav(path, pcm)
            if self.provider == "openai":
                form = aiohttp.FormData()
                form.add_field("model", self.model or "whisper-1")
                form.add_field("language", "zh")
                form.add_field("file", path.read_bytes(), filename="in.wav", content_type="audio/wav")
                async with session.post(f"{self.base_url}/audio/transcriptions", data=form,
                                        headers={"Authorization": f"Bearer {self.api_key}"},
                                        timeout=aiohttp.ClientTimeout(total=30)) as r:
                    r.raise_for_status()
                    return str((await r.json()).get("text", "")).strip()
            return await asyncio.get_running_loop().run_in_executor(None, self._transcribe_local, path)


# ---------------------------------------------------------------------------
# Language model
# ---------------------------------------------------------------------------
@dataclass
class Conversation:
    messages: list[dict[str, Any]] = field(default_factory=list)
    last: float = 0.0
    turns: int = 0


class LLM:
    def __init__(self, cfg: dict[str, Any]) -> None:
        self.provider = cfg.get("provider", "anthropic")
        self.model = cfg.get("model", "claude-opus-5-5" if self.provider == "anthropic" else "")
        # 语音问答追求快:默认 low;想要更周到的回答可在配置里改成 medium / high。
        self.effort = cfg.get("effort", "low")
        self.base_url = cfg.get("base_url", "").rstrip("/")
        self.api_key = cfg.get("api_key") or os.environ.get(cfg.get("api_key_env", ""), "")
        self._client: Any = None
        self._convs: dict[str, Conversation] = {}

    def _conversation(self, device: str) -> Conversation:
        conv = self._convs.get(device)
        now = time.time()
        if (conv is None or now - conv.last > CONVERSATION_IDLE_SECONDS
                or conv.turns >= CONVERSATION_MAX_TURNS):
            conv = Conversation()
            self._convs[device] = conv
        conv.last = now
        return conv

    @staticmethod
    def _rollback(conv: Conversation) -> None:
        """出错的这一问不留在历史里,避免下一轮出现两条连续的用户消息。"""
        if conv.messages and conv.messages[-1]["role"] == "user":
            conv.messages.pop()
            conv.turns -= 1

    async def _anthropic(self, conv: Conversation) -> str:
        import anthropic

        if self._client is None:
            # 不写死密钥:读取 ANTHROPIC_API_KEY 或 `ant auth login` 的登录信息。
            self._client = anthropic.AsyncAnthropic(api_key=self.api_key or None)
        try:
            resp = await self._client.beta.messages.create(
                model=self.model,
                max_tokens=4000,
                system=SYSTEM_PROMPT,
                output_config={"effort": self.effort},
                # 被安全策略拒绝时由服务端自动换模型重试
                betas=["server-side-fallback-2026-07-01"],
                fallbacks="default",
                messages=conv.messages,
            )
        except anthropic.AuthenticationError:
            self._rollback(conv)
            return "服务器上的 Claude 密钥无效,请检查配置。"
        except anthropic.RateLimitError:
            self._rollback(conv)
            return "请求太频繁了,稍后再问我吧。"
        except anthropic.APIConnectionError:
            self._rollback(conv)
            return "服务器连不上 Claude,检查一下网络。"
        except anthropic.APIStatusError as exc:
            log.warning("claude error %s: %s", exc.status_code, exc.message)
            self._rollback(conv)
            return "大模型出错了,稍后再试。"
        if resp.stop_reason == "refusal":
            self._rollback(conv)   # 这一问不进入历史
            return "这个问题我没法回答。"
        # 原样追加完整内容(含思考块),保持历史只追加不改动。
        conv.messages.append({"role": "assistant",
                              "content": [b.to_dict() for b in resp.content]})
        return "".join(b.text for b in resp.content if b.type == "text").strip()

    async def _openai_compatible(self, conv: Conversation, session: aiohttp.ClientSession) -> str:
        msgs = [{"role": "system", "content": SYSTEM_PROMPT}]
        for m in conv.messages:
            content = m["content"]
            if isinstance(content, list):
                content = "".join(b.get("text", "") for b in content if b.get("type") == "text")
            msgs.append({"role": m["role"], "content": content})
        async with session.post(f"{self.base_url}/chat/completions",
                                headers={"Authorization": f"Bearer {self.api_key}"},
                                json={"model": self.model, "messages": msgs, "max_tokens": 800},
                                timeout=aiohttp.ClientTimeout(total=60)) as r:
            if r.status != 200:
                log.warning("llm error %s: %s", r.status, await r.text())
                self._rollback(conv)
                return "大模型出错了,稍后再试。"
            data = await r.json()
        text = data["choices"][0]["message"]["content"].strip()
        conv.messages.append({"role": "assistant", "content": text})
        return text

    async def answer(self, device: str, question: str, session: aiohttp.ClientSession) -> str:
        conv = self._conversation(device)
        conv.messages.append({"role": "user", "content": question})
        conv.turns += 1
        if self.provider == "openai_compatible":
            return await self._openai_compatible(conv, session)
        return await self._anthropic(conv)


# ---------------------------------------------------------------------------
# Speech synthesis
# ---------------------------------------------------------------------------
def speakable(text: str) -> str:
    text = re.sub(r"[*#`>_~\[\]]", "", text)
    return re.sub(r"\s+", " ", text).strip()


def resample(pcm: list[int], src: int, dst: int = RATE) -> list[int]:
    if src == dst or not pcm:
        return pcm
    n = int(len(pcm) * dst / src)
    out = []
    for i in range(n):
        pos = i * src / dst
        j = int(pos)
        frac = pos - j
        a = pcm[j]
        b = pcm[j + 1] if j + 1 < len(pcm) else a
        out.append(int(a + (b - a) * frac))
    return out


class TTS:
    def __init__(self, cfg: dict[str, Any]) -> None:
        self.provider = cfg.get("provider", "say" if shutil.which("say") else "none")
        self.voice = cfg.get("voice", "Tingting" if self.provider == "say" else "alloy")
        self.model = cfg.get("model", "gpt-4o-mini-tts")
        self.base_url = cfg.get("base_url", "https://api.openai.com/v1").rstrip("/")
        self.api_key = cfg.get("api_key") or os.environ.get(cfg.get("api_key_env", "OPENAI_API_KEY"), "")

    def _say(self, text: str) -> list[int]:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out.wav"
            subprocess.run(["say", "-v", self.voice, "-o", str(out), "--file-format=WAVE",
                            "--data-format=LEI16@16000", text], check=True, timeout=60)
            with wave.open(str(out), "rb") as w:
                frames = w.readframes(w.getnframes())
                pcm = adpcm.bytes_to_pcm(frames)
                return resample(pcm, w.getframerate())

    async def synthesize(self, text: str, session: aiohttp.ClientSession) -> list[int]:
        text = speakable(text)
        if not text or self.provider == "none":
            return []
        if self.provider == "openai":
            async with session.post(f"{self.base_url}/audio/speech",
                                    headers={"Authorization": f"Bearer {self.api_key}"},
                                    json={"model": self.model, "voice": self.voice, "input": text,
                                          "response_format": "pcm"},
                                    timeout=aiohttp.ClientTimeout(total=60)) as r:
                r.raise_for_status()
                return resample(adpcm.bytes_to_pcm(await r.read()), 24000)
        return await asyncio.get_running_loop().run_in_executor(None, self._say, text)


# ---------------------------------------------------------------------------
class Assistant:
    MIN_SAMPLES = RATE // 4   # 少于 0.25 秒的录音当作误触

    def __init__(self, asr: ASR, llm: LLM, tts: TTS) -> None:
        self.asr, self.llm, self.tts = asr, llm, tts
        self._audio: dict[str, bytes] = {}

    def audio(self, audio_id: str) -> bytes | None:
        return self._audio.get(audio_id)

    def _store(self, audio_id: str, data: bytes) -> None:
        self._audio[audio_id] = data
        while len(self._audio) > 20:
            self._audio.pop(next(iter(self._audio)))

    async def talk(self, device: str, adpcm_bytes: bytes,
                   session: aiohttp.ClientSession) -> dict[str, Any] | None:
        pcm = adpcm.decode_stream(adpcm_bytes)
        if len(pcm) < self.MIN_SAMPLES:
            return None
        t0 = time.time()
        question = await self.asr.transcribe(pcm, session)
        if not question:
            return None
        t1 = time.time()
        answer = await self.llm.answer(device, question, session)
        t2 = time.time()
        audio_id = ""
        try:
            speech = await self.tts.synthesize(answer, session)
            if speech:
                audio_id = f"{device}-{int(time.time() * 1000)}"
                self._store(audio_id, b"".join(adpcm.encode_stream(speech)))
        except (OSError, subprocess.SubprocessError, aiohttp.ClientError) as exc:
            log.warning("tts failed: %s", exc)
        log.info("asr %.1fs llm %.1fs tts %.1fs | %s -> %s", t1 - t0, t2 - t1, time.time() - t2,
                 question, answer[:40])
        return {"question": question, "answer": answer, "audio": audio_id}
