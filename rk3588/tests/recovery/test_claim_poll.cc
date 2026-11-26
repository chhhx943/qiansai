#include "auth_test_access.hpp"
#include "recovery/authorization.hpp"
#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <memory>
#include <new>

namespace {

struct ThrowAt final : recovery::RuntimeFailureProvider {
    recovery::RuntimeFailureStage target;
    explicit ThrowAt(recovery::RuntimeFailureStage value) : target(value) {}
    void checkpoint(recovery::RuntimeFailureStage value) override {
        if (value == target) throw std::bad_alloc{};
    }
};

std::unique_ptr<recovery::RecoveryCoordinator> coordinator_with_deliverable(
    recovery::RuntimeFailureProvider* failure = nullptr) {
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
    RECOVERY_CHECK(coordinator->begin_freeze(
        {}, {{1, ExecutorBindingStatus::RESOLVED,
              ExecutorPrincipal{ProcessPrincipal{{99, 100}}}, 1}}));
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
    return coordinator;
}

}  // namespace

RECOVERY_TEST(R105_unauthorized_executor_cannot_claim_or_mutate_entry) {
    using namespace recovery;
    auto coordinator = coordinator_with_deliverable();
    auto wrong = AuthenticatedExecutorContextTestAccess::issue({{55, 66}});
    const auto result = coordinator->claim_authorization(wrong, 1);
    RECOVERY_CHECK_EQ(result.code, ClaimCode::UNAUTHORIZED);
    const auto* entry = coordinator->active().journal.find_entry(0);
    RECOVERY_CHECK(entry != nullptr);
    RECOVERY_CHECK_EQ(entry->lifecycle, AuthorizationLifecycle::DELIVERABLE);
    RECOVERY_CHECK(!entry->claimed_by.has_value());
    RECOVERY_CHECK(coordinator->active().requests.by_request.empty());
}

RECOVERY_TEST(R98_R99_claim_retry_is_idempotent_and_other_request_cannot_reclaim) {
    using namespace recovery;
    auto coordinator = coordinator_with_deliverable();
    auto executor = AuthenticatedExecutorContextTestAccess::issue({{99, 100}});
    auto first = coordinator->claim_authorization(executor, 7);
    RECOVERY_CHECK_EQ(first.code, ClaimCode::CLAIMED);
    RECOVERY_CHECK(first.lease.has_value());
    RECOVERY_CHECK(first.lease->handle.valid());
    const auto delivery = first.lease->delivery_id;

    auto retry = coordinator->claim_authorization(executor, 7);
    RECOVERY_CHECK_EQ(retry.code, ClaimCode::REDELIVERED);
    RECOVERY_CHECK(retry.lease.has_value());
    RECOVERY_CHECK_EQ(retry.lease->delivery_id, delivery);
    RECOVERY_CHECK(retry.lease->handle.valid());

    const auto other_request = coordinator->claim_authorization(executor, 8);
    RECOVERY_CHECK_EQ(other_request.code, ClaimCode::NONE_AVAILABLE);
    const auto polled = coordinator->poll_authorizations(executor);
    RECOVERY_CHECK(std::holds_alternative<std::vector<LeaseDescriptor>>(polled));
    const auto& descriptors = std::get<std::vector<LeaseDescriptor>>(polled);
    RECOVERY_CHECK_EQ(descriptors.size(), 1U);
    RECOVERY_CHECK_EQ(descriptors.front().delivery_id, delivery);
    RECOVERY_CHECK_EQ(descriptors.front().lifecycle, AuthorizationLifecycle::CLAIMED);
}

RECOVERY_TEST(R97_claim_allocation_failure_preserves_deliverable) {
    using namespace recovery;
    ThrowAt failure{RuntimeFailureStage::CLAIM_REQUEST_INSERT};
    auto coordinator = coordinator_with_deliverable(&failure);
    auto executor = AuthenticatedExecutorContextTestAccess::issue({{99, 100}});
    const auto result = coordinator->claim_authorization(executor, 9);
    RECOVERY_CHECK_EQ(result.code, ClaimCode::NONE_AVAILABLE);
    RECOVERY_CHECK(coordinator->active().requests.by_request.empty());
    RECOVERY_CHECK_EQ(coordinator->active().journal.lifecycle_of(0),
                      std::optional{AuthorizationLifecycle::DELIVERABLE});
}

RECOVERY_TEST(R110_poll_allocation_failure_preserves_claim_state) {
    using namespace recovery;
    ThrowAt failure{RuntimeFailureStage::POLL_COLLECT};
    auto coordinator = coordinator_with_deliverable(&failure);
    auto executor = AuthenticatedExecutorContextTestAccess::issue({{99, 100}});
    const auto claimed = coordinator->claim_authorization(executor, 10);
    RECOVERY_CHECK_EQ(claimed.code, ClaimCode::CLAIMED);
    const auto polled = coordinator->poll_authorizations(executor);
    RECOVERY_CHECK(std::holds_alternative<PollError>(polled));
    RECOVERY_CHECK_EQ(std::get<PollError>(polled).code,
                      PollErrorCode::ALLOCATION_FAILED);
    RECOVERY_CHECK_EQ(coordinator->active().requests.by_request.size(), 1U);
    RECOVERY_CHECK_EQ(coordinator->active().journal.lifecycle_of(0),
                      std::optional{AuthorizationLifecycle::CLAIMED});
}
