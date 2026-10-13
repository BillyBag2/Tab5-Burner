"""Run the production firmware model with a native C compiler or Emscripten/Node.

Dependencies are fetched by `idf.py reconfigure`. Examples:
  python tests/run_host_tests.py --cc gcc
  python tests/run_host_tests.py --cc C:/tools/emsdk/upstream/emscripten/emcc.exe --node C:/tools/emsdk/node/24.19.0_64bit/node.exe
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("cc") or shutil.which("gcc") or shutil.which("emcc"))
    parser.add_argument("--node", default=shutil.which("node"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("provide --cc with a native C compiler or emcc")
    cjson = ROOT / "managed_components/espressif__cjson/cJSON"
    if not (cjson / "cJSON.c").exists():
        parser.error("run idf.py reconfigure to fetch cJSON first")
    wasm = "emcc" in Path(args.cc).name
    if wasm and not args.node:
        parser.error("Emscripten tests require --node")
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="model-tests-", dir=build) as temp:
        output = Path(temp) / ("test.cjs" if wasm else "test.exe")
        command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                   "-I" + str(ROOT / "main"), "-I" + str(cjson),
                   str(ROOT / "main/firmware.c"), str(cjson / "cJSON.c"),
                   str(ROOT / "tests/test_firmware.c"), "-o", str(output)]
        if wasm:
            command += ["-sENVIRONMENT=node", "-sASSERTIONS=2"]
        subprocess.run(command, cwd=temp, check=True)
        subprocess.run([args.node, str(output)] if wasm else [str(output)], cwd=temp, check=True)
        services = Path(temp) / ("services.cjs" if wasm else "services.exe")
        command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                   "-I" + str(ROOT / "main"), str(ROOT / "main/battery_math.c"),
                   str(ROOT / "main/storage_gate.c"), str(ROOT / "tests/test_host_services.c"),
                   "-o", str(services)]
        if wasm:
            command += ["-sENVIRONMENT=node", "-sASSERTIONS=2"]
        subprocess.run(command, cwd=temp, check=True)
        subprocess.run([args.node, str(services)] if wasm else [str(services)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
