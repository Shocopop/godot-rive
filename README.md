# Godot Rive

A Godot 4.5+ GDExtension using the Rive C++ runtime and Rive's Vulkan renderer.

The current backend targets **Windows x86_64, Vulkan, Forward+ or Mobile**.
Skia is no longer linked. macOS, Linux, mobile exports, Web, Compatibility and
Direct3D are not supported by this backend yet.

## Rendering

Rive draws into a GPU texture exposed to Godot through `Texture2DRD`.
Animation frames are neither read back to the CPU nor uploaded as images.
The viewer uses premultiplied-alpha blending; custom materials must preserve it.

This first implementation serializes GPU work with queue waits. It prioritizes
correctness over GPU overlap and is not a throughput-optimized renderer for
large numbers of viewers. A small compute operation establishes Godot's texture
layout when allocating or resizing a target; it reads back a 16-byte buffer once,
not animation frames. Targets are recreated only when their size changes.

Existing artboard, animation, state-machine and pointer APIs remain. Updating the
runtime does not expose every newer Rive feature to GDScript automatically.

## Build on Windows

Use an x64 Visual Studio 2022 Developer PowerShell with:

- C++ Build Tools, Windows SDK, Clang and **MSBuild support for LLVM (clang-cl)**.
- Python 3 and SCons (`python -m pip install scons`). Rive's shader build also
  requires a working `python3` command.
- Git for Windows, with its `usr/bin` tools (`sh`, `unzip`, etc.) on `PATH`.
- GNU Make (`python -m pip install gnumake`).
- Vulkan SDK shader tools (`glslangValidator` and `spirv-opt`) on `PATH`.

Initialize the pinned submodules, then run from the repository root:

```powershell
git submodule update --init --recursive
python build/build.py -j12
```

The first build downloads Rive's pinned dependencies and compiles its shaders.
Both extension configurations link the release Rive runtime. Build failures
return a nonzero exit code.

```powershell
python build/build.py --target release -j12
python build/build.py --skip-rive -j12
```

Outputs are `demo/bin/librive.windows.template_debug.x86_64.dll` and the
corresponding `template_release` DLL. Copy the DLLs, `demo/rive.gdextension` and
`demo/icons/` into your project, preserving their relative paths.

## Verify

Open `demo/project.godot` in Godot with the Vulkan driver. Run the GPU smoke test
with an actual graphics device (do not use `--headless`):

```powershell
godot --path demo --rendering-driver vulkan --script res://tests/gpu_smoke.gd
```

The test checks changing animation pixels, resizing, representative clipping,
blend, text and image assets, pause, retained file resources and the original
multi-viewer demo. It also exercises the upstream feather fixture when present.
It saves screenshots to `user://` (or `RIVE_TEST_OUTPUT`). Screenshot readback
is test-only. `RIVE_TEST_EXTENSION` can select an alternate extension descriptor.

Verified on Godot 4.6.3 / Windows / NVIDIA RTX 5080: debug DLL with Mobile and
release DLL with Forward+, both with Vulkan validation enabled and no validation
errors. These are smoke tests, not pixel-perfect comparisons or performance
benchmarks. Other GPUs and platforms have not been validated.
