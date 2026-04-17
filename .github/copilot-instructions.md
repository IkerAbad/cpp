## Purpose
Short, actionable guidance for an AI coding assistant working in this repository.

This repository contains small C++ example projects: a desktop/console "helloworld" sample and a Raspberry Pi Pico project that uses the Pico SDK. The file below focuses on the concrete patterns and commands an agent should know to be productive.

## Big picture
- Top-level folders:
  - `cpp/helloworld/` — small console examples (standard C++ iostream, vectors, basic demos).
  - `raspberry/` — Raspberry Pi Pico project built with CMake and the Pico SDK (single executable target `raspberry_project`).
- The `raspberry` target uses the Pico SDK via `pico_sdk_import.cmake` and `pico_add_extra_outputs` to produce UF2 and map outputs.

## Key files to reference
- `raspberry/CMakeLists.txt` — top-level CMake for the Pico target. Shows target name `raspberry_project`, links `pico_stdlib`, and calls `pico_add_extra_outputs`.
- `raspberry/main.cpp` — embedded app: `stdio_init_all()`, `gpio_*` API, `sleep_ms()` and `printf` usage for serial output.
- `raspberry/pico_sdk_import.cmake` — includes the SDK import helper; it expects `PICO_SDK_PATH` to be set (see Build steps).
- `cpp/helloworld/helloworld.cpp` and `cpp/helloworld/DataTypes.cpp` — simple local examples and style references for desktop C++ code.

## Build, run and flash (concrete steps)
Follow these discoverable steps. When writing commands for Windows PowerShell, use the PowerShell examples below.

1. Ensure the Pico SDK is available and the environment variable `PICO_SDK_PATH` points to its root. This repo uses `pico_sdk_import.cmake` which includes `${PICO_SDK_PATH}/external/pico_sdk_import.cmake`.
   - PowerShell example:
     $env:PICO_SDK_PATH = 'C:\path\to\pico-sdk'

2. Configure and build with CMake from the `raspberry` folder (recommended):
   - Configure and build (portable):
     cmake -S . -B build
     cmake --build build -- -j

3. Outputs and flashing
   - `pico_add_extra_outputs(raspberry_project)` creates UF2 and .elf files in `raspberry/build` (or `raspberry/build/<configuration>`).
   - To flash a Pico manually: copy the generated `.uf2` to the mounted USB mass storage device (named `RPI-RP2`) when the Pico is in BOOTSEL mode.

## Project-specific patterns and conventions
- Pico code uses the SDK C API and expects `#include "pico/stdlib.h"`. Common functions: `stdio_init_all()`, `printf()` (serial), `gpio_init`, `gpio_set_dir`, `gpio_put`, `sleep_ms()`.
- Keep the LED pin constant near the top of `main.cpp` (example: `const uint LED_PIN = 25;`). Use guarded, minimal main loops as shown.
- Desktop examples use `std::cout` and `std::vector` without special build tooling; they are place-holders for learning and should be kept simple.

## Editing and testing guidance for an AI agent
- When adding Pico features, update `raspberry/CMakeLists.txt` only to: add sources, modify `add_executable(...)` or `target_link_libraries(...)`. Avoid changing SDK import logic unless the user requests it.
- Prefer small, isolated changes and compile them locally in `raspberry/build`. After code edits, run the configure+build steps to validate.
- For runtime verification, use `printf`/`stdio_init_all()` output and the UF2 flash -> Pico serial monitor (or use a serial tool to read USB CDC output if available).

## Patterns to avoid / assumptions
- Do not assume advanced Windows toolchains are configured. If a generator or toolchain choice is required, ask the user for their environment or suggest setting `PICO_SDK_PATH` and using a generic `cmake -S . -B build` flow.
- There are no unit tests or CI files in the repo; do not invent tests without confirming the user's intent.

## Example edits and snippets (use these verbatim when appropriate)
- Add a new source to the Pico target in `raspberry/CMakeLists.txt`:
  Replace: add_executable(raspberry_project main.cpp)
  With: add_executable(raspberry_project main.cpp src/new_module.cpp)

- Typical minimal runtime check in `raspberry/main.cpp` (already used as canonical style):
  stdio_init_all();
  printf("LED ON\n");

## If you need clarification
- Ask where the user's Pico SDK is located and what host toolchain they prefer (MSYS2/MinGW, Visual Studio, or Linux). Provide both PowerShell and POSIX variants when proposing commands.

---
If you'd like, I can: (1) add a short `README.md` with the exact example PowerShell commands tailored to your environment, or (2) detect an existing Pico SDK path on this machine and write a sample `build.ps1`. Which would you prefer?
