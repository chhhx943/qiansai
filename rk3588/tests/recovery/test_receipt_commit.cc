#include "auth_test_access.hpp"
#include "recovery/authorization.hpp"
#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <memory>
#include <new>

namespace {

struct ThrowAt final : recovery::RuntimeFailureProvider {
    recovery::RuntimeFailureStage target;
    bool enabled{true};
    explicit ThrowAt(recovery::RuntimeFailureStage value) : target(value) {}
    void checkpoint(recovery::RuntimeFailureStage value) override {
        if (enabled && value == target) throw std::bad_alloc{};
    }
};

struct ClaimedRoot {
    std::unique_ptr<recovery::RecoveryCoordinator> coordinator;
    recovery::AuthenticatedExecutorContext executor;
    recovery::ClaimResult claim;
};

ClaimedRoot claimed_root(recovery::RuntimeFailureProvider* failure = nullptr,
                         bool parallel = false) {
    using namespace recovery;
    auto coordinator = failure
        ? std::make_unique<RecoveryCoordinator>(11, nullptr, failure)
        : std::make_unique<RecoveryCoordinator>(11);
    const FaultReport fault{FaultKind::PACKET_DOMAIN_FAULT,
                            {1, std::nullopt, std::nullopt},
                            {1, 2, 3, 4}, {1, {}}};
    auto policy_contents = std::make_shared<PolicyContents>();
    policy_contents->dispositions[FaultKind::PACKET_DOMAIN_FAULT] =
        FaultDisposition::COVERED;
    policy_contents->recipes[{ResourceKind::PACKET_DOMAIN, RecoveryMode::PACKET_RESET}] =
        {ResourceKind::PACKET_DOMAIN, RecoveryMode::PACKET_RESET,
         {{ActionPhase::TEARDOWN, OperationKind::PACKET_STOP, {}},
          {ActionPhase::INVALIDATE_EPOCH, OperationKind::INVALIDATE_EPOCH, {0}},
          {ActionPhase::REINIT, OperationKind::REINIT, {1}}}};
    auto registry_contents = std::make_shared<CapabilityRegistryContents>();
    const PolicySnapshot policy{2, canonical_digest(*policy_contents), policy_contents};
    const CapabilityRegistrySnapshot registry{
        3, canonical_digest(*registry_contents), registry_contents};
    RECOVERY_CHECK(coordinator->report_fault(fault));
    if (parallel) {
        auto second = fault;
        second.subject.resource = 2;
        second.evidence.evidence_id = 2;
        second.evidence.evidence_digest[0] = 1;
        RECOVERY_CHECK(coordinator->report_fault(second));
    }
    std::vector<OwnerExecutorBinding> bindings{
        {1, ExecutorBindingStatus::RESOLVED,
         ExecutorPrincipal{ProcessPrincipal{{99, 100}}}, 1}};
    if (parallel)
        bindings.push_back(
            {2, ExecutorBindingStatus::RESOLVED,
             ExecutorPrincipal{ProcessPrincipal{{99, 100}}}, 1});
    RECOVERY_CHECK(coordinator->begin_freeze({}, bindings));
    RECOVERY_CHECK(coordinator->commit_freeze());
    RECOVERY_CHECK(coordinator->begin_compile(policy, registry));
    auto compiled = RecoveryCompiler::compile(
        *coordinator->active().current_snapshot, policy, registry, 0);
    coordinator->finish_compile(std::move(compiled));
    RECOVERY_CHECK(coordinator->seal_validated_program(
        coordinator->active().compiled_program));
    coordinator->begin_quiesce();
    coordinator->commit_barrier();
    RECOVERY_CHECK_EQ(coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    auto executor = AuthenticatedExecutorContextTestAccess::issue({{99, 100}});
    auto claim = coordinator->claim_authorization(executor, 7);
    RECOVERY_CHECK_EQ(claim.code, ClaimCode::CLAIMED);
    RECOVERY_CHECK(claim.lease.has_value());
    return {std::move(coordinator), std::move(executor), std::move(claim)};
}

recovery::ActionReceipt successful_root_receipt(const ClaimedRoot& in) {
    using namespace recovery;
    const auto& node = in.coordinator->active().current_program->action_dag()[
        in.claim.lease->action_id];
    ActionReceipt receipt{node.id, in.coordinator->active().recovery_id,
                          in.coordinator->active().transaction_hash, node.id,
                          node.operation, node.scope, node.old_epochs, node.new_epochs,
                          ActionOutcome::SUCCEEDED, PacketStopResult{true}, {}, {}};
    receipt.receipt_hash = compute_receipt_hash(receipt);
    return receipt;
}

recovery::ClaimResult accept_root_and_claim_next(ClaimedRoot& in) {
    using namespace recovery;
    auto root_receipt = successful_root_receipt(in);
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          root_receipt, std::move(in.claim.lease->handle)).status,
                      ReceiptSubmitResult::ACCEPTED);
    RECOVERY_CHECK_EQ(in.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    auto next = in.coordinator->claim_authorization(in.executor, 8);
    RECOVERY_CHECK_EQ(next.code, ClaimCode::CLAIMED);
    RECOVERY_CHECK(next.lease.has_value());
    return next;
}

recovery::ActionReceipt successful_receipt_for(
    const ClaimedRoot& in, const recovery::AuthorizationLease& lease) {
    using namespace recovery;
    const auto& node = in.coordinator->active().current_program->action_dag()[
        lease.action_id];
    const auto* entry = in.coordinator->active().journal.find_entry(node.id);
    RECOVERY_CHECK(entry != nullptr);
    ActionResult result = [&]() -> ActionResult {
        switch (node.operation) {
        case OperationKind::PACKET_STOP: return PacketStopResult{true};
        case OperationKind::INVALIDATE_EPOCH: return EpochInvalidateResult{true};
        case OperationKind::REINIT: return ReinitResult{true};
        case OperationKind::LOCAL_COMPLETE: return LocalCompleteResult{true};
        default: return PacketStopResult{false};
        }
    }();
    ActionReceipt receipt{node.id, in.coordinator->active().recovery_id,
                          in.coordinator->active().transaction_hash, node.id,
                          node.operation, node.scope, node.old_epochs, node.new_epochs,
                          ActionOutcome::SUCCEEDED, std::move(result),
                          entry->prerequisite_receipts, {}};
    receipt.receipt_hash = compute_receipt_hash(receipt);
    return receipt;
}

}  // namespace

