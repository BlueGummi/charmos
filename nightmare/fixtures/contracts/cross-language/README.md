# Cross-language contract fixtures

The accepted plan and fleet snapshot in `../valid/` are the canonical P8 v1
fixtures for the Python orchestrator. CI Studio vendors byte-for-byte copies at
`src/domain/__fixtures__/upstream/` and reads them through its production Zod
schemas in `src/domain/__tests__/upstream-contracts.test.ts`.

A fixture refresh is a user-reviewed content change; neither repository fetches
or rewrites the other during CI.
