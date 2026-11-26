# Recovery Task 7 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add atomic epoch commit, a single coordinator-only reopen path, and complete destruction/isolation of per-recovery state.

**Architecture:** Reuse one exported set of pure epoch functions in both compiler and runtime. `commit_epoch` and `commit_reopen` each perform all allocation-bearing validation and receipt preparation before a nothrow state commit. `advance_authorizations` remains the only journal installation point and gains the COMMITTING branch that installs exactly one REOPEN entry before transitioning to REOPENING.

**Tech Stack:** C++20, existing recovery state machine, OpenSSL-backed canonical digests, custom host test runner, ASan/UBSan, AArch64 Buildroot sysroot probe.

---

### Task 1: Shared Epoch Algebra and Successful Epoch Commit

**Files:**
- Modify: `recovery/include/recovery/compiler.hpp`
- Modify: `recovery/src/compiler.cc`
- Modify: `recovery/include/recovery/authorization.hpp`
- Modify: `recovery/include/recovery/runtime.hpp`
- Modify: `recovery/src/runtime.cc`
- Create: `tests/recovery/task7_flow_fixture.hpp`
- Create: `tests/recovery/test_epoch_commit.cc`

- [ ] **Step 1: Write the failing successful-commit test**

Create a packet-domain flow fixture that reaches a DELIVERABLE COMMIT entry through real ordinary receipts. Assert the desired API and result:

```cpp
auto flow = recovery::test::flow_at_commit_deliverable();
const auto before = flow.coordinator->active().current_snapshot->epoch_snapshot;
const auto result = flow.coordinator->commit_epoch();
RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(result));
RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::COMMITTING);
RECOVERY_CHECK_EQ(flow.coordinator->epochs().worker_epoch, before.worker_epoch + 1);
```

- [ ] **Step 2: Run RED**

Run: `make -B recovery-test`

Expected: compilation fails because `CommitResult`, `commit_epoch`, or the read-only epoch accessor does not exist.

- [ ] **Step 3: Export one epoch algebra**

Declare and define these namespace-level functions, moving the current compiler implementation out of the anonymous namespace:

```cpp
std::optional<EpochSnapshot> advance_epochs(
    const EpochSnapshot&, const std::set<EpochDomain>&);
bool epoch_domains_consistent(const std::vector<RecoveryResourcePlan>&);
std::set<EpochDomain> canonical_epoch_domains(
    const std::vector<RecoveryResourcePlan>&);
```

The compiler and runtime must call the same functions; do not duplicate switch logic.

- [ ] **Step 4: Add commit result types and private helpers**

Add `CommitSuccess`, `CommitErrorCode`, `CommitError`, and `CommitResult` to `authorization.hpp`, with nothrow move static assertions. Add private declarations for unique-node lookup, accepted prerequisite reconstruction, exact LOCAL_COMPLETE acceptance, commit/reopen receipt construction, and runtime destruction.

- [ ] **Step 5: Implement `commit_epoch` with prepare/commit separation**

Stage A checks EXECUTING, exact LOCAL_COMPLETE acceptance, a unique DELIVERABLE COMMIT entry, old epoch equality, shared-domain consistency, overflow, exact compiler/runtime `new_epochs`, receipt validation, and prepared receipt capacity. Stage B contains only:

```cpp
slot_.epochs = *new_epochs;
commit_receipt_locked(std::move(prepared), *commit_node,
                      commit_lease, *slot_.active);
slot_.state = RecoverySlotState::COMMITTING;
return CommitSuccess{std::move(commit_receipt)};
```

- [ ] **Step 6: Run GREEN**

Run: `make recovery-test`

Expected: successful epoch test and all existing tests pass.

### Task 2: Epoch Commit Rejection and Failure Atomicity

**Files:**
- Modify: `recovery/include/recovery/runtime.hpp`
- Modify: `recovery/src/runtime.cc`
- Modify: `tests/recovery/test_epoch_commit.cc`

- [ ] **Step 1: Add RED tests for R32-R38 and R51**

Use a narrow `EpochRuntimeTestAccess` friend to alter only test state. Cover:

```cpp
// old snapshot/node epoch mismatch -> OLD_EPOCH_MISMATCH + FAIL_CLOSED
// shared domain old epoch disagreement -> DOMAIN_CONFLICT + FAIL_CLOSED
// current committed component UINT64_MAX -> EPOCH_OVERFLOW + FAIL_CLOSED
// untouched domains remain byte-identical on success
// RECEIPT_PREPARE injection -> no epoch/receipt/lease/state partial commit
// duplicate commit_epoch -> explicit rejection and no second receipt
```

