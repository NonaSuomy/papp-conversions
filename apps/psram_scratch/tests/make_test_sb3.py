#!/usr/bin/env python3
"""Build papp_test.sb3: a tiny Scratch 3 project for testing the PAPP port.

Everything in it is generated here (no third-party assets):

  Stage   an SVG backdrop (sky, ground, a sun) and a "moves" variable monitor
  Cat     a round SVG face (costume 1) and a PNG star (costume 2, bitmap
          at resolution 2), a short WAV beep

  when green flag clicked:  go to 0,0, say "Hello from the P4!" for 2 s,
                            then forever: arrow keys move 4 steps (and count
                            "moves"), a finger/mouse down makes it go to the
                            mouse pointer
  when space key pressed:   start the beep, change the colour effect by 25,
                            next costume
  when this sprite clicked: turn 15 degrees
  when green flag clicked (stage): set moves to 0

The zip is written with fixed timestamps, so the same script always gives the
same bytes.

    python3 apps/psram_scratch/tests/make_test_sb3.py [out.sb3]
"""

from __future__ import annotations

import hashlib
import io
import json
import math
import struct
import sys
import wave
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent


def md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


# ── Assets ──────────────────────────────────────────────────────────────────

BACKDROP_SVG = b"""<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360" viewBox="0 0 480 360">
<rect x="0" y="0" width="480" height="360" fill="#9fd8ff"/>
<rect x="0" y="270" width="480" height="90" fill="#5cb85c"/>
<circle cx="400" cy="70" r="36" fill="#ffd23f"/>
<rect x="40" y="220" width="70" height="50" fill="#b5651d"/>
<polygon points="30,220 75,180 120,220" fill="#d9534f"/>
</svg>
"""

FACE_SVG = b"""<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80" viewBox="0 0 80 80">
<circle cx="40" cy="40" r="36" fill="#ff9f1c" stroke="#7a4a00" stroke-width="4"/>
<circle cx="28" cy="32" r="6" fill="#ffffff"/>
<circle cx="52" cy="32" r="6" fill="#ffffff"/>
<circle cx="30" cy="33" r="3" fill="#000000"/>
<circle cx="54" cy="33" r="3" fill="#000000"/>
<path d="M24 50 Q40 64 56 50" fill="none" stroke="#7a4a00" stroke-width="4" stroke-linecap="round"/>
<polygon points="64,40 78,34 78,46" fill="#7a4a00"/>
</svg>
"""


def png_rgba(width: int, height: int, pixels: bytes) -> bytes:
    """A minimal RGBA PNG (8 bits per channel, no filtering)."""
    raw = b"".join(b"\x00" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def star_png(size: int = 160) -> bytes:
    """A five-pointed star (Scratch bitmaps are drawn at resolution 2, so this shows as 80x80)."""
    cx = cy = size / 2
    outer, inner = size * 0.47, size * 0.2
    points = []
    for i in range(10):
        r = outer if i % 2 == 0 else inner
        a = -math.pi / 2 + i * math.pi / 5
        points.append((cx + r * math.cos(a), cy + r * math.sin(a)))

    def inside(x: float, y: float) -> bool:
        hit = False
        for i in range(len(points)):
            (x1, y1), (x2, y2) = points[i], points[(i + 1) % len(points)]
            if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
                hit = not hit
        return hit

    px = bytearray()
    for y in range(size):
        for x in range(size):
            if inside(x + 0.5, y + 0.5):
                px += bytes((0x4C, 0x97, 0xFF, 0xFF))  # Scratch blue
            else:
                px += bytes((0, 0, 0, 0))
    return png_rgba(size, size, bytes(px))


def beep_wav(rate: int = 22050, seconds: float = 0.25, freq: float = 880.0) -> bytes:
    """A short decaying sine beep, 16-bit mono PCM."""
    frames = int(rate * seconds)
    samples = bytearray()
    for i in range(frames):
        env = 1.0 - i / frames
        samples += struct.pack("<h", int(12000 * env * math.sin(2 * math.pi * freq * i / rate)))
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(samples))
    return out.getvalue()


