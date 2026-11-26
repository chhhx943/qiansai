# Recovery Task 6 Execution Plan

## Goal

Implement the authorization journal, idempotent claim/poll delivery, and ordinary action receipt commit described by Task 6 of the recovery kernel plan.

## Constraints

- Work against the candidate design only; keep evidence as `CANDIDATE PASS / pending re-freeze`.
- Preserve all existing dirty-tree changes.
- Follow RED -> GREEN for each behavior group.
- Do not implement epoch commit/reopen (Task 7) or the trusted IPC/authentication subsystem (Tasks 8-9) beyond the minimum interface boundary Task 6 requires.

## Phases

1. **Design and dependency review** - complete
2. **Authorization types and private journal** - complete
3. **Two-phase authorization advance** - complete
4. **Idempotent claim/poll delivery** - complete
5. **Ordinary receipt validation and commit** - complete
6. **Full verification and traceability update** - in_progress (implemented rows verified; 24 plan-required rows remain `NOT_RUN`, including Task 7-dependent cases)

## Errors Encountered

| Error | Attempt | Resolution |
| --- | --- | --- |
| Task 5 runtime stopped at SEALED | 1 | Added the proof-barrier transition required to reach EXECUTING. |
| Claim/poll auth boundary was scheduled for Task 8 | 1 | Added an opaque verifier-backed context; production issuance remains private for Task 8/9. |
| Receipt tests did not compile without `submit_action_receipt` | 1 | Expected RED; implemented validate/prepare/nothrow-commit stages. |
| Parallel branch test returned `CREATED_DELIVERABLE` while a peer root remained deliverable | 1 | Gate each authorization batch before computing new eligible nodes; return `ALREADY_DELIVERABLE`/`WAITING_INFLIGHT` until it drains. |
| Cross-check probe omitted the new authorization translation units | 1 | Added `auth.cc` and `authorization.cc` to the host/target probe link source list. |
| Review found FAIL_CLOSED claim/receipt bypass and OOM termination paths | 1 | Gate mutation APIs on EXECUTING, publish FAIL_CLOSED before best-effort diagnostics, contain lock/barrier allocation exceptions, and add terminal-state retry coverage. |

## Decisions

- The user's instruction to continue authorizes candidate implementation despite the invalidated baseline; it does not authorize claiming a frozen-baseline PASS.
- Work in the current feature branch because uncommitted Task 4/5 prerequisites would not exist in a new worktree.
- Task 6's implementation surface is complete, but the written full-evidence gate is not: 24 listed rows remain `NOT_RUN`, several of which require Task 7 epoch/reopen behavior.