RECOVERY_TEST(R106_consumed_request_is_permanent_and_does_not_claim_successor) {
    using namespace recovery;
    auto in = claimed_root();
    auto receipt = successful_root_receipt(in);
    const auto delivery = in.claim.lease->delivery_id;
    const auto action = in.claim.lease->action_id;

    const auto submitted = in.coordinator->submit_action_receipt(
        receipt, std::move(in.claim.lease->handle));
    RECOVERY_CHECK_EQ(submitted.status, ReceiptSubmitResult::ACCEPTED);
    RECOVERY_CHECK(!in.claim.lease->handle.valid());
    RECOVERY_CHECK_EQ(in.coordinator->active().journal.lifecycle_of(action),
                      std::optional{AuthorizationLifecycle::CONSUMED});
    RECOVERY_CHECK(in.coordinator->active().receipts.slots[action].has_value());
    RECOVERY_CHECK_EQ(in.coordinator->active().receipts.slots[action]->receipt_hash,
                      receipt.receipt_hash);

    RECOVERY_CHECK_EQ(in.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    RECOVERY_CHECK(in.coordinator->active().journal.find_entry(action + 1) != nullptr);
    const auto retry = in.coordinator->claim_authorization(in.executor, 7);
    RECOVERY_CHECK_EQ(retry.code, ClaimCode::ALREADY_CONSUMED);
    RECOVERY_CHECK_EQ(retry.delivery_id, std::optional{delivery});
    auto successor = in.coordinator->claim_authorization(in.executor, 8);
    RECOVERY_CHECK_EQ(successor.code, ClaimCode::CLAIMED);
    RECOVERY_CHECK_EQ(successor.lease->action_id, action + 1);
}

RECOVERY_TEST(R77_bad_receipt_hash_preserves_lease_for_valid_retry) {
    using namespace recovery;
    auto in = claimed_root();
    auto receipt = successful_root_receipt(in);
    const auto action = in.claim.lease->action_id;
    receipt.receipt_hash[0] ^= 1U;

    const auto rejected = in.coordinator->submit_action_receipt(
        receipt, std::move(in.claim.lease->handle));
    RECOVERY_CHECK_EQ(rejected.status, ReceiptSubmitResult::REJECT);
    RECOVERY_CHECK(in.claim.lease->handle.valid());
    RECOVERY_CHECK_EQ(in.coordinator->active().journal.lifecycle_of(action),
                      std::optional{AuthorizationLifecycle::CLAIMED});
    RECOVERY_CHECK(!in.coordinator->active().receipts.slots[action].has_value());

    receipt.receipt_hash = compute_receipt_hash(receipt);
    const auto accepted = in.coordinator->submit_action_receipt(
        receipt, std::move(in.claim.lease->handle));
    RECOVERY_CHECK_EQ(accepted.status, ReceiptSubmitResult::ACCEPTED);
}

RECOVERY_TEST(R29_generic_receipt_path_rejects_commit_and_reopen) {
    using namespace recovery;
    auto in = claimed_root();
    auto receipt = successful_root_receipt(in);
    const auto action = in.claim.lease->action_id;

    receipt.operation = OperationKind::COMMIT_EPOCH;
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(in.claim.lease->handle)).status,
                      ReceiptSubmitResult::REJECT);
    RECOVERY_CHECK(in.claim.lease->handle.valid());
    receipt.operation = OperationKind::REOPEN_ADMISSION;
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(in.claim.lease->handle)).status,
                      ReceiptSubmitResult::REJECT_USE_COMMIT_REOPEN);
    RECOVERY_CHECK(in.claim.lease->handle.valid());
    RECOVERY_CHECK_EQ(in.coordinator->active().journal.lifecycle_of(action),
                      std::optional{AuthorizationLifecycle::CLAIMED});
}

