#ifndef RECOVERY_TASK7_FLOW_FIXTURE_HPP
#define RECOVERY_TASK7_FLOW_FIXTURE_HPP

#include "auth_test_access.hpp"
#include "epoch_runtime_test_access.hpp"
#include "recovery/authorization.hpp"
#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <functional>
#include <memory>

namespace recovery::test {

struct EpochCommitFlow {
    std::unique_ptr<RecoveryCoordinator> coordinator;
    AuthenticatedExecutorContext executor;
};

inline ActionResult successful_result_for(const ActionNode& node) {
    switch (node.operation) {
    case OperationKind::RGA_STOP: return RgaStopResult{0};
    case OperationKind::MPP_STOP: return MppStopResult{0};
    case OperationKind::PACKET_STOP: return PacketStopResult{true};
    case OperationKind::INVALIDATE_EPOCH: return EpochInvalidateResult{true};
    case OperationKind::REINIT: return ReinitResult{true};
    case OperationKind::LOCAL_COMPLETE: return LocalCompleteResult{true};
    default:
        RECOVERY_CHECK(false);
        return PacketStopResult{false};
    }
}

inline ActionReceipt successful_receipt_for(
    const RecoveryCoordinator& coordinator, ActionId action_id) {
    const auto& runtime = coordinator.active();
    const auto& dag = runtime.current_program->action_dag();
    const auto node = std::find_if(dag.begin(), dag.end(), [&](const auto& candidate) {
        return candidate.id == action_id;
    });
    RECOVERY_CHECK(node != dag.end());
    const auto* entry = runtime.journal.find_entry(node->id);
    RECOVERY_CHECK(entry != nullptr);
    ActionReceipt receipt{
        node->id, runtime.recovery_id, runtime.transaction_hash, node->id,
        node->operation, node->scope, node->old_epochs, node->new_epochs,
        ActionOutcome::SUCCEEDED, successful_result_for(*node),
        entry->prerequisite_receipts, {}};
    receipt.receipt_hash = compute_receipt_hash(receipt);
    return receipt;
}

inline ActionReceipt successful_receipt_for(
    const RecoveryCoordinator& coordinator, const AuthorizationLease& lease) {
    return successful_receipt_for(coordinator, lease.action_id);
}

inline AuthenticatedExecutorContext begin_flow_for(
    RecoveryCoordinator& coordinator,
    const std::vector<FaultReport>& faults,
    const std::shared_ptr<PolicyContents>& policy_contents,
    const std::vector<OwnerExecutorBinding>& bindings,
    std::optional<std::pair<std::size_t, EpochSnapshot>> plan_epoch_override =
        std::nullopt,
    const std::vector<FrozenGrantRecord>& ledger = {},
    const std::function<void()>& after_faults = {}) {
    auto registry_contents = std::make_shared<CapabilityRegistryContents>();
    const PolicySnapshot policy{
        2, canonical_digest(*policy_contents), policy_contents};
    const CapabilityRegistrySnapshot registry{
        3, canonical_digest(*registry_contents), registry_contents};
    for (const auto& fault : faults) {
        RECOVERY_CHECK(coordinator.report_fault(fault));
    }
    if (after_faults) after_faults();
    RECOVERY_CHECK(coordinator.begin_freeze(ledger, bindings));
    std::set<OwnerId> owners;
    for (const auto& grant : ledger) owners.insert(grant.owner);
    for (const auto owner : owners) {
        std::set<GrantIdentity> grants;
        std::set<LeaseIdentity> leases;
        for (const auto& grant : ledger) {
            if (grant.owner != owner) continue;
            grants.insert(grant.identity);
            if (grant.lease) leases.insert(*grant.lease);
        }
        const auto& freeze = *coordinator.active().freeze_request;
        OwnerSnapshotAck ack{
            freeze.recovery_id, freeze.freeze_seq, freeze.boot_id,
            freeze.closing_generation, owner, std::move(grants),
            std::move(leases), {}};
        ack.local_view_digest = canonical_digest(ack);
        RECOVERY_CHECK(coordinator.submit_owner_snapshot_ack(ack));
    }
    RECOVERY_CHECK(coordinator.commit_freeze());
    RECOVERY_CHECK(coordinator.begin_compile(policy, registry));
    auto compiled = RecoveryCompiler::compile(
        *coordinator.active().current_snapshot, policy, registry, 0);
    if (plan_epoch_override) {
        EpochRuntimeTestAccess::set_compile_plan_old_epochs(
            compiled, plan_epoch_override->first, plan_epoch_override->second);
    }
    coordinator.finish_compile(std::move(compiled));
    RECOVERY_CHECK(coordinator.seal_validated_program(
        coordinator.active().compiled_program));
    coordinator.begin_quiesce();
    coordinator.commit_barrier();
    return AuthenticatedExecutorContextTestAccess::issue({{99, 100}});
}

inline void complete_ordinary_actions_to_commit_deliverable(
    RecoveryCoordinator& coordinator,
    const AuthenticatedExecutorContext& executor,
    std::optional<AuthorizationLease>* retained_redelivery = nullptr) {
    DeliveryRequestId request = 1;
    for (;;) {
        const auto advance = coordinator.advance_authorizations().code;
        RECOVERY_CHECK(
            advance == AuthorizationAdvanceCode::CREATED_DELIVERABLE
            || advance == AuthorizationAdvanceCode::ALREADY_DELIVERABLE);
        const auto& dag = coordinator.active().current_program->action_dag();
        const auto commit = std::find_if(dag.begin(), dag.end(), [](const auto& node) {
            return node.operation == OperationKind::COMMIT_EPOCH;
        });
        RECOVERY_CHECK(commit != dag.end());
        if (coordinator.active().journal.lifecycle_of(commit->id)
            == std::optional{AuthorizationLifecycle::DELIVERABLE}) {
            return;
        }

        const auto current_request = request++;
        auto claim = coordinator.claim_authorization(executor, current_request);
        RECOVERY_CHECK_EQ(claim.code, ClaimCode::CLAIMED);
        RECOVERY_CHECK(claim.lease.has_value());
        if (retained_redelivery && !retained_redelivery->has_value()) {
            auto redelivery =
                coordinator.claim_authorization(executor, current_request);
            RECOVERY_CHECK_EQ(redelivery.code, ClaimCode::REDELIVERED);
            RECOVERY_CHECK(redelivery.lease.has_value());
            retained_redelivery->emplace(std::move(*redelivery.lease));
        }
        auto receipt = successful_receipt_for(coordinator, *claim.lease);
        RECOVERY_CHECK_EQ(
            coordinator.submit_action_receipt(
                receipt, std::move(claim.lease->handle)).status,
            ReceiptSubmitResult::ACCEPTED);
    }
}

inline EpochCommitFlow flow_at_commit_deliverable_for(
    const std::vector<FaultReport>& faults,
    const std::shared_ptr<PolicyContents>& policy_contents,
    const std::vector<OwnerExecutorBinding>& bindings,
    RuntimeFailureProvider* failure_provider,
    std::optional<std::pair<std::size_t, EpochSnapshot>> plan_epoch_override =
        std::nullopt) {
    auto coordinator = failure_provider
        ? std::make_unique<RecoveryCoordinator>(11, nullptr, failure_provider)
        : std::make_unique<RecoveryCoordinator>(11);
    auto executor = begin_flow_for(
        *coordinator, faults, policy_contents, bindings, plan_epoch_override);
    complete_ordinary_actions_to_commit_deliverable(*coordinator, executor);

    return {std::move(coordinator), std::move(executor)};
}

struct PacketFlowDefinition {
    std::vector<FaultReport> faults;
    std::shared_ptr<PolicyContents> policy_contents;
    std::vector<OwnerExecutorBinding> bindings;
};

inline PacketFlowDefinition packet_flow_definition() {
    PacketFlowDefinition definition{
        {{FaultKind::PACKET_DOMAIN_FAULT,
          {1, std::nullopt, std::nullopt},
          {1, 2, 3, 4},
          {1, {}}}},
        std::make_shared<PolicyContents>(),
        {{1, ExecutorBindingStatus::RESOLVED,
          ExecutorPrincipal{ProcessPrincipal{{99, 100}}}, 1}}};
    definition.policy_contents->dispositions[
        FaultKind::PACKET_DOMAIN_FAULT] = FaultDisposition::COVERED;
    definition.policy_contents->recipes[
        {ResourceKind::PACKET_DOMAIN, RecoveryMode::PACKET_RESET}] = {
            ResourceKind::PACKET_DOMAIN,
            RecoveryMode::PACKET_RESET,
            {{ActionPhase::TEARDOWN, OperationKind::PACKET_STOP, {}},
             {ActionPhase::INVALIDATE_EPOCH,
              OperationKind::INVALIDATE_EPOCH, {0}},
             {ActionPhase::REINIT, OperationKind::REINIT, {1}}}};
    return definition;
}

inline EpochCommitFlow flow_at_commit_deliverable(
    RuntimeFailureProvider* failure_provider = nullptr) {
    auto definition = packet_flow_definition();
    return flow_at_commit_deliverable_for(
        definition.faults, definition.policy_contents, definition.bindings,
        failure_provider);
}

inline EpochCommitFlow media_flow_at_commit_deliverable(
    RuntimeFailureProvider* failure_provider = nullptr,
    bool inject_epoch_conflict = false) {
    const EpochSnapshot epochs{10, 20, 30, 40};
    const std::vector<FaultReport> faults{
        {FaultKind::RGA_DEVICE_FAULT,
         {1, std::nullopt, std::nullopt}, epochs, {1, {}}},
        {FaultKind::MPP_DEVICE_FAULT,
         {2, std::nullopt, std::nullopt}, epochs, {2, {}}}};
    auto policy_contents = std::make_shared<PolicyContents>();
    policy_contents->dispositions[FaultKind::RGA_DEVICE_FAULT] =
        FaultDisposition::COVERED;
    policy_contents->dispositions[FaultKind::MPP_DEVICE_FAULT] =
        FaultDisposition::COVERED;
    policy_contents->recipes[{ResourceKind::RGA, RecoveryMode::ENCODING_RESET}] = {
        ResourceKind::RGA,
        RecoveryMode::ENCODING_RESET,
        {{ActionPhase::TEARDOWN, OperationKind::RGA_STOP, {}},
         {ActionPhase::INVALIDATE_EPOCH,
          OperationKind::INVALIDATE_EPOCH, {0}},
         {ActionPhase::REINIT, OperationKind::REINIT, {1}}}};
    policy_contents->recipes[
        {ResourceKind::MPP_ENCODER, RecoveryMode::ENCODING_RESET}] = {
            ResourceKind::MPP_ENCODER,
            RecoveryMode::ENCODING_RESET,
            {{ActionPhase::TEARDOWN, OperationKind::MPP_STOP, {}},
             {ActionPhase::INVALIDATE_EPOCH,
              OperationKind::INVALIDATE_EPOCH, {0}},
             {ActionPhase::REINIT, OperationKind::REINIT, {1}}}};
    const std::vector<OwnerExecutorBinding> bindings{
        {1, ExecutorBindingStatus::RESOLVED,
         ExecutorPrincipal{ProcessPrincipal{{99, 100}}}, 1},
        {2, ExecutorBindingStatus::RESOLVED,
         ExecutorPrincipal{ProcessPrincipal{{99, 100}}}, 1}};
    auto conflict = epochs;
    ++conflict.media_epoch;
    const auto plan_override = inject_epoch_conflict
        ? std::optional{std::pair<std::size_t, EpochSnapshot>{1, conflict}}
        : std::nullopt;
    return flow_at_commit_deliverable_for(
        faults, policy_contents, bindings, failure_provider, plan_override);
}

}  // namespace recovery::test

#endif
