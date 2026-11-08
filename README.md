# SkySaveSystemUltra (SSS-Ultra)

An SKSE plugin for Skyrim that hardens the save/load system: it classifies
saves, repairs saves that reference removed plugins (missing masters), and keeps
load-order state consistent across sessions. A companion standalone repair tool
(`SSS-Ultra-SaveDoctor.exe`) fixes a broken `.ess` on the raw file layer before
the engine ever parses it, so a save referencing uninstalled mods no longer
triggers the "The following content is missing" dialog.

## Runtime support

- **Skyrim SE** 1.5.97
- **Skyrim AE / GOG** 1.6.x

Runtime function addresses are resolved through the **Skyrim Script Extender
Address Library**, so any game version that has a matching address library is
supported. Because the engine's reverse-engineered struct layouts differ between
SE and AE, **two separate binaries are shipped** (see *Building* below).

## Repository layout

| Path | Purpose |
|------|---------|
| `src/` | The SKSE plugin (DLL): save classification, repair policy, state store, load-order tracking, the DLL-side SaveDoctor launcher. |
| `doctor/` | `SSS-Ultra-SaveDoctor.exe` — standalone, dependency-free `.ess` repair tool (`ess.hpp`, `lz4.hpp`, `main.cpp`). No SKSE, no CommonLibSSE. |
| `tools/` | Engine-free offline test programs (CodecTest, PathTest, NotifyTest, HeaderTest, JournalTest, StateTest, RepairTest) that prove each subsystem without Skyrim. |
| `CMakeLists.txt` | Top-level build definition (plugin + SaveDoctor + tests). |
| `build_all.py` | Configure + build both the SE and AE trees with Visual Studio 2022. |
| `run_build.py` | Single-target build helper (defaults to AE; pass `se`/`ae`). |
| `deploy.py` | Copy finished artefacts into the MO2 mod folder. |
| `docs/description.md` | Nexus Mods release description (English BBCode + Chinese). |

## Prerequisites

- **Visual Studio 2022** with the **Desktop development with C++** workload (MSVC).
- **CMake** ≥ 3.21 (the VS2022-bundled CMake is used by the build scripts).
- **Git**.
- C++23 support (the project sets `CMAKE_CXX_STANDARD 23`).

## Dependencies (external, not vendored)

The build expects two dependencies on fixed local paths (mirroring the
`maintainer`'s machine). Point them at your own clones if you prefer different
locations — the paths live at the top of `CMakeLists.txt`
(`add_subdirectory("D:/dev/CommonLibSSE-NG" ...)`) and
`CMAKE_PREFIX_PATH "D:/dev/deps"`.

1. **CommonLibSSE-NG** — clone a recent version to `D:/dev/CommonLibSSE-NG`.
   ```bat
   git clone https://github.com/CharmedBaryon/CommonLibSSE-NG.git D:/dev/CommonLibSSE-NG
   ```
2. **spdlog** (header-only) — make it discoverable under `D:/dev/deps`
   (e.g. `D:/dev/deps/spdlog`). The build adds `D:/dev/deps` to
   `CMAKE_PREFIX_PATH`.

## Building

The reverse-engineered engine structs (`TES`, `TESObjectCELL`,
`BGSSaveLoadManager`, …) pick their member layout at **compile time** from the
legacy `SKYRIM_SUPPORT_AE` macro, so a single DLL cannot be struct-correct for
both runtimes. Two isolated build trees are produced:

- `build_se/` → **SE** layout (`ENABLE_SKYRIM_SE=ON`, `ENABLE_SKYRIM_AE=OFF`)
- `build_ae/` → **AE** layout (`ENABLE_SKYRIM_SE=ON`, `ENABLE_SKYRIM_AE=ON`,
  which also defines `SKYRIM_SUPPORT_AE=1`)

### Option A — one shot (both trees)

```bat
python build_all.py
```

### Option B — manual CMake

```bat
:: AE
cmake -S . -B build_ae -G "Visual Studio 17 2022" -A x64 ^
  -DENABLE_SKYRIM_SE=ON -DENABLE_SKYRIM_AE=ON -DENABLE_SKYRIM_VR=OFF ^
  -DBUILD_TESTS=OFF -DSKSE_SUPPORT_XBYAK=OFF
cmake --build build_ae --config Release

:: SE
cmake -S . -B build_se -G "Visual Studio 17 2022" -A x64 ^
  -DENABLE_SKYRIM_SE=ON -DENABLE_SKYRIM_AE=OFF -DENABLE_SKYRIM_VR=OFF ^
  -DBUILD_TESTS=OFF -DSKSE_SUPPORT_XBYAK=OFF
cmake --build build_se --config Release
```

### Outputs

- `build_ae/Release/SkySaveSystemUltra.dll` (+ `.pdb`) — the live AE plugin.
- `build_se/Release/SkySaveSystemUltra.dll` (+ `.pdb`) — the SE-only build.
- `build_ae/doctor/Release/SSS-Ultra-SaveDoctor.exe` — the repair tool
  (version-independent; built once, used by both runtimes).

## Deploying to a mod manager

`deploy.py` copies the artefacts into an MO2 mod folder:

- AE `SkySaveSystemUltra.{dll,pdb}` → `SKSE/Plugins/`
- SE `SkySaveSystemUltra.{dll,pdb}` → `SkySaveSystemUltra/SE/` (SE users copy
  these over the AE copy in `SKSE/Plugins/`)
- `SSS-Ultra-SaveDoctor.exe` → `SkySaveSystemUltra/`

```bat
python deploy.py
```

## Offline tests (optional)

Each subsystem has an engine-free test target you can build and run directly
(no Skyrim, no SKSE): `CodecTest`, `PathTest`, `NotifyTest`, `HeaderTest`,
`JournalTest`, `StateTest`, `RepairTest`.

## License

MIT — see [LICENSE](LICENSE).
