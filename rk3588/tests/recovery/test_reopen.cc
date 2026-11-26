#include "epoch_runtime_test_access.hpp"
#include "permit_test_access.hpp"
#include "task7_flow_fixture.hpp"

#include <limits>
#include <new>
#include <type_traits>
#include <utility>
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

struct ReopenAtomicState {
    recovery::RecoverySlotState state;
    recovery::AdmissionGeneration generation;
    bool admission_open;
    bool has_active;
    std::vector<std::optional<recovery::ActionReceipt>> receipt_slots;
    std::vector<recovery::ActionId> accepted_order;
    std::size_t accepted_count;
    std::optional<recovery::AuthorizationLifecycle> reopen_lifecycle;
    auto operator<=>(const ReopenAtomicState&) const = default;
};

ReopenAtomicState atomic_state(
    const recovery::RecoveryCoordinator& coordinator) {
    if (!coordinator.has_active()) {
        return {coordinator.state(), coordinator.closing_generation(),
                coordinator.admission_open(), false, {}, {}, 0, std::nullopt};
    }
    const auto reopen =
        recovery::EpochRuntimeTestAccess::reopen_action_id(coordinator);
    return {
        coordinator.state(), coordinator.closing_generation(),
        coordinator.admission_open(), true,
        coordinator.active().receipts.slots,
        coordinator.active().receipts.accepted_order,
        coordinator.active().receipts.accepted_count,
        coordinator.active().journal.lifecycle_of(reopen),
    };
}

void check_error(const recovery::CommitResult& result,
                 recovery::CommitErrorCode expected) {
    RECOVERY_CHECK(std::holds_alternative<recovery::CommitError>(result));
    RECOVERY_CHECK_EQ(std::get<recovery::CommitError>(result).code, expected);
}

recovery::test::EpochCommitFlow flow_at_committing(
    recovery::RuntimeFailureProvider* provider = nullptr) {
    auto flow = recovery::test::flow_at_commit_deliverable(provider);
    RECOVERY_CHECK(std::holds_alternative<recovery::CommitSuccess>(
        flow.coordinator->commit_epoch()));
    RECOVERY_CHECK_EQ(flow.coordinator->state(),
                      recovery::RecoverySlotState::COMMITTING);
    return flow;
}

void check_advance_waits_without_reopening(
    recovery::RecoveryCoordinator& coordinator) {
    const auto generation = coordinator.closing_generation();
    RECOVERY_CHECK_EQ(coordinator.advance_authorizations().code,
                      recovery::AuthorizationAdvanceCode::WAITING_INFLIGHT);
    RECOVERY_CHECK_EQ(coordinator.state(),
                      recovery::RecoverySlotState::COMMITTING);
    RECOVERY_CHECK_EQ(coordinator.closing_generation(), generation);
    RECOVERY_CHECK(!coordinator.admission_open());
}

}  // namespace

RECOVERY_TEST(admission_is_initially_open) {
    recovery::RecoveryCoordinator coordinator{11};
    static_assert(!noexcept(
        std::declval<const recovery::RecoveryCoordinator&>().snapshot()));
    static_assert(!noexcept(
        std::declval<const recovery::RecoveryCoordinator&>().state()));
    const auto snapshot = coordinator.snapshot();
    RECOVERY_CHECK_EQ(snapshot.state, recovery::RecoverySlotState::IDLE);
    RECOVERY_CHECK_EQ(snapshot.epochs, recovery::EpochSnapshot{});
    RECOVERY_CHECK(snapshot.admission_open);
    RECOVERY_CHECK_EQ(snapshot.closing_generation, 0U);
    RECOVERY_CHECK(!snapshot.has_active);
    RECOVERY_CHECK(coordinator.admission_open());
    RECOVERY_CHECK_EQ(coordinator.closing_generation(), 0U);
}

