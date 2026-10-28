# Building AutoLayout

There are two builds: the **mod** (needs the Geode SDK, Windows or macOS) and the **core** (tests,
command-line tools, training data tools; any OS, no Geode needed).

## Requirements

| | Windows | macOS |
|---|---|---|
| Compiler | Visual Studio 2022 (MSVC or clang-cl), C++23 | Xcode 15+ (Apple clang), C++23 |
| CMake | 3.25+ | 3.25+ |
| Geode SDK | 5.11.2 (`geode sdk install`, then `geode sdk install-binaries`) | same |
| Geometry Dash | 2.2081 | 2.2081 |

Install the Geode CLI from https://docs.geode-sdk.org/getting-started/geode-cli and set the
`GEODE_SDK` environment variable (the CLI does it for you).

## The mod

```bash
geode build
```

or with CMake directly:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```

The result is `build/bens8888.autolayout.geode`; `geode build` also installs it into your GD's
`geode/mods` folder. On macOS the build is universal (arm64 + x86_64).

When `GEODE_SDK` is not set, CMake builds the core only (next section).

## The core: tests and tools (any platform)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAL_CORE_ONLY=ON
cmake --build build -j
./build/tests/al_tests             # all unit + integration tests (about 1-2 minutes)
./build/tests/al_tests fuzz        # one group; AL_FUZZ_N=500 ./build/tests/al_tests fuzz for a long fuzz run
./build/tools/alcli bench          # performance benchmark
```

Useful tools:

```bash
./build/tools/alcli generate song.mp3 --difficulty 4 --variants 5 --out level.gmd --png preview.png
./build/tools/alcli generate demo --recording assets/demo/demo_clicks.alrec --faith 0.8
./build/tools/alcli analyze song.mp3 --json analysis.json
./build/tools/aldata selfplay --out data.jsonl --songs 100
```

`AL_DEBUG=1` prints the generator's decisions (why patterns were rejected, retries, transitions).

## Continuous integration

`.github/workflows/build.yml` runs on every push: the core tests on Linux (GCC), and the mod
builds for Windows and macOS with `geode-sdk/build-geode-mod`. The combined `.geode` file is
uploaded as the **Build Output** artifact of the workflow run.
