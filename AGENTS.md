# Agent instructions

- This is a Godot GDExtension integrating the Rive C++ runtime. Extension code lives in `src/`, build scripts in `build/`, and the example project in `demo/`.
- Use the **ponytail** skill for coding tasks. Read `$CODEX_HOME/skills/ponytail/SKILL.md` (defaults to `~/.codex/skills/ponytail/SKILL.md`). Source: https://github.com/DietrichGebert/ponytail/tree/main/skills/ponytail.
- Prefer the smallest correct change; reuse existing code and upstream capabilities before adding abstractions or dependencies.
- Keep dependency revisions pinned. Avoid modifying vendored code in `godot-cpp/` or `thirdparty/rive-cpp/` unless necessary.
- Build affected targets and verify rendering changes in the demo. Clearly report checks that could not run and remaining platform limitations.