RECOVERY_TEST(R27_R71_single_reopen_authorization_and_atomic_commit) {
    using namespace recovery;
    auto flow = flow_at_committing();
    const auto generation = flow.coordinator->closing_generation();
    const auto commit_id =
        EpochRuntimeTestAccess::commit_action_id(*flow.coordinator);
    const auto commit_hash =
        flow.coordinator->active().receipts.slots.at(commit_id)->receipt_hash;

    RECOVERY_CHECK(!flow.coordinator->admission_open());
    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::REOPENING);
    const auto reopen_id =
        EpochRuntimeTestAccess::reopen_action_id(*flow.coordinator);
    const auto* entry = flow.coordinator->active().journal.find_entry(reopen_id);
    RECOVERY_CHECK(entry != nullptr);
    RECOVERY_CHECK_EQ(entry->lifecycle, AuthorizationLifecycle::DELIVERABLE);
    RECOVERY_CHECK_EQ(entry->prerequisite_receipts,
                      std::set<ReceiptHash>{commit_hash});

    const auto after_publish = atomic_state(*flow.coordinator);
    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::PHASE_COMPLETE);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), after_publish);
    RECOVERY_CHECK_EQ(
        flow.coordinator->claim_authorization(flow.executor, 99).code,
        ClaimCode::NONE_AVAILABLE);

    const auto result = flow.coordinator->commit_reopen();

    RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(result));
    const auto& receipt = std::get<CommitSuccess>(result).receipt;
    RECOVERY_CHECK_EQ(receipt.operation, OperationKind::REOPEN_ADMISSION);
    RECOVERY_CHECK_EQ(receipt.action_id, reopen_id);
    RECOVERY_CHECK_EQ(receipt.prerequisite_receipts,
                      std::set<ReceiptHash>{commit_hash});
    RECOVERY_CHECK_EQ(flow.coordinator->closing_generation(), generation + 1);
    RECOVERY_CHECK(flow.coordinator->admission_open());
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::IDLE);
    RECOVERY_CHECK(!flow.coordinator->has_active());
}

RECOVERY_TEST(R27_generic_submit_cannot_consume_reopen) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    const auto& node = EpochRuntimeTestAccess::reopen_node(*flow.coordinator);
    ActionReceipt receipt{
        node.id, flow.coordinator->active().recovery_id,
        flow.coordinator->active().transaction_hash, node.id,
        OperationKind::REOPEN_ADMISSION, node.scope, node.old_epochs,
        node.new_epochs, ActionOutcome::SUCCEEDED, ReopenResult{true}, {}, {}};
    receipt.receipt_hash = compute_receipt_hash(receipt);
    auto handle = PermitHandleTestAccess::issue(
        receipt.recovery_id, receipt.transaction_hash, node.id, 1);

    RECOVERY_CHECK_EQ(
        flow.coordinator->submit_action_receipt(receipt, std::move(handle)).status,
        ReceiptSubmitResult::REJECT_USE_COMMIT_REOPEN);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::EXECUTING);
}

RECOVERY_TEST(R72_R73_R104_corrupt_local_complete_cannot_reissue_commit) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    EpochRuntimeTestAccess::erase_commit_entry(*flow.coordinator);
    EpochRuntimeTestAccess::corrupt_local_complete_receipt_binding(
        *flow.coordinator);
    const auto commit_id =
        EpochRuntimeTestAccess::commit_action_id(*flow.coordinator);

    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::PHASE_COMPLETE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::EXECUTING);
    RECOVERY_CHECK_EQ(flow.coordinator->active().journal.lifecycle_of(commit_id),
                      std::nullopt);
}

RECOVERY_TEST(R72_missing_local_complete_receipt_cannot_reissue_commit) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    EpochRuntimeTestAccess::erase_commit_entry(*flow.coordinator);
    EpochRuntimeTestAccess::erase_local_complete_receipt(*flow.coordinator);
    const auto commit_id =
        EpochRuntimeTestAccess::commit_action_id(*flow.coordinator);

    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::PHASE_COMPLETE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::EXECUTING);
    RECOVERY_CHECK_EQ(flow.coordinator->active().journal.lifecycle_of(commit_id),
                      std::nullopt);
}

RECOVERY_TEST(R104_accepted_order_extra_cannot_reissue_commit) {
    using namespace recovery;
    auto flow = test::flow_at_commit_deliverable();
    EpochRuntimeTestAccess::erase_commit_entry(*flow.coordinator);
    EpochRuntimeTestAccess::append_unbacked_commit_to_accepted_order(
        *flow.coordinator);
    const auto commit_id =
        EpochRuntimeTestAccess::commit_action_id(*flow.coordinator);

    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::PHASE_COMPLETE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::EXECUTING);
    RECOVERY_CHECK_EQ(flow.coordinator->active().journal.lifecycle_of(commit_id),
                      std::nullopt);
}

RECOVERY_TEST(R74_missing_accepted_commit_receipt_waits_in_committing) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::erase_commit_receipt(*flow.coordinator);
    check_advance_waits_without_reopening(*flow.coordinator);
}

RECOVERY_TEST(R74_non_consumed_commit_entry_waits_in_committing) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::set_commit_lifecycle(
        *flow.coordinator, AuthorizationLifecycle::DELIVERABLE);
    check_advance_waits_without_reopening(*flow.coordinator);
}

RECOVERY_TEST(R74_commit_entry_binding_mismatch_waits_in_committing) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::corrupt_commit_entry_binding(*flow.coordinator);
    check_advance_waits_without_reopening(*flow.coordinator);
}

