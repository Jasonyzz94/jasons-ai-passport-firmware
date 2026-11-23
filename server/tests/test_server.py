"""Server tests: ADPCM parity with the firmware, API routes, data shaping, LLM call shape."""

from __future__ import annotations

import asyncio
import math
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import date, datetime
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from aiohttp.test_utils import TestClient, TestServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from passport_server import adpcm, board, discovery, weather  # noqa: E402
from passport_server.app import create_app  # noqa: E402
from passport_server.assistant import (LLM, TTS, Conversation, StageError, http_reason,  # noqa: E402
                                       resample, speakable, trim_answer)
from passport_server.config import Config  # noqa: E402
from passport_server.home import item_from_state  # noqa: E402

REPO = Path(__file__).resolve().parents[2]


def tone(n: int = 2048) -> list[int]:
    return [int(9000 * math.sin(2 * math.pi * 330 * i / 16000) + 3000 * math.sin(2 * math.pi * 1700 * i / 16000))
            for i in range(n)]


class AdpcmTest(unittest.TestCase):
    def test_roundtrip_quality(self) -> None:
        pcm = tone()
        data = b"".join(adpcm.encode_stream(pcm))
        self.assertEqual(len(data), adpcm.BLOCK_BYTES * (len(pcm) // adpcm.BLOCK_SAMPLES))
        out = adpcm.decode_stream(data)
        self.assertEqual(len(out), len(pcm))
        sig = sum(x * x for x in pcm[256:])
        err = sum((a - b) ** 2 for a, b in zip(pcm[256:], out[256:]))
        self.assertGreater(10 * math.log10(sig / (err + 1)), 20)

    @unittest.skipUnless(shutil.which("cc"), "needs a C compiler")
    def test_bit_exact_with_firmware_codec(self) -> None:
        prog = r'''
#include "audio_dsp.h"
#include <math.h>
#include <stdio.h>
int main(void) {
    int16_t pcm[2048]; uint8_t blk[ADPCM_BLOCK_BYTES]; adpcm_state_t st = {0, 0};
    for (int i = 0; i < 2048; i++)
        pcm[i] = (int16_t)(int)(9000 * sin(2 * 3.141592653589793 * 330 * i / 16000)
                                + 3000 * sin(2 * 3.141592653589793 * 1700 * i / 16000));
    for (int b = 0; b < 8; b++) {
        size_t n = adpcm_encode_block(&st, pcm + b * 256, 256, blk);
        for (size_t k = 0; k < n; k++) printf("%02x", blk[k]);
    }
    return 0;
}'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "t.c"
            src.write_text(prog)
            exe = Path(tmp) / "t"
            subprocess.run(["cc", "-std=c11", f"-I{REPO / 'main'}", str(src), str(REPO / "main/audio_dsp.c"),
                            "-lm", "-o", str(exe)], check=True)
            c_hex = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        py_hex = b"".join(adpcm.encode_stream(tone())).hex()
        self.assertEqual(c_hex, py_hex)


class DataShapeTest(unittest.TestCase):
    def test_discovery_reply(self) -> None:
        self.assertIsNone(discovery.reply_for(b"hello", "http://x:1", "M"))
        reply = discovery.reply_for(b"PASSPORT_DISCOVER 1 A3F2", "http://1.2.3.4:8787", "Mac mini")
        self.assertIn(b'"url": "http://1.2.3.4:8787"', reply)

    def test_beacon(self) -> None:
        self.assertIn(b'"beacon": 1', discovery.beacon_payload("http://x:1", "M"))
        self.assertEqual(discovery.broadcast_targets("192.168.31.20"),
                         ["255.255.255.255", "192.168.31.255"])
        self.assertEqual(discovery.broadcast_targets("127.0.0.1"), ["255.255.255.255"])

    def test_wmo_and_aqi(self) -> None:
        self.assertEqual(weather.wmo(0, True), ("sun", "晴"))
        self.assertEqual(weather.wmo(0, False)[0], "moon")
        self.assertEqual(weather.wmo(63)[0], "rain")
        self.assertEqual(weather.aqi_text(42), "优")
        self.assertEqual(weather.aqi_text(None), "--")

    def test_weather_shape(self) -> None:
        fc = {
            "current": {"time": "2026-11-19T09:45", "temperature_2m": 18.4, "apparent_temperature": 17.2,
                        "relative_humidity_2m": 62, "weather_code": 2, "is_day": 1},
            "hourly": {"time": [f"2026-11-19T{h:02d}:00" for h in range(24)],
                       "temperature_2m": [15 + h * 0.2 for h in range(24)], "weather_code": [1] * 24},
            "daily": {"time": ["2026-11-19", "2026-11-20", "2026-11-21", "2026-11-22"],
                      "weather_code": [2, 61, 3, 0], "temperature_2m_max": [21, 17, 16, 19],
                      "temperature_2m_min": [14, 12, 11, 10]},
        }
        out = weather.shape(Config(), fc, {"current": {"us_aqi": 42}}, datetime(2026, 11, 19, 9, 45))
        self.assertEqual(out["now"]["temp"], 18)
        self.assertEqual(out["now"]["aqi_text"], "优")
        self.assertEqual(out["hourly"][0]["h"], "10时")
        self.assertEqual([d["day"] for d in out["daily"]], ["今天", "周五", "周六", "周日"])
        self.assertEqual(out["daily"][1]["code"], "rain")

    def test_calendar_card(self) -> None:
        ics = ("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nDTSTART:20261120T013000Z\r\nSUMMARY:周会\r\nEND:VEVENT\r\n"
               "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261121\r\nSUMMARY:生日\r\nEND:VEVENT\r\n"
               "BEGIN:VEVENT\r\nDTSTART:20261101T090000\r\nSUMMARY:过去\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n")
        now = datetime(2026, 11, 19, 8, 0)
        events = board.next_events(ics, now)
        self.assertEqual([e[2] for e in events], ["周会", "生日"])
        card = board.calendar_card({"title": "日程"}, ics, now)
        self.assertIn("之后还有 1 项", card["sub"])
        empty = board.calendar_card({"title": "日程"}, "BEGIN:VCALENDAR\r\nEND:VCALENDAR", now)
        self.assertEqual(empty["value"], "0")

    def test_countdown(self) -> None:
        card = board.countdown({"title": "元旦", "date": "2027-01-01"}, today=date(2026, 12, 25))
        self.assertEqual((card["value"], card["unit"]), ("7", "天"))

    def test_home_item(self) -> None:
        item = item_from_state({"entity_id": "light.living", "state": "on",
                                "attributes": {"friendly_name": "客厅灯"}})
        self.assertEqual((item["type"], item["on"], item["state"]), ("light", True, "开"))
        sensor = item_from_state({"entity_id": "sensor.t", "state": "23.5",
                                  "attributes": {"unit_of_measurement": "℃"}})
        self.assertEqual(sensor["state"], "23.5℃")

    def test_text_helpers(self) -> None:
        self.assertEqual(speakable("**你好** `x`"), "你好 x")
        self.assertEqual(len(resample([0, 100] * 120, 24000)), 160)


class LLMTest(unittest.TestCase):
    def _resp(self, stop: str, text: str = "需要带伞。"):
        block = SimpleNamespace(type="text", text=text, to_dict=lambda: {"type": "text", "text": text})
        return SimpleNamespace(stop_reason=stop, content=[block])

    def test_anthropic_request_shape_and_history(self) -> None:
        llm = LLM({})
        llm._client = mock.MagicMock()
        llm._client.beta.messages.create = mock.AsyncMock(return_value=self._resp("end_turn"))
        answer = asyncio.run(llm.answer("A3F2", "明天要带伞吗", None))
        self.assertEqual(answer, "需要带伞。")
        kwargs = llm._client.beta.messages.create.call_args.kwargs
        self.assertEqual(kwargs["model"], "claude-opus-5-5")
        self.assertEqual(kwargs["output_config"], {"effort": "low"})
        self.assertEqual(kwargs["fallbacks"], "default")
        self.assertIn("server-side-fallback-2026-07-01", kwargs["betas"])
        self.assertNotIn("thinking", kwargs)
        conv = llm._convs["A3F2"]
        self.assertEqual([m["role"] for m in conv.messages], ["user", "assistant"])

    def test_refusal_is_not_kept_in_history(self) -> None:
        llm = LLM({})
        llm._client = mock.MagicMock()
        llm._client.beta.messages.create = mock.AsyncMock(return_value=self._resp("refusal", ""))
        answer = asyncio.run(llm.answer("A3F2", "x", None))
        self.assertEqual(answer, "这个问题我没法回答。")
        self.assertEqual(llm._convs["A3F2"].messages, [])

    def test_new_conversation_after_max_turns(self) -> None:
        llm = LLM({})
        conv = llm._conversation("d")
        conv.turns = 8
        self.assertIsNot(llm._conversation("d"), conv)
        self.assertIsInstance(llm._conversation("d"), Conversation)


class FakeResponse:
    def __init__(self, status: int, payload: object = None, body: bytes = b"") -> None:
        self.status = status
        self._payload = payload
        self._body = body

    async def __aenter__(self) -> "FakeResponse":
        return self

    async def __aexit__(self, *exc: object) -> None:
        return None

    async def json(self) -> object:
        return self._payload

    async def text(self) -> str:
        return str(self._payload)

    async def read(self) -> bytes:
        return self._body


class FakeSession:
    """记录请求、返回预设响应的 aiohttp.ClientSession 替身。"""

    def __init__(self, resp: FakeResponse) -> None:
        self.resp = resp
        self.calls: list[tuple[str, dict]] = []

    def post(self, url: str, **kwargs: object) -> FakeResponse:
        self.calls.append((url, kwargs))
        return self.resp


class OpenAICompatibleTest(unittest.TestCase):
    def _llm(self, **cfg: str) -> LLM:
        base = {"provider": "openai_compatible", "base_url": "https://api.deepseek.com/v1",
                "model": "deepseek-chat", "api_key": "sk-test"}
        base.update(cfg)
        return LLM(base)

    def test_deepseek_request_and_history(self) -> None:
        llm = self._llm()
        session = FakeSession(FakeResponse(200, {"choices": [{"message": {"content": " 晴。 "}}]}))
        self.assertEqual(asyncio.run(llm.answer("d", "天气", session)), "晴。")
        url, kw = session.calls[0]
        self.assertEqual(url, "https://api.deepseek.com/v1/chat/completions")
        self.assertEqual(kw["headers"], {"Authorization": "Bearer sk-test"})
        self.assertEqual(kw["json"]["model"], "deepseek-chat")
        self.assertEqual([m["role"] for m in llm._convs["d"].messages], ["user", "assistant"])

    def test_error_names_stage_and_reason_and_rolls_back(self) -> None:
        llm = self._llm()
        for status, reason in ((401, "密钥无效"), (402, "余额不足"), (404, "接口或模型不存在"), (503, "服务商出错")):
            session = FakeSession(FakeResponse(status, {"error": "x"}))
            with self.assertRaises(StageError) as ctx:
                asyncio.run(llm.answer("d", "q", session))
            self.assertEqual(str(ctx.exception), f"对话:{reason}")
            self.assertEqual(llm._convs["d"].messages, [])

    def test_local_ollama_sends_no_key(self) -> None:
        llm = self._llm(base_url="http://localhost:11434/v1", model="qwen2.5:7b", api_key="")
        session = FakeSession(FakeResponse(200, {"choices": [{"message": {"content": "好"}}]}))
        asyncio.run(llm.answer("d", "q", session))
        self.assertEqual(session.calls[0][1]["headers"], {})

    def test_missing_config(self) -> None:
        with self.assertRaises(StageError) as ctx:
            asyncio.run(self._llm(base_url="").answer("d", "q", FakeSession(FakeResponse(200))))
        self.assertEqual(str(ctx.exception), "对话:没配置接口或模型")

    def test_http_reason_matches_firmware_table(self) -> None:
        self.assertEqual(http_reason(403), "密钥无效")
        self.assertEqual(http_reason(429), "太频繁或额度用完")
        self.assertEqual(http_reason(400), "请求被拒")


class CloudTTSTest(unittest.TestCase):
    def test_openai_pcm_24k_is_resampled(self) -> None:
        tts = TTS({"provider": "openai", "api_key": "k"})
        pcm = adpcm.pcm_to_bytes([1000] * 2400)
        session = FakeSession(FakeResponse(200, body=pcm))
        out = asyncio.run(tts.synthesize("你好", session))
        self.assertEqual(len(out), 1600)
        self.assertNotIn("sample_rate", session.calls[0][1]["json"])

    def test_siliconflow_asks_for_16k(self) -> None:
        tts = TTS({"provider": "openai", "api_key": "k", "base_url": "https://api.siliconflow.cn/v1",
                   "model": "FunAudioLLM/CosyVoice2-0.5B", "voice": "FunAudioLLM/CosyVoice2-0.5B:anna",
                   "sample_rate": 16000})
        session = FakeSession(FakeResponse(200, body=adpcm.pcm_to_bytes([5] * 1600) + b"\x01"))
        out = asyncio.run(tts.synthesize("你好", session))
        self.assertEqual(len(out), 1600)   # 不重采样;多出的半个样本被丢掉
        self.assertEqual(session.calls[0][1]["json"]["sample_rate"], 16000)

    def test_error_is_stage_error(self) -> None:
        tts = TTS({"provider": "openai", "api_key": "bad"})
        with self.assertRaises(StageError) as ctx:
            asyncio.run(tts.synthesize("你好", FakeSession(FakeResponse(401, {"error": "x"}))))
        self.assertEqual(str(ctx.exception), "朗读:密钥无效")


class TrimAnswerTest(unittest.TestCase):
    def test_short_answer_unchanged(self) -> None:
        self.assertEqual(trim_answer("晴,二十度。"), "晴,二十度。")

    def test_long_answer_stops_after_a_sentence(self) -> None:
        text = "第一句话说完了。" * 50
        out = trim_answer(text, 30)
        self.assertLessEqual(len(out), 30)
        self.assertTrue(out.endswith("。…"))

    def test_no_sentence_end_cuts_by_character(self) -> None:
        out = trim_answer("一" * 100, 30)
        self.assertEqual(out, "一" * 29 + "…")


class ApiTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        cfg = Config(data_dir=Path(self.tmp.name))
        app = create_app(cfg)
        asst = app["assistant"]
        asst.asr.transcribe = mock.AsyncMock(return_value="今天天气怎么样")
        asst.llm.answer = mock.AsyncMock(return_value="晴,二十度。")
        asst.tts.synthesize = mock.AsyncMock(return_value=tone(1000))
        app["weather"].get = mock.AsyncMock(return_value={"city": "上海"})
        self.app = app
        self.client = TestClient(TestServer(app))
        await self.client.start_server()

    async def asyncTearDown(self) -> None:
        await self.client.close()
        self.tmp.cleanup()

    async def test_flow(self) -> None:
        r = await self.client.get("/api/hello")
        self.assertEqual(r.status, 401)
        r = await self.client.post("/api/pair", json={"device": "A3F2", "name": "Passport-A3F2"})
        token = (await r.json())["token"]
        auth = {"Authorization": f"Bearer {token}"}
        self.assertEqual((await self.client.get("/api/hello", headers=auth)).status, 200)
        self.assertEqual((await (await self.client.get("/api/weather", headers=auth)).json())["city"], "上海")
        home = await (await self.client.get("/api/home", headers=auth)).json()
        self.assertEqual(home["items"], [])

        # 太短的录音 → 422
        r = await self.client.post("/api/assistant/talk", headers=auth, data=b"\x00" * 100)
        self.assertEqual(r.status, 422)
        audio = b"".join(adpcm.encode_stream(tone(16000)))
        r = await self.client.post("/api/assistant/talk", headers=auth, data=audio)
        body = await r.json()
        self.assertEqual(body["question"], "今天天气怎么样")
        self.assertTrue(body["audio"])
        r = await self.client.get(f"/api/assistant/audio/{body['audio']}", headers=auth)
        data = await r.read()
        self.assertGreater(len(data), 0)
        self.assertEqual(len(adpcm.decode_stream(data)), 1000)

    async def test_talk_reply_is_utf8_and_capped(self) -> None:
        r = await self.client.post("/api/pair", json={"device": "A3F2", "name": "Passport-A3F2"})
        auth = {"Authorization": f"Bearer {(await r.json())['token']}"}
        self.app["assistant"].llm.answer = mock.AsyncMock(return_value="很长的一句回答。" * 80)
        audio = b"".join(adpcm.encode_stream(tone(16000)))
        r = await self.client.post("/api/assistant/talk", headers=auth, data=audio)
        raw = await r.read()
        # 汉字直接用 UTF-8 发,不再是 6 字节的 \\uXXXX;回答不超过 300 字,停在句末
        self.assertNotIn(b"\\u", raw)
        self.assertIn("很长的一句回答".encode(), raw)
        answer = (await r.json())["answer"]
        self.assertLessEqual(len(answer), 300)
        self.assertTrue(answer.endswith("。…"))
        self.assertLess(len(raw), 3072)   # 设备的接收缓冲

    async def test_stage_errors_reach_device(self) -> None:
        r = await self.client.post("/api/pair", json={"device": "C1", "name": "x"})
        auth = {"Authorization": f"Bearer {(await r.json())['token']}"}
        audio = b"".join(adpcm.encode_stream(tone(16000)))
        asst = self.app["assistant"]
        # 对话失败:502 + 哪一步、为什么
        asst.llm.answer = mock.AsyncMock(side_effect=StageError("对话", 402))
        r = await self.client.post("/api/assistant/talk", headers=auth, data=audio)
        self.assertEqual(r.status, 502)
        self.assertEqual((await r.json())["error"], "对话:余额不足")
        # 朗读失败:文字照常返回,另带 tts_error
        asst.llm.answer = mock.AsyncMock(return_value="晴。")
        asst.tts.synthesize = mock.AsyncMock(side_effect=StageError("朗读", "网络不通"))
        body = await (await self.client.post("/api/assistant/talk", headers=auth, data=audio)).json()
        self.assertEqual(body["answer"], "晴。")
        self.assertEqual(body["audio"], "")
        self.assertEqual(body["tts_error"], "朗读:网络不通")

    async def test_token_persisted(self) -> None:
        await self.client.post("/api/pair", json={"device": "B1", "name": "x"})
        saved = (Path(self.tmp.name) / "devices.json").read_text()
        self.assertIn('"B1"', saved)


if __name__ == "__main__":
    unittest.main()