RECOVERY_TEST(R71_coordinator_only_actions_cannot_use_generic_delivery_paths) {
    using namespace recovery;
    auto in = claimed_root();
    std::optional<ClaimResult> current{std::move(in.claim)};
    for (DeliveryRequestId request = 8; request <= 11; ++request) {
        auto receipt = successful_receipt_for(in, *current->lease);
        RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                              receipt, std::move(current->lease->handle)).status,
                          ReceiptSubmitResult::ACCEPTED);
        RECOVERY_CHECK_EQ(in.coordinator->advance_authorizations().code,
                          AuthorizationAdvanceCode::CREATED_DELIVERABLE);
        if (request != 11) {
            current.emplace(
                in.coordinator->claim_authorization(in.executor, request));
            RECOVERY_CHECK_EQ(current->code, ClaimCode::CLAIMED);
        }
    }

    const auto& dag = in.coordinator->active().current_program->action_dag();
    const auto commit = std::find_if(dag.begin(), dag.end(), [](const auto& node) {
        return node.operation == OperationKind::COMMIT_EPOCH;
    });
    RECOVERY_CHECK(commit != dag.end());
    RECOVERY_CHECK_EQ(in.coordinator->active().journal.lifecycle_of(commit->id),
                      std::optional{AuthorizationLifecycle::DELIVERABLE});
    RECOVERY_CHECK_EQ(in.coordinator->claim_authorization(in.executor, 12).code,
                      ClaimCode::NONE_AVAILABLE);
}

RECOVERY_TEST(R109_receipt_validation_allocation_failure_has_zero_side_effects) {
    using namespace recovery;
    ThrowAt failure{RuntimeFailureStage::RECEIPT_VALIDATE_PREREQS};
    failure.enabled = false;
    auto in = claimed_root(&failure);
    auto next = accept_root_and_claim_next(in);
    auto receipt = successful_receipt_for(in, *next.lease);
    const auto action = next.lease->action_id;
    failure.enabled = true;
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(next.lease->handle)).status,
                      ReceiptSubmitResult::REJECT);
    RECOVERY_CHECK(next.lease->handle.valid());
    RECOVERY_CHECK_EQ(in.coordinator->state(), RecoverySlotState::EXECUTING);
    RECOVERY_CHECK_EQ(in.coordinator->active().journal.lifecycle_of(action),
                      std::optional{AuthorizationLifecycle::CLAIMED});
    RECOVERY_CHECK(!in.coordinator->active().receipts.slots[action].has_value());
    RECOVERY_CHECK_EQ(in.coordinator->active().receipts.accepted_count, 1U);
}

