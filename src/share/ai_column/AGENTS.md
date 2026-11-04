# Durable AI column protocol

- Keep bounded value types, encoding and state transitions here. No SQL, networking, background threads or application transaction substitutes.
- Persist typed input and row identity. Reject pointer-bearing values and unmaterialized LOB locators.
- State transitions require an atomic database compare-and-swap around the old serialized row state; these helpers alone do not provide transaction isolation.
- Keep public status distinct from execution stage. Publish all still-current members of one group atomically through normal DML.
- New headers use pragma once; do not change existing column flags or persisted schema without documenting their compatibility and testing real DDL.
