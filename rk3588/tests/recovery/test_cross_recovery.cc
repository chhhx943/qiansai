#include "cross_recovery_test_access.hpp"
#include "task7_flow_fixture.hpp"

#include <algorithm>
#include <optional>
#include <variant>

namespace {

const recovery::CrossRecoveryAuthorizationSnapshot& ordinary_authorization(
    const recovery::CrossRecoveryRuntimeSnapshot& snapshot,
    recovery::AuthorizationLifecycle lifecycle) {
    const auto found = std::find_if(
        snapshot.authorizations.begin(), snapshot.authorizations.end(),
        [lifecycle](const auto& authorization) {
            return authorization.operation != recovery::OperationKind::COMMIT_EPOCH
                && authorization.operation
                    != recovery::OperationKind::REOPEN_ADMISSION
                && authorization.lifecycle == lifecycle;
        });
    RECOVERY_CHECK(found != snapshot.authorizations.end());
    return *found;
}

const recovery::CrossRecoveryRequestSnapshot& request_snapshot(
    const recovery::CrossRecoveryRuntimeSnapshot& snapshot,
    recovery::DeliveryRequestId request) {
    const auto found = std::find_if(
        snapshot.requests.begin(), snapshot.requests.end(),
        [request](const auto& candidate) { return candidate.request == request; });
    RECOVERY_CHECK(found != snapshot.requests.end());
    return *found;
}

}  // namespace

