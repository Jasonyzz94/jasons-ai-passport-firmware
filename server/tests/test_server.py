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
from passport_server.assistant import LLM, Conversation, resample, speakable  # noqa: E402
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

    async def test_token_persisted(self) -> None:
        await self.client.post("/api/pair", json={"device": "B1", "name": "x"})
        saved = (Path(self.tmp.name) / "devices.json").read_text()
        self.assertIn('"B1"', saved)


if __name__ == "__main__":
    unittest.main()
