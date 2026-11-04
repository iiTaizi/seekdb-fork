# AI SQL regression suite

- t/ and r/ retain native mysqltest inputs and expected results; preserve official assets.
- local_ai_column_regression.py is an independent local regression harness, not an official Mock or official acceptance.
- It may start only a fresh instance under the explicitly supplied task root, using the pinned build environment and successful build receipt. Never upgrade or reuse an existing database.
- Models, endpoints and tables belong only to that fresh instance. The harness may kill and restart only its own process to test durable leases.
- Write logs and data outside tracked source; preserve failed receipts. Assertions must check native SQL and actual HTTP requests rather than regenerate expected outputs.
