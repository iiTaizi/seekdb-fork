# SQL regression test layout

- `engine/expr/` contains native SQL expression and protocol utility regressions.
- Declare every executable in its local `BUILD.bazel`; use real production APIs through narrowly scoped test-only module interfaces.
- Use the repository's pinned Bazel, toolchain, and Google Test; each executable defines one Google Test main.
- Do not introduce a replacement for unpublished official tests or the competition Mock service. Keep local unit evidence separate from SQL integration and official acceptance.
- Keep generated files and logs in the dedicated WSL task directory. Ask before changing the directory convention, database DDL, or deleting user data.
