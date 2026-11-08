"""Configure / build Sky Save System Ultra (3SU) with the VS bundled CMake.

No vcvars64.bat: on this machine reg.exe is blocked by the security policy, so
the vcvars capture trick used by the older projects fails.  The Visual Studio
generator finds the toolchain on its own, and an already-configured build
directory can simply be incremented with --build.

Both HTTP_PROXY and http_proxy must never be present at the same time:
MSBuild's environment dictionary is case-insensitive and aborts with
"MSB6001 ... 已添加项".
"""
import os
import subprocess
import sys

CMAKE = (r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
         r"\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
SOURCE = r"D:\dev\3SU"
BUILD = os.path.join(SOURCE, "build")
# The CMake target is spelled out; only the artefact is called 3SU.dll.
TARGET = "SkySaveSystemUltra"


def clean_env() -> dict:
    env = {}
    for key, value in os.environ.items():
        lower = key.lower()
        if lower in ("http_proxy", "https_proxy", "all_proxy"):
            continue
        env[key] = value
    return env


def run(args) -> int:
    print(">", " ".join(args))
    return subprocess.call(args, env=clean_env())


def main() -> int:
    reconfigure = "--reconfigure" in sys.argv or not os.path.isfile(
        os.path.join(BUILD, "CMakeCache.txt"))

    if reconfigure:
        rc = run([CMAKE, "-S", SOURCE, "-B", BUILD,
                  "-G", "Visual Studio 17 2022", "-A", "x64"])
        if rc != 0:
            return rc

    return run([CMAKE, "--build", BUILD, "--config", "Release", "--target", TARGET])


if __name__ == "__main__":
    raise SystemExit(main())
