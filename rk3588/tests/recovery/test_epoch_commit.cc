#include "epoch_runtime_test_access.hpp"
#include "task7_flow_fixture.hpp"

#include <limits>
#include <new>
#include <variant>

namespace {

struct ThrowAt final : recovery::RuntimeFailureProvider {
    recovery::RuntimeFailureStage target;
    bool armed{};

    explicit ThrowAt(recovery::RuntimeFailureStage value) : target(value) {}

    void checkpoint(recovery::RuntimeFailureStage value) override {
        if (armed && value == target) throw std::bad_alloc{};
    }
};

struct CommitAtomicState {
    recovery::EpochSnapshot epochs;
    std::vector<std::optional<recovery::ActionReceipt>> receipt_slots;
    std::vector<recovery::ActionId> accepted_order;
    std::size_t accepted_count;
    std::optional<recovery::AuthorizationLifecycle> lifecycle;
    std::optional<std::uint64_t> issue_nonce;
    std::optional<recovery::PermitTokenDigest> token_digest;
    auto operator<=>(const CommitAtomicState&) const = default;
};

CommitAtomicState atomic_state(const recovery::RecoveryCoordinator& coordinator) {
    const auto id = recovery::EpochRuntimeTestAccess::commit_action_id(coordinator);
    const auto* entry = coordinator.active().journal.find_entry(id);
    return {
        coordinator.epochs(),
        coordinator.active().receipts.slots,
        coordinator.active().receipts.accepted_order,
        coordinator.active().receipts.accepted_count,
        coordinator.active().journal.lifecycle_of(id),
        entry ? std::optional{entry->issue_nonce} : std::nullopt,
        entry ? std::optional{entry->token_digest} : std::nullopt,
    };
}

void check_error(const recovery::CommitResult& result,
                 recovery::CommitErrorCode expected) {
    RECOVERY_CHECK(std::holds_alternative<recovery::CommitError>(result));
    RECOVERY_CHECK_EQ(std::get<recovery::CommitError>(result).code, expected);
}

}  // namespace

RECOVERY_TEST(R31_successful_epoch_commit_is_atomic_and_enters_committing) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    const auto& runtime = flow.coordinator->active();
    const auto before = runtime.current_snapshot->epoch_snapshot;
    const auto domains = canonical_epoch_domains(
        runtime.current_program->resource_plans());
    const auto expected = advance_epochs(before, domains);
    RECOVERY_CHECK(!domains.empty());
    RECOVERY_CHECK(expected.has_value());
    const auto commit_id =
        EpochRuntimeTestAccess::commit_action_id(*flow.coordinator);
    const auto accepted_before = runtime.receipts.accepted_count;

    auto result = flow.coordinator->commit_epoch();

    RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(result));
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::COMMITTING);
    RECOVERY_CHECK_EQ(flow.coordinator->epochs(), *expected);
    const auto& receipt = std::get<CommitSuccess>(result).receipt;
    RECOVERY_CHECK_EQ(receipt.operation, OperationKind::COMMIT_EPOCH);
    RECOVERY_CHECK_EQ(receipt.old_epochs, before);
    RECOVERY_CHECK_EQ(receipt.new_epochs, expected);
    RECOVERY_CHECK(flow.coordinator->active().receipts.slots[commit_id].has_value());
    RECOVERY_CHECK_EQ(*flow.coordinator->active().receipts.slots[commit_id],
                      receipt);
    RECOVERY_CHECK_EQ(flow.coordinator->active().receipts.accepted_count,
                      accepted_before + 1);
    RECOVERY_CHECK_EQ(
        flow.coordinator->active().receipts.accepted_order[accepted_before],
        commit_id);
    RECOVERY_CHECK_EQ(flow.coordinator->active().journal.lifecycle_of(commit_id),
                      std::optional{AuthorizationLifecycle::CONSUMED});
}

