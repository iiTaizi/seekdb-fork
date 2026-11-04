# AI column protocol tests

- Exercise the production codec and transition helpers, including late results, lease recovery, retry bounds, deleted-row identity, partial group replacement and successful NULL.
- Native tests prove state invariants only. Report SQL transaction, persistence, scheduler and index validation separately.
- Declare the target in BUILD.bazel and use the pinned toolchain; generated files belong in the authorized exercise root.
