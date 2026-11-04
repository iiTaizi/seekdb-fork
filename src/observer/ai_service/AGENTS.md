# AI service and asynchronous column worker

- Existing model/endpoint management remains compatible. Implement the AI column service as an owned native background worker with explicit start, stop and wait.
- Claim and publish committed row state using typed native SQL parameters and byte CAS. Never hold a business transaction or row lock during HTTP.
- Verify row identity, typed row key, column definition version, request and lease owner again after model execution. Deleted or superseded work cannot recreate or overwrite rows.
- Use SQL-owned input capture, model evaluation and target conversion. No application worker, model-name conditions or mock-specific responses.
- Bound polling batches and resource usage. Preserve retry deadlines and terminal failure, publish current group members together, and release connections and temporary values on every exit.
- Source, compilation, local SQL/fault tests and official acceptance must be reported separately.