RECOVERY_TEST(R74_commit_receipt_binding_mismatch_waits_in_committing) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::corrupt_commit_receipt_binding(*flow.coordinator);
    check_advance_waits_without_reopening(*flow.coordinator);
}

RECOVERY_TEST(R74_duplicate_commit_accepted_order_waits_in_committing) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::duplicate_commit_in_accepted_order(
        *flow.coordinator);
    check_advance_waits_without_reopening(*flow.coordinator);
}

RECOVERY_TEST(R74_commit_accepted_count_mismatch_waits_in_committing) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::hide_commit_from_accepted_count(*flow.coordinator);
    check_advance_waits_without_reopening(*flow.coordinator);
}

RECOVERY_TEST(R74_committed_epoch_mismatch_waits_in_committing) {
    using namespace recovery;
    auto flow = flow_at_committing();
    auto epochs = flow.coordinator->epochs();
    ++epochs.ai_epoch;
    EpochRuntimeTestAccess::set_accepted_epoch(*flow.coordinator, epochs);
    check_advance_waits_without_reopening(*flow.coordinator);
}

RECOVERY_TEST(R74_missing_reopen_node_fails_closed_without_publication) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::remove_reopen_node(*flow.coordinator);
    const auto generation = flow.coordinator->closing_generation();

    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::INTERNAL_FAILURE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(flow.coordinator->closing_generation(), generation);
    RECOVERY_CHECK(!flow.coordinator->admission_open());
}

RECOVERY_TEST(R74_duplicate_reopen_node_fails_closed_without_publication) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::duplicate_reopen_node(*flow.coordinator);
    const auto generation = flow.coordinator->closing_generation();

    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::INTERNAL_FAILURE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(flow.coordinator->closing_generation(), generation);
    RECOVERY_CHECK(!flow.coordinator->admission_open());
}

RECOVERY_TEST(R74_reopen_node_exact_prerequisite_mismatch_fails_closed) {
    using namespace recovery;
    auto flow = flow_at_committing();
    EpochRuntimeTestAccess::clear_reopen_node_prerequisites(*flow.coordinator);

    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::INTERNAL_FAILURE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(!flow.coordinator->admission_open());
}

RECOVERY_TEST(R85_reopen_authorization_prepare_failure_is_atomic) {
    using namespace recovery;
    ThrowAt failure{RuntimeFailureStage::AUTH_PREPARE};
    auto flow = flow_at_committing(&failure);
    const auto reopen_id =
        EpochRuntimeTestAccess::reopen_action_id(*flow.coordinator);
    const auto nonce =
        EpochRuntimeTestAccess::issue_nonce_counter(*flow.coordinator);
    const auto generation = flow.coordinator->closing_generation();
    failure.armed = true;

    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::INTERNAL_FAILURE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(flow.coordinator->active().journal.lifecycle_of(reopen_id),
                      std::nullopt);
    RECOVERY_CHECK_EQ(
        EpochRuntimeTestAccess::issue_nonce_counter(*flow.coordinator), nonce);
    RECOVERY_CHECK_EQ(flow.coordinator->closing_generation(), generation);
    RECOVERY_CHECK(!flow.coordinator->admission_open());
}

RECOVERY_TEST(COMMITTING_authorization_eligibility_failure_is_atomic) {
    using namespace recovery;
    ThrowAt failure{RuntimeFailureStage::AUTH_ELIGIBILITY};
    auto flow = flow_at_committing(&failure);
    const auto before = atomic_state(*flow.coordinator);
    const auto nonce =
        EpochRuntimeTestAccess::issue_nonce_counter(*flow.coordinator);
    failure.armed = true;

    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::INTERNAL_FAILURE);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    const auto after = atomic_state(*flow.coordinator);
    RECOVERY_CHECK_EQ(after.generation, before.generation);
    RECOVERY_CHECK_EQ(after.admission_open, before.admission_open);
    RECOVERY_CHECK_EQ(after.receipt_slots, before.receipt_slots);
    RECOVERY_CHECK_EQ(after.accepted_order, before.accepted_order);
    RECOVERY_CHECK_EQ(after.accepted_count, before.accepted_count);
    RECOVERY_CHECK_EQ(after.reopen_lifecycle, before.reopen_lifecycle);
    RECOVERY_CHECK_EQ(
        EpochRuntimeTestAccess::issue_nonce_counter(*flow.coordinator), nonce);
}

