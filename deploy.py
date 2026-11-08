"""Copy the finished artefacts into the MO2 mod folder.

Source and scripts stay in D:/dev/3SU.  Because the engine struct layouts differ
between Skyrim SE (1.5.97) and AE (1.6.x) and are baked in at compile time, we
ship TWO DLLs:

    build_ae/Release/SkySaveSystemUltra.{dll,pdb}  ->  SKSE/Plugins/   (live, AE)
    build_se/Release/SkySaveSystemUltra.{dll,pdb}  ->  SkySaveSystemUltra/SE/

The mod directory only ever sees SkySaveSystemUltra.dll / .pdb / .ini / README.txt
in SKSE/Plugins, plus the SSS-Ultra-SaveDoctor.exe (version-independent) and the
SE-only DLL in its own SkySaveSystemUltra folder.  The DLL name is ASCII because
SKSE and the MO2 path handling both behave badly with non-ASCII module names; the
name shown in the SKSE plugin list ("SkySaveSystemUltra") comes from SKSEPluginInfo.
"""
import os
import shutil

SOURCE = r"D:\dev\3SU"
BUILD_SE = os.path.join(SOURCE, "build_se", "Release")
BUILD_AE = os.path.join(SOURCE, "build_ae", "Release")

MOD_NAME = "【自制】SkySaveSystemUltra"
MOD_DIR = os.path.join(r"E:\MO2（2.5.2）\AE-mods", MOD_NAME)
PLUGIN_DIR = os.path.join(MOD_DIR, "SKSE", "Plugins")
EXE_DIR = os.path.join(MOD_DIR, "SkySaveSystemUltra")
SE_DIR = os.path.join(EXE_DIR, "SE")

ARTEFACTS = ["SkySaveSystemUltra.dll", "SkySaveSystemUltra.pdb"]

# The repair exe is version-independent (pure C++ file parsing, no engine
# structs), so it is built once and taken from the AE tree.  Note its output
# path is build_ae/doctor/Release (RUNTIME_OUTPUT_DIRECTORY + VS config), not
# under build_ae/Release like the main DLL.
EXE_SRC = os.path.join(SOURCE, "build_ae", "doctor", "Release", "SSS-Ultra-SaveDoctor.exe")

SE_NOTE = (
    "这个目录里的 SkySaveSystemUltra.dll 是给【Skyrim SE 1.5.97】用的。\n"
    "如果你用的是 SE（不是 AE / 1.6.x），请把这个 dll 和 pdb 复制到上一级\n"
    "SKSE/Plugins/ 目录，覆盖掉那里给 AE 用的那份。\n"
    "AE（1.6.x）用户不需要动这里。\n"
)


def main() -> int:
    os.makedirs(PLUGIN_DIR, exist_ok=True)
    os.makedirs(EXE_DIR, exist_ok=True)
    os.makedirs(SE_DIR, exist_ok=True)

    locked = []

    # --- AE build -> live SKSE/Plugins (current user runs AE) ---
    for name in ARTEFACTS:
        src = os.path.join(BUILD_AE, name)
        if not os.path.isfile(src):
            print(f"missing AE build artefact: {src}")
            return 1
        dst = os.path.join(PLUGIN_DIR, name)
        try:
            shutil.copy2(src, dst)
            print(f"copied [AE] {name} -> {dst}")
        except PermissionError:
            print(f"[LOCKED] {dst} is in use (game running) -- close the game and re-run deploy.py")
            locked.append(dst)

    # --- SE build -> side folder for SE users ---
    for name in ARTEFACTS:
        src = os.path.join(BUILD_SE, name)
        if not os.path.isfile(src):
            print(f"missing SE build artefact: {src}")
            return 1
        dst = os.path.join(SE_DIR, name)
        shutil.copy2(src, dst)
        print(f"copied [SE] {name} -> {dst}")
    with open(os.path.join(SE_DIR, "README_SE.txt"), "w", encoding="utf-8") as f:
        f.write(SE_NOTE)

    # --- repair exe (version-independent) ---
    if os.path.isfile(EXE_SRC):
        dst = os.path.join(EXE_DIR, "SSS-Ultra-SaveDoctor.exe")
        shutil.copy2(EXE_SRC, dst)
        print(f"copied SSS-Ultra-SaveDoctor.exe -> {dst}")
    else:
        print(f"missing exe artefact: {EXE_SRC}")
        return 1

    for name in ("SkySaveSystemUltra.ini", "README.txt"):
        path = os.path.join(MOD_DIR, name) if name.endswith(".txt") else os.path.join(PLUGIN_DIR, name)
        if os.path.isfile(path):
            print(f"kept existing {name}")
        else:
            print(f"note: {name} not found at {path}")

    if locked:
        print(f"\n{len(locked)} file(s) skipped because they are locked by a running game.")
        print("Close Skyrim and run:  python deploy.py")
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
