"""Build SkySaveSystemUltra for both Skyrim SE (1.5.97) and AE (1.6.x).

The RE struct layouts (TES, TESObjectCELL, BGSSaveLoadManager ...) are
compile-time per version, so a single DLL cannot be struct-correct for both
runtimes.  We configure two isolated build trees:

    build_se/  ->  SE  layout  (ENABLE_SKYRIM_SE=ON  AE=OFF, no SKYRIM_SUPPORT_AE)
    build_ae/  ->  AE  layout  (ENABLE_SKYRIM_SE=ON  AE=ON,  SKYRIM_SUPPORT_AE=1)

The SSS-Ultra-SaveDoctor.exe is pure C++ file parsing (no engine structs), so
it is version-independent and only needs to be built once (taken from build_ae).

Run:  python build_all.py
"""
import os
import subprocess
import sys

CM = r"C:/Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
SRC = r"D:/dev/3SU"

SE_DIR = os.path.join(SRC, "build_se")
AE_DIR = os.path.join(SRC, "build_ae")


def make_env():
    # MSBuild blows up if both HTTP_PROXY and http_proxy are present
    # (case-insensitive Hashtable key clash).  Strip every *_proxy too.
    env = {}
    for k, v in os.environ.items():
        env[k.lower()] = v
    return {k: v for k, v in env.items() if not k.endswith("_proxy")}


def configure(build_dir: str, se: bool, ae: bool) -> int:
    os.makedirs(build_dir, exist_ok=True)
    cmd = [
        CM, "-S", SRC, "-B", build_dir,
        "-G", "Visual Studio 17 2022", "-A", "x64",
        f"-DENABLE_SKYRIM_SE={'ON' if se else 'OFF'}",
        f"-DENABLE_SKYRIM_AE={'ON' if ae else 'OFF'}",
        "-DENABLE_SKYRIM_VR=OFF",
        "-DBUILD_TESTS=OFF",
        "-DSKSE_SUPPORT_XBYAK=OFF",
    ]
    print(f">> configure {build_dir}  SE={se} AE={ae}")
    r = subprocess.run(cmd, env=make_env(), capture_output=True, text=True, errors="replace")
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    return r.returncode


def build(build_dir: str) -> int:
    cmd = [CM, "--build", build_dir, "--config", "Release", "--", "/nologo", "/v:m"]
    print(f">> build {build_dir}")
    r = subprocess.run(cmd, env=make_env(), capture_output=True, text=True, errors="replace")
    for line in (r.stdout + r.stderr).splitlines():
        if any(t in line for t in (
                "error", "Error", "错误", "warning C",
                "Build succeeded", "生成成功", "FAILED", "失败")):
            print(line)
    return r.returncode


def main() -> int:
    rc = 0
    rc |= configure(SE_DIR, True, False)
    rc |= build(SE_DIR)
    rc |= configure(AE_DIR, True, True)
    rc |= build(AE_DIR)
    print("ALL DONE rc =", rc)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