RECOVERY_TEST(R30_R70_R75_R81_R88_R89_R90_R92_R101_R102_cross_recovery_isolation) {
    using namespace recovery;
    constexpr DeliveryRequestId retained_request = 1;
    RecoveryCoordinator coordinator{11};
    auto definition = test::packet_flow_definition();
    const std::vector<FrozenGrantRecord> first_ledger{{
        1, std::nullopt, {}, {}, {11, 0, 1}, 55, 99}};
    auto first_executor = test::begin_flow_for(
        coordinator, definition.faults, definition.policy_contents,
        definition.bindings, std::nullopt, first_ledger);
    std::optional<AuthorizationLease> old_lease;
    test::complete_ordinary_actions_to_commit_deliverable(
        coordinator, first_executor, &old_lease);
    RECOVERY_CHECK(old_lease.has_value());
    RECOVERY_CHECK(old_lease->handle.valid());

    const auto first = CrossRecoveryTestAccess::snapshot(coordinator);
    RECOVERY_CHECK(first.has_value());
    const auto first_action = old_lease->action_id;
    const auto first_authorization = std::find_if(
        first->authorizations.begin(), first->authorizations.end(),
        [first_action](const auto& authorization) {
            return authorization.action_id == first_action;
        });
    RECOVERY_CHECK(first_authorization != first->authorizations.end());
    RECOVERY_CHECK_EQ(first_authorization->lifecycle,
                      AuthorizationLifecycle::CONSUMED);
    const auto& first_request = request_snapshot(*first, retained_request);
    RECOVERY_CHECK_EQ(first_request.action_id, first_action);
    RECOVERY_CHECK_EQ(first_request.delivery_id, old_lease->delivery_id);
    RECOVERY_CHECK_EQ(first_request.lifecycle, AuthorizationLifecycle::CONSUMED);
    RECOVERY_CHECK(first->receipt_slots.at(first_action).has_value());
    RECOVERY_CHECK_EQ(first->owner_acks.size(), 1U);
    RECOVERY_CHECK_EQ(first->owner_acks.front().owner, 55U);
    RECOVERY_CHECK_EQ(first->owner_acks.front().recovery_id, first->recovery_id);
    const auto first_receipt = *first->receipt_slots.at(first_action);
    const auto first_issue_nonce = first_authorization->issue_nonce;

    RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(
        coordinator.commit_epoch()));
    RECOVERY_CHECK_EQ(coordinator.advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    const auto first_final = CrossRecoveryTestAccess::snapshot(coordinator);
    RECOVERY_CHECK(first_final.has_value());
    const auto reopen = std::find_if(
        first_final->authorizations.begin(),
        first_final->authorizations.end(), [](const auto& authorization) {
            return authorization.operation
                == OperationKind::REOPEN_ADMISSION;
        });
    RECOVERY_CHECK(reopen != first_final->authorizations.end());
    RECOVERY_CHECK_EQ(reopen->lifecycle, AuthorizationLifecycle::DELIVERABLE);
    RECOVERY_CHECK_EQ(reopen->issue_nonce,
                      first_final->issue_nonce_counter);
    RECOVERY_CHECK(std::holds_alternative<CommitSuccess>(
        coordinator.commit_reopen()));
    RECOVERY_CHECK_EQ(coordinator.state(), RecoverySlotState::IDLE);
    RECOVERY_CHECK(!coordinator.has_active());
    RECOVERY_CHECK(!CrossRecoveryTestAccess::snapshot(coordinator).has_value());

    definition = test::packet_flow_definition();
    const std::vector<FrozenGrantRecord> second_ledger{{
        1, std::nullopt, {}, {}, {11, 1, 2}, 66, 100}};
    std::optional<CrossRecoveryRuntimeSnapshot> second_initial;
    auto second_executor = test::begin_flow_for(
        coordinator, definition.faults, definition.policy_contents,
        definition.bindings, std::nullopt, second_ledger, [&] {
            second_initial = CrossRecoveryTestAccess::snapshot(coordinator);
        });
    RECOVERY_CHECK(second_initial.has_value());
    RECOVERY_CHECK_EQ(second_initial->state, RecoverySlotState::PREPARING);
    RECOVERY_CHECK(second_initial->recovery_id != first->recovery_id);
    RECOVERY_CHECK_EQ(second_initial->epochs, first_final->epochs);
    RECOVERY_CHECK(second_initial->closing_generation
                   > first_final->closing_generation);
    RECOVERY_CHECK_EQ(second_initial->issue_nonce_counter,
                      first_final->issue_nonce_counter);
    RECOVERY_CHECK(second_initial->fault_seq_counter
                   > first_final->fault_seq_counter);
    RECOVERY_CHECK(second_initial->ledger_prefix.empty());
    RECOVERY_CHECK(second_initial->owner_acks.empty());
    RECOVERY_CHECK_EQ(second_initial->fault_batch.size(), 1U);
    RECOVERY_CHECK(second_initial->frozen_fault_batch.empty());
    RECOVERY_CHECK(second_initial->receipt_slots.empty());
    RECOVERY_CHECK(second_initial->accepted_order.empty());
    RECOVERY_CHECK_EQ(second_initial->accepted_count, 0U);
    RECOVERY_CHECK(second_initial->requests.empty());
    RECOVERY_CHECK(second_initial->authorizations.empty());
    RECOVERY_CHECK(!second_initial->has_current_snapshot);
    RECOVERY_CHECK(!second_initial->has_compiled_program);
    RECOVERY_CHECK(!second_initial->has_current_program);
    RECOVERY_CHECK(!second_initial->has_proofs);
    RECOVERY_CHECK(!second_initial->has_barrier);
    RECOVERY_CHECK_EQ(coordinator.advance_authorizations().code,
                      AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    const auto second = CrossRecoveryTestAccess::snapshot(coordinator);
    RECOVERY_CHECK(second.has_value());
    const auto& second_authorization = ordinary_authorization(
        *second, AuthorizationLifecycle::DELIVERABLE);
    RECOVERY_CHECK_EQ(second->authorizations.size(), 1U);

    RECOVERY_CHECK(second->recovery_id != first->recovery_id);
    RECOVERY_CHECK_EQ(second->recovery_id.boot_id,
                      first->recovery_id.boot_id);
    RECOVERY_CHECK(second->recovery_id.recovery_seq
                   > first->recovery_id.recovery_seq);
    RECOVERY_CHECK(second->transaction_hash != first->transaction_hash);
    RECOVERY_CHECK_EQ(second_authorization.action_id, first_action);
    RECOVERY_CHECK(second_authorization.issue_nonce > first_issue_nonce);
    RECOVERY_CHECK(second_authorization.issue_nonce
                   > first_final->issue_nonce_counter);
    RECOVERY_CHECK(second->issue_nonce_counter
                   > first_final->issue_nonce_counter);
    RECOVERY_CHECK(second->closing_generation > first->closing_generation);
    RECOVERY_CHECK(second->freeze_seq > first->freeze_seq);
    RECOVERY_CHECK(second->fault_seq_counter > first->fault_seq_counter);
    RECOVERY_CHECK_EQ(second->epochs, first_final->epochs);
    RECOVERY_CHECK(second_authorization.binding_is_current);
    RECOVERY_CHECK_EQ(second->ledger_prefix, second_ledger);
    RECOVERY_CHECK(second->ledger_prefix != first->ledger_prefix);
    RECOVERY_CHECK_EQ(second->accepted_count, 0U);
    RECOVERY_CHECK(std::all_of(
        second->receipt_slots.begin(), second->receipt_slots.end(),
        [](const auto& receipt) { return !receipt.has_value(); }));
    RECOVERY_CHECK(second->requests.empty());
    RECOVERY_CHECK_EQ(second->owner_acks.size(), 1U);
    RECOVERY_CHECK_EQ(second->owner_acks.front().owner, 66U);
    RECOVERY_CHECK_EQ(second->owner_acks.front().recovery_id,
                      second->recovery_id);
    RECOVERY_CHECK(second->owner_acks.front().recovery_id
                   != first->owner_acks.front().recovery_id);
    RECOVERY_CHECK_EQ(second->fault_batch.size(), 1U);
    RECOVERY_CHECK_EQ(second->fault_batch.front().kind,
                      FaultKind::PACKET_DOMAIN_FAULT);
    RECOVERY_CHECK_EQ(second->frozen_fault_batch, second->fault_batch);
    RECOVERY_CHECK(second->fault_batch.front().id
                   != first->fault_batch.front().id);
    RECOVERY_CHECK(second->frozen_fault_batch.front().id
                   != first->frozen_fault_batch.front().id);
    RECOVERY_CHECK(std::none_of(
        second->receipt_slots.begin(), second->receipt_slots.end(),
        [&first_receipt](const auto& receipt) {
            return receipt && receipt->recovery_id == first_receipt.recovery_id;
        }));

    auto second_receipt = test::successful_receipt_for(
        coordinator, second_authorization.action_id);
    const auto before_stale_submit =
        CrossRecoveryTestAccess::snapshot(coordinator);
    RECOVERY_CHECK_EQ(
        coordinator.submit_action_receipt(
            second_receipt, std::move(old_lease->handle)).status,
        ReceiptSubmitResult::REJECT);
    RECOVERY_CHECK(old_lease->handle.valid());
    const auto after_stale_submit = CrossRecoveryTestAccess::snapshot(coordinator);
    RECOVERY_CHECK(before_stale_submit.has_value());
    RECOVERY_CHECK(after_stale_submit.has_value());
    RECOVERY_CHECK_EQ(after_stale_submit, before_stale_submit);
    RECOVERY_CHECK_EQ(
        ordinary_authorization(*after_stale_submit,
                               AuthorizationLifecycle::DELIVERABLE).issue_nonce,
        second_authorization.issue_nonce);

    auto second_claim =
        coordinator.claim_authorization(second_executor, retained_request);
    RECOVERY_CHECK_EQ(second_claim.code, ClaimCode::CLAIMED);
    RECOVERY_CHECK(second_claim.lease.has_value());
    RECOVERY_CHECK(second_claim.lease->delivery_id != old_lease->delivery_id);
    const auto claimed = CrossRecoveryTestAccess::snapshot(coordinator);
    RECOVERY_CHECK_EQ(request_snapshot(*claimed, retained_request).delivery_id,
                      second_claim.lease->delivery_id);
    RECOVERY_CHECK_EQ(
        coordinator.submit_action_receipt(
            second_receipt, std::move(second_claim.lease->handle)).status,
        ReceiptSubmitResult::ACCEPTED);
    const auto accepted = CrossRecoveryTestAccess::snapshot(coordinator);
    RECOVERY_CHECK_EQ(accepted->accepted_count, 1U);
    RECOVERY_CHECK_EQ(request_snapshot(*accepted, retained_request).lifecycle,
                      AuthorizationLifecycle::CONSUMED);
}
