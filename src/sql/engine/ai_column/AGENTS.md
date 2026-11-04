# AI column SQL execution

- Own native DML capture expressions, typed constant model evaluation and the strict-read gate here. SQL planning, transactions, snapshots and index maintenance stay native.
- Capturing inputs must never call a model. Persist materialized typed arguments and actual row keys; never store LOB locators or expression pointers.
- Model execution reuses AI function/provider validation. Internal worker write permissions are scoped to native sessions and restored on every exit.
- Strict read drains an unfiltered control scan on the same statement snapshot before opening the main plan, including plans with no output rows.
- Keep byte/resource limits explicit and errors observable. No application worker or filter on returned rows may substitute for kernel behavior.
