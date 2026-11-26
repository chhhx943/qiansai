# Recovery Task 6 Findings

- Task 6 starts at `docs/superpowers/plans/2026-11-09-dma-buf-recovery-kernel.md` and owns authorization journal, claim/poll, and ordinary receipt commit.
- Existing recovery code implements canonical/compiler/runtime freeze/seal/proof behavior but has no `authorization.hpp` or `authorization.cc`.
- The authoritative candidate design specifies a three-state authorization lifecycle: `DELIVERABLE -> CLAIMED -> CONSUMED`.
- Authorization preparation must allocate and hash entirely before a nothrow journal publication and nonce commit.
- Request idempotency is permanent for the lifetime of a recovery runtime; consumption changes lifecycle but does not delete the request mapping.
- Ordinary receipt submission validates without side effects, prepares all allocation-bearing state, then consumes the permit and publishes the receipt in a nothrow commit.
- Potential ordering issue: Task 6 claim/poll requires `AuthenticatedExecutorContext`, while the total plan assigns authentication implementation to Task 8. This boundary must be resolved without creating a forgeable public context.
- Production runtime currently stops at `SEALED`; `begin_quiesce`, proof submission, `commit_barrier`, barrier storage, and runtime table preallocation are absent even though Task 5 is described as candidate-complete.
- The authorization phase cannot be reached without closing that Task 5 runtime transition gap.
- The authentication boundary will be represented by an opaque, privately constructed context and verifier interface. Tests may use a `RECOVERY_TESTING` friend; production construction remains unavailable until the trusted IPC/auth implementation is completed.
- For the first RED slice, a legal program with no proof predicates makes `barrier_complete` true for an empty `ProofIndex`, allowing the runtime transition to be tested without target capabilities.
- Canonical compiler output assigns dense ActionIds `0..N-1`; journal and receipt vector indexing therefore matches the validated program invariant.
- Authorization progression is batch-gated: if any entry is DELIVERABLE or CLAIMED, no successor batch is constructed. This is required for R111 and prevents one completed parallel branch from advancing past its peer.
- Ordinary receipt acceptance uses three stages: side-effect-free validation, allocation-bearing preparation, then a statically nothrow consume/request/index commit.
- Task 6 does not implement epoch commit, reopen, cross-recovery cleanup, production trusted IPC issuance, or target capability execution; those remain Tasks 7-10.