RECOVERY_TEST(R32_old_epoch_mismatch_fails_closed_without_partial_commit) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    auto mismatched = flow.coordinator->active().current_snapshot->epoch_snapshot;
    ++mismatched.stream_epoch;
    EpochRuntimeTestAccess::set_snapshot_epochs(*flow.coordinator, mismatched);
    const auto before = atomic_state(*flow.coordinator);

    const auto result = flow.coordinator->commit_epoch();

    check_error(result, CommitErrorCode::OLD_EPOCH_MISMATCH);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R33_shared_domain_old_epoch_conflict_is_atomic_and_fails_closed) {
    using namespace recovery;
    auto flow = test::media_flow_at_commit_deliverable(nullptr, true);
    RECOVERY_CHECK_EQ(flow.coordinator->active().current_program
                          ->resource_plans().size(), 2U);
    const auto before = atomic_state(*flow.coordinator);

    const auto result = flow.coordinator->commit_epoch();

    check_error(result, CommitErrorCode::DOMAIN_CONFLICT);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R34_R51_shared_media_domain_advances_once_and_preserves_others) {
    using namespace recovery;
    auto flow = test::media_flow_at_commit_deliverable();
    const auto before = flow.coordinator->epochs();
    const auto domains = canonical_epoch_domains(
        flow.coordinator->active().current_program->resource_plans());
    RECOVERY_CHECK_EQ(domains, std::set<EpochDomain>{EpochDomain::MEDIA});

    const auto result = flow.coordinator->commit_epoch();

    RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(result));
    const auto after = flow.coordinator->epochs();
    RECOVERY_CHECK_EQ(after.media_epoch, before.media_epoch + 1);
    RECOVERY_CHECK_EQ(after.stream_epoch, before.stream_epoch);
    RECOVERY_CHECK_EQ(after.ai_epoch, before.ai_epoch);
    RECOVERY_CHECK_EQ(after.worker_epoch, before.worker_epoch);
}

RECOVERY_TEST(R35_epoch_overflow_fails_closed_without_partial_commit) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    auto overflow = flow.coordinator->epochs();
    overflow.stream_epoch = std::numeric_limits<std::uint64_t>::max();
    EpochRuntimeTestAccess::set_committed_epochs(*flow.coordinator, overflow);
    const auto before = atomic_state(*flow.coordinator);

    const auto result = flow.coordinator->commit_epoch();

    check_error(result, CommitErrorCode::EPOCH_OVERFLOW);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R36_R37_receipt_prepare_failure_has_no_partial_commit) {
    using namespace recovery;
    ThrowAt failure{RuntimeFailureStage::RECEIPT_PREPARE};
    auto flow = test::flow_at_commit_deliverable(&failure);
    const auto before = atomic_state(*flow.coordinator);
    failure.armed = true;

    const auto result = flow.coordinator->commit_epoch();

    check_error(result, CommitErrorCode::RECEIPT_REJECT);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R38_duplicate_commit_fails_closed_without_second_receipt) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(
        flow.coordinator->commit_epoch()));
    const auto before = atomic_state(*flow.coordinator);

    const auto duplicate = flow.coordinator->commit_epoch();

    check_error(duplicate, CommitErrorCode::WRONG_PHASE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R38_duplicate_commit_fails_closed_with_missing_commit_entry) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(
        flow.coordinator->commit_epoch()));
    EpochRuntimeTestAccess::erase_commit_entry(*flow.coordinator);
    const auto before = atomic_state(*flow.coordinator);

    const auto duplicate = flow.coordinator->commit_epoch();

    check_error(duplicate, CommitErrorCode::WRONG_PHASE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(pre_executing_commit_wrong_phase_preserves_active_state) {
    using namespace recovery;
    RecoveryCoordinator coordinator{11};
    RECOVERY_CHECK(coordinator.report_fault(
        {FaultKind::PACKET_DOMAIN_FAULT,
         {1, std::nullopt, std::nullopt},
         {1, 2, 3, 4},
         {1, {}}}));
    RECOVERY_CHECK_EQ(coordinator.state(), RecoverySlotState::PREPARING);

    const auto result = coordinator.commit_epoch();

    check_error(result, CommitErrorCode::WRONG_PHASE);
    RECOVERY_CHECK_EQ(coordinator.state(), RecoverySlotState::PREPARING);
    RECOVERY_CHECK(coordinator.has_active());
}

RECOVERY_TEST(R54_missing_commit_entry_rejects_without_epoch_or_receipt_write) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    EpochRuntimeTestAccess::erase_commit_entry(*flow.coordinator);
    const auto before = atomic_state(*flow.coordinator);

    const auto result = flow.coordinator->commit_epoch();

    check_error(result, CommitErrorCode::PERMIT_NOT_ISSUED);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::EXECUTING);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R54_non_deliverable_commit_entry_rejects_without_mutation) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    EpochRuntimeTestAccess::set_commit_lifecycle(
        *flow.coordinator, AuthorizationLifecycle::CLAIMED);
    const auto before = atomic_state(*flow.coordinator);

    const auto result = flow.coordinator->commit_epoch();

    check_error(result, CommitErrorCode::PERMIT_NOT_ISSUED);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::EXECUTING);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}
