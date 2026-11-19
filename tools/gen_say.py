#!/usr/bin/env python3
"""Make the spoken clips the duck reads its IP address with, sounds/say/*.wav (16 kHz, mono, 16-bit).

The voice is Microsoft's zh-CN-XiaoxiaoNeural through edge-tts (it needs the network); the
silence around each clip is cut so the digits follow each other closely. Needs:

    pip install edge-tts miniaudio
    python3 tools/gen_say.py [out_dir]     # default: sounds/say/
"""
import array
import asyncio
import os
import sys
import wave

import edge_tts
import miniaudio

VOICE = "zh-CN-XiaoxiaoNeural"
RATE = 16000
CLIPS = {"ip": "配网成功，请使用浏览器访问", "dot": "点", "ap": "开始配网",
         **{str(i): c for i, c in enumerate("零一二三四五六七八九")}}


async def main(out):
    os.makedirs(out, exist_ok=True)
    for name, text in CLIPS.items():
        mp3 = b"".join([c["data"] async for c in edge_tts.Communicate(text, VOICE).stream() if c["type"] == "audio"])
        pcm = miniaudio.decode(mp3, miniaudio.SampleFormat.SIGNED16, 1, RATE).samples
        loud = [i for i, v in enumerate(pcm) if abs(v) > 500]
        pad = RATE // 50  # 20 ms
        pcm = pcm[max(loud[0] - pad, 0):loud[-1] + pad]
        with wave.open(os.path.join(out, name + ".wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(array.array("h", pcm).tobytes())
        print(f"{name}: {text} {len(pcm) / RATE:.2f} s")


asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "sounds", "say")))