RECOVERY_TEST(R69_prerequisite_hash_mismatch_preserves_lease) {
    using namespace recovery;
    auto in = claimed_root();
    auto next = accept_root_and_claim_next(in);
    auto receipt = successful_receipt_for(in, *next.lease);
    const auto action = next.lease->action_id;
    receipt.prerequisite_receipts.clear();
    receipt.receipt_hash = compute_receipt_hash(receipt);
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(next.lease->handle)).status,
                      ReceiptSubmitResult::REJECT);
    RECOVERY_CHECK(next.lease->handle.valid());
    RECOVERY_CHECK_EQ(in.coordinator->active().journal.lifecycle_of(action),
                      std::optional{AuthorizationLifecycle::CLAIMED});
    RECOVERY_CHECK(!in.coordinator->active().receipts.slots[action].has_value());
    RECOVERY_CHECK_EQ(in.coordinator->active().receipts.accepted_count, 1U);
}

RECOVERY_TEST(R111_parallel_branch_remains_deliverable_after_peer_completes) {
    using namespace recovery;
    auto in = claimed_root(nullptr, true);
    auto receipt = successful_root_receipt(in);
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(in.claim.lease->handle)).status,
                      ReceiptSubmitResult::ACCEPTED);
    RECOVERY_CHECK_EQ(in.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::ALREADY_DELIVERABLE);

    auto peer = in.coordinator->claim_authorization(in.executor, 8);
    RECOVERY_CHECK_EQ(peer.code, ClaimCode::CLAIMED);
    RECOVERY_CHECK(peer.lease.has_value());
    RECOVERY_CHECK(peer.lease->action_id != receipt.action_id);
}

RECOVERY_TEST(R100_redelivered_lease_cannot_be_consumed_after_first_copy) {
    using namespace recovery;
    auto in = claimed_root();
    auto duplicate = in.coordinator->claim_authorization(in.executor, 7);
    RECOVERY_CHECK_EQ(duplicate.code, ClaimCode::REDELIVERED);
    auto receipt = successful_root_receipt(in);
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(in.claim.lease->handle)).status,
                      ReceiptSubmitResult::ACCEPTED);
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(duplicate.lease->handle)).status,
                      ReceiptSubmitResult::REJECT);
    RECOVERY_CHECK(duplicate.lease->handle.valid());
    RECOVERY_CHECK_EQ(in.coordinator->active().receipts.accepted_count, 1U);
}

RECOVERY_TEST(R86_receipt_prepare_allocation_failure_fails_closed_before_commit) {
    using namespace recovery;
    ThrowAt failure{RuntimeFailureStage::RECEIPT_PREPARE};
    auto in = claimed_root(&failure);
    auto receipt = successful_root_receipt(in);
    const auto action = in.claim.lease->action_id;
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(in.claim.lease->handle)).status,
                      ReceiptSubmitResult::REJECT);
    RECOVERY_CHECK(in.claim.lease->handle.valid());
    RECOVERY_CHECK_EQ(in.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK_EQ(in.coordinator->active().journal.lifecycle_of(action),
                      std::optional{AuthorizationLifecycle::CLAIMED});
    RECOVERY_CHECK(!in.coordinator->active().receipts.slots[action].has_value());
    RECOVERY_CHECK_EQ(in.coordinator->active().receipts.accepted_count, 0U);

    failure.enabled = false;
    RECOVERY_CHECK_EQ(in.coordinator->submit_action_receipt(
                          receipt, std::move(in.claim.lease->handle)).status,
                      ReceiptSubmitResult::REJECT);
    RECOVERY_CHECK(in.claim.lease->handle.valid());
    RECOVERY_CHECK_EQ(in.coordinator->claim_authorization(in.executor, 7).code,
                      ClaimCode::NONE_AVAILABLE);
    RECOVERY_CHECK_EQ(in.coordinator->active().journal.lifecycle_of(action),
                      std::optional{AuthorizationLifecycle::CLAIMED});
    RECOVERY_CHECK_EQ(in.coordinator->active().receipts.accepted_count, 0U);
}