# ── Blocks ──────────────────────────────────────────────────────────────────

class Script:
    """Builds a target's "blocks" dictionary with sequential ids."""

    def __init__(self, prefix: str):
        self.prefix = prefix
        self.blocks: dict[str, dict] = {}
        self.count = 0

    def _id(self) -> str:
        self.count += 1
        return f"{self.prefix}{self.count}"

    def block(self, opcode: str, inputs=None, fields=None, parent=None, shadow=False, top=None) -> str:
        bid = self._id()
        self.blocks[bid] = {
            "opcode": opcode, "next": None, "parent": parent, "inputs": inputs or {}, "fields": fields or {},
            "shadow": shadow, "topLevel": top is not None,
        }
        if top is not None:
            self.blocks[bid]["x"], self.blocks[bid]["y"] = top
        return bid

    def chain(self, ids: list[str]) -> None:
        for a, b in zip(ids, ids[1:]):
            self.blocks[a]["next"] = b
            self.blocks[b]["parent"] = a

    def menu(self, opcode: str, field: str, value: str, parent: str) -> str:
        return self.block(opcode, fields={field: [value, None]}, parent=parent, shadow=True)

    @staticmethod
    def num(value) -> list:
        return [1, [4, str(value)]]

    @staticmethod
    def text(value) -> list:
        return [1, [10, str(value)]]

    def set_parent(self, child: str, parent: str) -> None:
        self.blocks[child]["parent"] = parent


def sprite_blocks(var_id: str) -> dict:
    s = Script("cat")
    # when green flag clicked
    hat = s.block("event_whenflagclicked", top=(20, 20))
    goto = s.block("motion_gotoxy", {"X": s.num(0), "Y": s.num(0)})
    say = s.block("looks_sayforsecs", {"MESSAGE": s.text("Hello from the P4!"), "SECS": s.num(2)})
    forever = s.block("control_forever")
    s.chain([hat, goto, say, forever])

    body = []
    for key, opcode, field, amount in (("right arrow", "motion_changexby", "DX", 4),
                                       ("left arrow", "motion_changexby", "DX", -4),
                                       ("up arrow", "motion_changeyby", "DY", 4),
                                       ("down arrow", "motion_changeyby", "DY", -4)):
        cond = s.block("sensing_keypressed")
        keymenu = s.menu("sensing_keyoptions", "KEY_OPTION", key, cond)
        s.blocks[cond]["inputs"]["KEY_OPTION"] = [1, keymenu]
        move = s.block(opcode, {field: s.num(amount)})
        count = s.block("data_changevariableby", {"VALUE": s.num(1)}, {"VARIABLE": ["moves", var_id]})
        s.chain([move, count])
        iff = s.block("control_if", {"CONDITION": [2, cond], "SUBSTACK": [2, move]})
        s.set_parent(cond, iff)
        s.set_parent(move, iff)
        body.append(iff)
    # if mouse down then go to mouse-pointer
    down = s.block("sensing_mousedown")
    goto_mouse = s.block("motion_goto")
    target = s.menu("motion_goto_menu", "TO", "_mouse_", goto_mouse)
    s.blocks[goto_mouse]["inputs"]["TO"] = [1, target]
    iff = s.block("control_if", {"CONDITION": [2, down], "SUBSTACK": [2, goto_mouse]})
    s.set_parent(down, iff)
    s.set_parent(goto_mouse, iff)
    body.append(iff)
    s.chain(body)
    s.blocks[forever]["inputs"]["SUBSTACK"] = [2, body[0]]
    s.set_parent(body[0], forever)

    # when space key pressed
    hat2 = s.block("event_whenkeypressed", fields={"KEY_OPTION": ["space", None]}, top=(20, 400))
    sound = s.block("sound_play")
    sound_menu = s.menu("sound_sounds_menu", "SOUND_MENU", "beep", sound)
    s.blocks[sound]["inputs"]["SOUND_MENU"] = [1, sound_menu]
    effect = s.block("looks_changeeffectby", {"CHANGE": s.num(25)}, {"EFFECT": ["COLOR", None]})
    nextc = s.block("looks_nextcostume")
    s.chain([hat2, sound, effect, nextc])

    # when this sprite clicked
    hat3 = s.block("event_whenthisspriteclicked", top=(20, 560))
    turn = s.block("motion_turnright", {"DEGREES": s.num(15)})
    s.chain([hat3, turn])
    return s.blocks