RECOVERY_TEST(R55_missing_reopen_entry_is_permit_not_issued_and_atomic) {
    using namespace recovery;
    auto flow = flow_at_committing();
    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    EpochRuntimeTestAccess::erase_reopen_entry(*flow.coordinator);
    const auto before = atomic_state(*flow.coordinator);

    check_error(flow.coordinator->commit_reopen(),
                CommitErrorCode::PERMIT_NOT_ISSUED);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R55_non_deliverable_reopen_entry_is_permit_not_issued_and_atomic) {
    using namespace recovery;
    auto flow = flow_at_committing();
    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    EpochRuntimeTestAccess::set_reopen_lifecycle(
        *flow.coordinator, AuthorizationLifecycle::CLAIMED);
    const auto before = atomic_state(*flow.coordinator);

    check_error(flow.coordinator->commit_reopen(),
                CommitErrorCode::PERMIT_NOT_ISSUED);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R69_exact_reopen_prerequisite_mismatch_rejects_atomically) {
    using namespace recovery;
    auto flow = flow_at_committing();
    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    EpochRuntimeTestAccess::clear_reopen_entry_prerequisites(*flow.coordinator);
    const auto before = atomic_state(*flow.coordinator);

    check_error(flow.coordinator->commit_reopen(),
                CommitErrorCode::RECEIPT_REJECT);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    const auto after = atomic_state(*flow.coordinator);
    RECOVERY_CHECK_EQ(after.generation, before.generation);
    RECOVERY_CHECK_EQ(after.admission_open, before.admission_open);
    RECOVERY_CHECK_EQ(after.receipt_slots, before.receipt_slots);
    RECOVERY_CHECK_EQ(after.accepted_order, before.accepted_order);
    RECOVERY_CHECK_EQ(after.accepted_count, before.accepted_count);
    RECOVERY_CHECK_EQ(after.reopen_lifecycle, before.reopen_lifecycle);
}

RECOVERY_TEST(R79_reopen_receipt_prepare_failure_has_no_partial_commit) {
    using namespace recovery;
    ThrowAt failure{RuntimeFailureStage::RECEIPT_PREPARE};
    auto flow = flow_at_committing(&failure);
    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    const auto before = atomic_state(*flow.coordinator);
    failure.armed = true;

    check_error(flow.coordinator->commit_reopen(),
                CommitErrorCode::RECEIPT_REJECT);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    const auto after = atomic_state(*flow.coordinator);
    RECOVERY_CHECK_EQ(after.generation, before.generation);
    RECOVERY_CHECK_EQ(after.admission_open, before.admission_open);
    RECOVERY_CHECK_EQ(after.receipt_slots, before.receipt_slots);
    RECOVERY_CHECK_EQ(after.accepted_order, before.accepted_order);
    RECOVERY_CHECK_EQ(after.accepted_count, before.accepted_count);
    RECOVERY_CHECK_EQ(after.reopen_lifecycle, before.reopen_lifecycle);
}

RECOVERY_TEST(commit_reopen_wrong_phase_is_rejected_without_mutation) {
    using namespace recovery;
    auto flow = flow_at_committing();
    const auto before = atomic_state(*flow.coordinator);

    check_error(flow.coordinator->commit_reopen(), CommitErrorCode::WRONG_PHASE);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(R87_duplicate_commit_reopen_does_not_advance_again) {
    using namespace recovery;
    auto flow = flow_at_committing();
    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(
        flow.coordinator->commit_reopen()));
    const auto before = atomic_state(*flow.coordinator);

    check_error(flow.coordinator->commit_reopen(), CommitErrorCode::WRONG_PHASE);
    RECOVERY_CHECK_EQ(atomic_state(*flow.coordinator), before);
}

RECOVERY_TEST(reopen_generation_overflow_is_explicit_and_never_wraps) {
    using namespace recovery;
    auto flow = flow_at_committing();
    RECOVERY_CHECK_EQ(flow.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    EpochRuntimeTestAccess::set_closing_generation_to_max(*flow.coordinator);
    const auto before = atomic_state(*flow.coordinator);

    check_error(flow.coordinator->commit_reopen(),
                CommitErrorCode::EPOCH_OVERFLOW);
    RECOVERY_CHECK_EQ(flow.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    const auto after = atomic_state(*flow.coordinator);
    RECOVERY_CHECK_EQ(after.generation,
                      std::numeric_limits<AdmissionGeneration>::max());
    RECOVERY_CHECK_EQ(after.admission_open, before.admission_open);
    RECOVERY_CHECK_EQ(after.receipt_slots, before.receipt_slots);
    RECOVERY_CHECK_EQ(after.accepted_count, before.accepted_count);
    RECOVERY_CHECK_EQ(after.reopen_lifecycle, before.reopen_lifecycle);
}
