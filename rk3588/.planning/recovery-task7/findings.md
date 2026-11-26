# Recovery Task 7 Findings

- Candidate design §13 defines `commit_epoch`, COMMITTING reopen authorization, and `commit_reopen` as prepare/nothrow-commit transitions.
- Compiler epoch functions currently live in an anonymous namespace and must be shared with runtime to prevent algorithm drift.
- `RecoverySlot` has `closing_generation` but no explicit admission-open state; Task 7 must make reopen observability explicit.
- Task 6's remaining evidence list contains Task 7-dependent rows, so the literal pre-Task-7 gate is circular.
- The existing production behavior already rejected a stale recovery-1 handle in recovery 2; the missing piece was a narrow, locked, read-only test snapshot that could prove all related state stayed unchanged.
- A nonce comparison taken before recovery-1 REOPEN publication is insufficient. The isolation test now captures the final REOPEN entry and requires recovery 2's first ordinary nonce to be greater than that final boot-global counter.
- Both recoveries carry distinct ledger and Owner ACK facts. Recovery 2's runtime/frozen fault batch, ledger, ACK map, request index, receipt index, entry binding, and delivery id are checked independently of recovery 1.
- `CrossRecoveryTestAccess` is available only under `RECOVERY_TESTING`, returns copied values under the coordinator mutex, and does not expose or forge permit internals.
- The AArch64 probe is compile/link evidence only. No RK3588 target binary was executed in this task.