def stage_blocks(var_id: str) -> dict:
    s = Script("stage")
    hat = s.block("event_whenflagclicked", top=(20, 20))
    reset = s.block("data_setvariableto", {"VALUE": s.text(0)}, {"VARIABLE": ["moves", var_id]})
    s.chain([hat, reset])
    return s.blocks


def build() -> bytes:
    backdrop = BACKDROP_SVG
    face = FACE_SVG
    star = star_png()
    beep = beep_wav()
    var_id = "papp-moves-var"
    project = {
        "targets": [
            {
                "isStage": True, "name": "Stage",
                "variables": {var_id: ["moves", 0]}, "lists": {}, "broadcasts": {},
                "blocks": stage_blocks(var_id), "comments": {}, "currentCostume": 0,
                "costumes": [{"name": "sky", "dataFormat": "svg", "assetId": md5(backdrop),
                              "md5ext": md5(backdrop) + ".svg", "rotationCenterX": 240, "rotationCenterY": 180}],
                "sounds": [], "volume": 100, "layerOrder": 0, "tempo": 60,
                "videoTransparency": 50, "videoState": "on", "textToSpeechLanguage": None,
            },
            {
                "isStage": False, "name": "Cat",
                "variables": {}, "lists": {}, "broadcasts": {},
                "blocks": sprite_blocks(var_id), "comments": {}, "currentCostume": 0,
                "costumes": [
                    {"name": "face", "dataFormat": "svg", "assetId": md5(face), "md5ext": md5(face) + ".svg",
                     "rotationCenterX": 40, "rotationCenterY": 40},
                    {"name": "star", "bitmapResolution": 2, "dataFormat": "png", "assetId": md5(star),
                     "md5ext": md5(star) + ".png", "rotationCenterX": 80, "rotationCenterY": 80},
                ],
                "sounds": [{"name": "beep", "assetId": md5(beep), "dataFormat": "wav", "format": "", "rate": 22050,
                            "sampleCount": (len(beep) - 44) // 2, "md5ext": md5(beep) + ".wav"}],
                "volume": 100, "layerOrder": 1, "visible": True, "x": 0, "y": 0, "size": 100, "direction": 90,
                "draggable": False, "rotationStyle": "all around",
            },
        ],
        "monitors": [
            {"id": var_id, "mode": "default", "opcode": "data_variable", "params": {"VARIABLE": "moves"},
             "spriteName": None, "value": 0, "width": 0, "height": 0, "x": 5, "y": 5, "visible": True,
             "sliderMin": 0, "sliderMax": 100, "isDiscrete": True},
        ],
        "extensions": [],
        "meta": {"semver": "3.0.0", "vm": "0.2.0", "agent": "papp-conversions make_test_sb3.py"},
    }
    files = {
        "project.json": json.dumps(project, indent=None, separators=(",", ":")).encode(),
        md5(backdrop) + ".svg": backdrop,
        md5(face) + ".svg": face,
        md5(star) + ".png": star,
        md5(beep) + ".wav": beep,
    }
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    return out.getvalue()


def main() -> int:
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "papp_test.sb3"
    data = build()
    target.write_bytes(data)
    print(f"{target}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
