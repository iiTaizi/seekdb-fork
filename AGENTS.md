# Project Agent Guidance

## C++ Type Naming

- New C++ classes, including interface classes, do not need the legacy `Ob` prefix.
- Keep the `I` prefix for interface classes. For example, use `ICacheMemoryGetter` instead of `ObICacheMemoryGetter`.
- Do not rename existing types only to remove the `Ob` prefix unless the task explicitly requires it.

## C++ Headers

- Use `#pragma once` in new C++ header files instead of macro-based `#ifndef`/`#define` include guards.

## Code Review

- For pull request or diff review tasks, read and follow `.agents/skills/code-review/SKILL.md`.

## Test Placement

- Prefer adding regression and correctness coverage to the repository's established test suites and test directories.
- Avoid adding test-only or benchmark-only files under `tools/` unless the task specifically requires a reusable developer tool.

## AI Function Enhancement Competition

- Work on `feat/ai-function-enhancement`, based on this repository's local `master`; do not merge the competition branch or unrelated fixes.
- Follow `docs/developer-guide/zh/ai-function-enhancement.md` for the six subtasks, compatibility rules, and verification checkpoints. Update the guide before changing its agreed behavior.
- Use this branch's Bazel entry point (`python3 build.py`), pinned dependencies, and a dedicated WSL validation workspace. Keep build caches and instance data out of tracked source files.
- Keep the existing SQL signatures and Prompt object representation. Share protocol validation between scalar and batch execution, and preserve platform conditionals.
- Competition SQL cases belong in the existing `ai_function` mysqltest suite. Import only task-related cases; do not import unrelated Endpoint features or invent replacements for unpublished official tests or services.
- Report source inspection, successful compilation, locally executed tests, and official Mock acceptance separately. Never report unavailable official tests as passed.
- Ask before database DDL or cleanup, deleting files, changing credentials or system configuration, global installation, pushing, or publishing.

## Remaining seekdb kernel exercises

- 2026-11-03 user steering: finish File SQL and AI columns first. The user explicitly permits these two tasks to continue in the existing C-backed WSL exercise root `/var/tmp/seekdb-kernel-exercises-20261102`, overriding the general D-only storage rule for this scope. Keep the previous AI task root read-only. Monitor both host C: free bytes and Linux available RAM before builds and instance starts. Stop new large writes below 25 GiB host free space; prepare subsequent outputs on D: without migrating/unregistering WSL or deleting old caches. Existing authorization for this exercise root's isolated test DDL and task-owned temporary cleanup remains scoped to that root. FTS and final integration follow after these two tasks.

- Implement File SQL on `feat/file-sql`, asynchronous AI columns on `feat/ai-columns`, and FTS optimization on `perf/fts-index`; use `feat/seekdb-kernel-integration` for combined validation. Do not merge the whole competition branch.
- AI columns depend on the AI enhancement checkpoint `93aed587c9043961f9623df4bd6ce7560520abdc`; the other independent branches start at master `9065fb8c2`.
- Read and update the matching Chinese developer guide before changing behavior. Define directory rules before creating implementation or test files.
- SQL execution, transaction handling and index maintenance must remain in seekdb. Format libraries may decode files; external SQL engines and application-level feature substitutes are forbidden.
- Use Bazel and the pinned toolchain. The new WSL validation root is `/var/tmp/seekdb-kernel-exercises-20261102`; the previous AI task root is read-only.
- User-approved local fault fixtures are independent regression tools, never official competition tests or services. Keep source inspection, builds, local tests and official acceptance separate.
- Preserve upstream benchmark inputs, queries, timers and scoring. Store profiling output, generated data, build artifacts and reports outside tracked source.
- Local checkpoint commits are authorized. Ask separately before cleanup, isolated database DDL/internal metadata creation, credentials, system configuration, global installation, push or publication. Do not upgrade existing databases.

- 2026-11-03 monitoring adjustment requested by Felix: check host disk/RAM about every 30 minutes and before a full build or a new isolated instance, not before routine reads, edits or log polls. Retain the 25 GiB stop threshold.

- 2026-11-04 Felix explicitly authorized autonomous overnight continuation and all task-required permission choices. Continue the approved four-exercise implementation, isolated verification, local checkpointing and final remote delivery without asking routine approvals again. Keep actions within this task; preserve old databases, old task caches, credentials and system configuration. Platform-required execution approval still follows tool policy. Record failures and unverified items honestly.
