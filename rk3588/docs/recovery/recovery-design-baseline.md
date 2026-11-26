# Recovery Design Baseline

This manifest records the design input used by the recovery kernel implementation.

- Authoritative path: `/mnt/e/Obsidian_warehouse/嵌入式/1-Project/RK3588工业安全监测/2026-11-02-DMA-BUF恢复与证明内核设计.md`
- status: **INVALIDATED / PENDING_REVIEW**
- Current frozen SHA-256: **none** — there is no executable frozen hash.
- Required heading: `## 18. 反向测试矩阵`
- Required heading: `## 19. 真机 capability 待验证清单`
- Required heading: `## 20. 编译闭合检查清单`

## Why this baseline is invalidated

Task 4 entered `DESIGN_BLOCKER`: §6.3 called `close_fault_batch()` before Phase A (mutating `fault_batch_open` / `fault_batch_cutoff` / `fault_batch_digest`) while Phase A promised zero publication side effects, Phase B was pinned to `registry → snapshot → state`, and §6.4 still required those three runtime fields to be closed after success. No implementation can satisfy all four requirements at once. The fix removes the duplicated runtime fields and makes `RecoverySlotState` + `FrozenRuntimeSnapshot` the single source of truth, with `state = FROZEN` as the only linearization point.

Until Codex reviews that replacement, no baseline is authoritative and no implementation may proceed.

## History (audit only, not authoritative)

- `65F25A5A7CBC7E18F39ADDFA54CA0E8ACE5C6E67DEBFBA75480548EB4F215F5A` — frozen 2026-11-11 after the previous review, **INVALIDATED** on 2026-11-12 by the fault-batch freeze-publication replacement. Tests executed under it keep their evidence for regression comparison only.
- `8B7C5DFD8AE8753CA05A06D130EC995735AB2B89B068861745B14BF5A265D9A8` — the pre-2026-11-10 baseline. INVALIDATED earlier by the Task 3 DESIGN_BLOCKER revision. Audit only.
- `CF6FD0CD6A64B691DB4607B93DA6F49ECC27B76AC24DD8C1441174C213924CC2` — pre-review candidate for the 2026-11-11 freeze. Audit only; never a baseline.

## Effect on gates

`tests/test_recovery_design_baseline.sh` compares the design document against the recorded SHA-256. While this manifest is `INVALIDATED`, that gate **fails by design** (no frozen hash can match): the red result means "no implementation may proceed against a stale baseline". Do not "fix" it by editing the recorded value or by loosening the test.

Re-arming happens only after Codex approves the replacement design: recompute the SHA-256, write `status: FROZEN` with `- SHA-256: \`<new hash>\``, and update the gate script's `expected_sha` in the same change.

The authoritative design is not copied or modified in this repository.
