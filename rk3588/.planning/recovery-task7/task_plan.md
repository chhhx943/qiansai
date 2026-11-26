# Recovery Task 7 Execution

## Goal

Implement epoch commit, single reopen, and cross-recovery isolation using the approved candidate design and `docs/superpowers/plans/2026-11-26-recovery-task7-implementation.md`.

## Phases

1. Shared epoch algebra and successful commit - complete
2. Commit rejection and atomicity matrix - complete
3. Reopen authorization and commit - complete
4. Cross-recovery isolation - complete
5. Traceability and full verification - complete

## Constraints

- Preserve all existing dirty-tree work and continue in the current feature branch.
- Follow RED -> GREEN for every behavior slice.
- Keep all evidence candidate-only while the baseline is invalidated.
- Do not implement Task 8 trusted IPC beyond the existing opaque context seam.
- Never return failure after Stage B starts mutating epoch/reopen state.

## Errors Encountered

| Error | Attempt | Resolution |
| --- | --- | --- |
| Same-resource conflicting fault epochs were silently collapsed during plan derivation | Phase 1 review | Add a RED compiler regression and restore explicit conflict rejection in Phase 2 |
| R33 test mutated a truly const program through `const_cast` | Phase 2 quality review | Inject conflict into mutable `CompileResult`, recompute hash, then run the normal seal/execute path |
| Generic wrong-phase calls were treated as duplicate COMMIT | Phase 2 spec review | Fail closed only on COMMITTING re-entry; preserve ordinary pre-EXECUTING wrong-phase state |
| Public runtime observers raced with atomic reopen and `active.reset()` | Phase 3 spec review | Add a mutex-protected coordinator snapshot and restrict raw `active()` access to test builds |
| Cross-recovery test needed nonce/entry observability without exposing permit internals | Phase 4 RED | Add one `RECOVERY_TESTING`-only, mutex-protected value snapshot friend; keep `PermitHandle` fields private |

## Decisions

- Proceed with Task 7 while closing Task 7-dependent Task 6 evidence; do not block on the circular gate.
- Promote epoch algebra to one shared compiler/runtime implementation rather than duplicate it.
- Work in place because a fresh worktree from HEAD would omit uncommitted Task 4-6 prerequisites.
- Compare recovery 2's first ordinary nonce against recovery 1's final REOPEN nonce/counter, not merely its first ordinary nonce.
- Keep RK3588 target runtime evidence `NOT_RUN`; the cross-check proves only AArch64 compile/link.