- [ ] **Step 2: Run RED**

Run: `make recovery-test`

Expected: at least one negative assertion fails because runtime revalidation/failure behavior is incomplete.

- [ ] **Step 3: Implement minimal validation and fault handling**

Keep every allocation and digest operation before the epoch assignment. Any Stage A internal failure must preserve `slot_.epochs`, journal lifecycle, receipt slot/count, and state except for the specified FAIL_CLOSED transition.

- [ ] **Step 4: Run GREEN**

Run: `make recovery-test`

Expected: all epoch success and rejection tests pass.

### Task 3: Single Reopen Authorization and Atomic Reopen

**Files:**
- Modify: `recovery/include/recovery/runtime.hpp`
- Modify: `recovery/src/authorization.cc`
- Modify: `recovery/src/runtime.cc`
- Create: `tests/recovery/test_reopen.cc`

- [ ] **Step 1: Write RED reopen tests**

After successful `commit_epoch`, assert:

```cpp
RECOVERY_CHECK_EQ(coordinator.advance_authorizations().code,
                  AuthorizationAdvanceCode::CREATED_DELIVERABLE);
RECOVERY_CHECK_EQ(coordinator.state(), RecoverySlotState::REOPENING);
RECOVERY_CHECK_EQ(coordinator.claim_authorization(executor, 99).code,
                  ClaimCode::NONE_AVAILABLE);
auto result = coordinator.commit_reopen();
RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(result));
RECOVERY_CHECK_EQ(coordinator.state(), RecoverySlotState::IDLE);
RECOVERY_CHECK(!coordinator.has_active());
RECOVERY_CHECK(coordinator.admission_open());
```

Also test missing/non-deliverable reopen entry, exact `{commit_receipt_hash}` prerequisite, prepare allocation failure, and repeated reopen.

- [ ] **Step 2: Run RED**

Run: `make recovery-test`

Expected: missing COMMITTING branch and `commit_reopen` API/behavior failures.

- [ ] **Step 3: Implement COMMITTING advance**

Validate the accepted COMMIT receipt once, require receipt `new_epochs == slot_.epochs`, reconstruct exact reopen prerequisites, prepare one authorization entry, publish it through the existing nothrow journal install, and transition to REOPENING only after publication.

- [ ] **Step 4: Implement atomic reopen**

Stage A prepares and validates the REOPEN receipt and internal lease. Stage B consumes the entry, increments `closing_generation` exactly once, sets admission open, enters IDLE, resets the complete `active` runtime, and returns the separately prepared receipt by nothrow move.

- [ ] **Step 5: Run GREEN**

Run: `make recovery-test`

Expected: all reopen and prior tests pass.

### Task 4: Cross-Recovery Isolation, Evidence, and Full Verification

**Files:**
- Create: `tests/recovery/test_cross_recovery.cc`
- Modify: `docs/recovery/recovery-traceability.md`
- Modify: `.planning/recovery-task7/{task_plan.md,findings.md,progress.md}`

- [ ] **Step 1: Write RED two-recovery test**

Run two full recoveries on one coordinator and capture the first recovery id, delivery id, issue nonce, receipt state, generation, and old handle. Assert after first reopen and second start:

```cpp
RECOVERY_CHECK(second_recovery_id != first_recovery_id);
RECOVERY_CHECK(second_issue_nonce > first_issue_nonce);
RECOVERY_CHECK(second_generation > first_generation);
RECOVERY_CHECK(second_runtime_has_no_first_receipts_or_requests);
RECOVERY_CHECK(old_handle_is_rejected_without_mutation);
```

- [ ] **Step 2: Run RED**

Run: `make recovery-test`

Expected: missing cross-recovery observability or stale-handle rejection assertion fails.

- [ ] **Step 3: Add only required read-only observability and fix isolation gaps**

Expose scalar read-only accessors only where tests and production diagnostics need them. Do not expose journal mutators or permit construction.

- [ ] **Step 4: Update traceability conservatively**

Mark only directly executed rows as `CANDIDATE PASS / pending re-freeze`. Keep target-runtime capabilities and unrelated Task 8+ rows unchanged.

- [ ] **Step 5: Run full verification**

Run:

```bash
make -B recovery-test
make recovery-compile-fail-test
make recovery-sanitize-test
make recovery-cross-check
git diff --check
```

Expected: all commands exit zero; target probe remains compile/link only and is not reported as runtime verification.
