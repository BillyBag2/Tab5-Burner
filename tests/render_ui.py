"""Build and inspect actual LVGL UI without hardware. Requires Emscripten, CMake,
Ninja and Node. Run with an activated Emscripten SDK after idf.py reconfigure.
Screenshots are written to build/ui-render/*.png.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--emcmake", default=shutil.which("emcmake"))
parser.add_argument("--cmake", default=shutil.which("cmake"))
parser.add_argument("--node", default=shutil.which("node"))
args = parser.parse_args()
if not all((args.emcmake, args.cmake, args.node)):
    parser.error("provide --emcmake, --cmake and --node or activate the Emscripten SDK")
output = ROOT / "build/ui-render"
output.mkdir(parents=True, exist_ok=True)
subprocess.run([args.emcmake, args.cmake, "-G", "Ninja", "-S", str(ROOT / "tests/host"), "-B", str(output)], check=True)
subprocess.run([args.cmake, "--build", str(output), "-j", "8"], check=True)
subprocess.run([args.node, str(output / "render_ui.cjs")], cwd=output, check=True)
for source in output.glob("*.bgr"):
    bgr = source.read_bytes()
    rgb = bytearray(len(bgr))
    rgb[0::3], rgb[1::3], rgb[2::3] = bgr[2::3], bgr[1::3], bgr[0::3]
    rows = b"".join(b"\0" + rgb[y * 3840:(y + 1) * 3840] for y in range(720))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    source.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", 1280, 720, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
print(f"UI screenshots: {output}")
