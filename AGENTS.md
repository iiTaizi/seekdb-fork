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
