#include "recovery/authorization.hpp"
#include "recovery/runtime.hpp"
#include "test_support.hpp"

#include <memory>
#include <new>

namespace recovery {
class AuthorizationRuntimeTestAccess {
public:
    static void set_issue_nonce(RecoveryCoordinator& coordinator,
                                std::uint64_t value) {
        coordinator.slot_.issue_nonce_counter = value;
    }
    static std::uint64_t issue_nonce(const RecoveryCoordinator& coordinator) {
        return coordinator.slot_.issue_nonce_counter;
    }
};
}

namespace {

struct ThrowAt final : recovery::RuntimeFailureProvider {
    recovery::RuntimeFailureStage target;
    explicit ThrowAt(recovery::RuntimeFailureStage value) : target(value) {}
    void checkpoint(recovery::RuntimeFailureStage value) override {
        if (value == target) throw std::bad_alloc{};
    }
};

struct ExecutingInputs {
    std::unique_ptr<recovery::RecoveryCoordinator> coordinator;
    recovery::PolicySnapshot policy;
    recovery::CapabilityRegistrySnapshot registry;
};

ExecutingInputs executing_coordinator(
    recovery::RuntimeFailureProvider* failure = nullptr) {
    using namespace recovery;
    ExecutingInputs in{failure
            ? std::make_unique<RecoveryCoordinator>(11, nullptr, failure)
            : std::make_unique<RecoveryCoordinator>(11), {}, {}};
    const FaultReport fault{FaultKind::PACKET_DOMAIN_FAULT,
                            {1, std::nullopt, std::nullopt},
                            {1, 2, 3, 4}, {1, {}}};
    auto policy = std::make_shared<PolicyContents>();
    policy->dispositions[FaultKind::PACKET_DOMAIN_FAULT] = FaultDisposition::COVERED;
    policy->recipes[{ResourceKind::PACKET_DOMAIN, RecoveryMode::PACKET_RESET}] =
        {ResourceKind::PACKET_DOMAIN, RecoveryMode::PACKET_RESET,
         {{ActionPhase::TEARDOWN, OperationKind::PACKET_STOP, {}},
          {ActionPhase::INVALIDATE_EPOCH, OperationKind::INVALIDATE_EPOCH, {0}},
          {ActionPhase::REINIT, OperationKind::REINIT, {1}}}};
    auto registry = std::make_shared<CapabilityRegistryContents>();
    in.policy = {2, canonical_digest(*policy), policy};
    in.registry = {3, canonical_digest(*registry), registry};

    RECOVERY_CHECK(in.coordinator->report_fault(fault));
    RECOVERY_CHECK(in.coordinator->begin_freeze(
        {}, {{1, ExecutorBindingStatus::RESOLVED,
              ExecutorPrincipal{ProcessPrincipal{{99, 100}}}, 1}}));
    RECOVERY_CHECK(in.coordinator->commit_freeze());
    RECOVERY_CHECK(in.coordinator->begin_compile(in.policy, in.registry));
    auto result = RecoveryCompiler::compile(
        *in.coordinator->active().current_snapshot, in.policy, in.registry, 0);
    in.coordinator->finish_compile(std::move(result));
    RECOVERY_CHECK(in.coordinator->seal_validated_program(
        in.coordinator->active().compiled_program));
    in.coordinator->begin_quiesce();
    in.coordinator->commit_barrier();
    RECOVERY_CHECK_EQ(in.coordinator->state(), RecoverySlotState::EXECUTING);
    return in;
}

}  // namespace

RECOVERY_TEST(R26_authorization_advance_installs_each_root_once) {
    using namespace recovery;
    auto in = executing_coordinator();
    const auto& dag = in.coordinator->active().current_program->action_dag();
    const auto root = dag.front().id;

    const auto first = in.coordinator->advance_authorizations();
    RECOVERY_CHECK_EQ(first.code, AuthorizationAdvanceCode::CREATED_DELIVERABLE);
    const auto* entry = in.coordinator->active().journal.find_entry(root);
    RECOVERY_CHECK(entry != nullptr);
    RECOVERY_CHECK_EQ(entry->lifecycle, AuthorizationLifecycle::DELIVERABLE);
    const auto first_nonce = entry->issue_nonce;
    RECOVERY_CHECK(first_nonce != 0);

    const auto second = in.coordinator->advance_authorizations();
    RECOVERY_CHECK_EQ(second.code, AuthorizationAdvanceCode::ALREADY_DELIVERABLE);
    entry = in.coordinator->active().journal.find_entry(root);
    RECOVERY_CHECK(entry != nullptr);
    RECOVERY_CHECK_EQ(entry->issue_nonce, first_nonce);
    for (std::size_t index = 1; index < dag.size(); ++index)
        RECOVERY_CHECK(in.coordinator->active().journal.find_entry(
                           dag[index].id) == nullptr);
}

RECOVERY_TEST(R91_nonce_exhaustion_fails_closed_without_journal_write) {
    using namespace recovery;
    auto in = executing_coordinator();
    AuthorizationRuntimeTestAccess::set_issue_nonce(
        *in.coordinator, std::numeric_limits<std::uint64_t>::max());
    RECOVERY_CHECK_EQ(in.coordinator->advance_authorizations().code,
                      AuthorizationAdvanceCode::INTERNAL_FAILURE);
    RECOVERY_CHECK_EQ(in.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
    RECOVERY_CHECK(in.coordinator->active().journal.find_entry(0) == nullptr);
    RECOVERY_CHECK_EQ(AuthorizationRuntimeTestAccess::issue_nonce(*in.coordinator),
                      std::numeric_limits<std::uint64_t>::max());
}

RECOVERY_TEST(R108_authorization_allocation_failures_leave_journal_unchanged) {
    using namespace recovery;
    for (const auto stage : {RuntimeFailureStage::AUTH_ELIGIBILITY,
                             RuntimeFailureStage::AUTH_PREPARE}) {
        ThrowAt failure{stage};
        auto in = executing_coordinator(&failure);
        RECOVERY_CHECK_EQ(in.coordinator->advance_authorizations().code,
                          AuthorizationAdvanceCode::INTERNAL_FAILURE);
        RECOVERY_CHECK_EQ(in.coordinator->state(), RecoverySlotState::FAIL_CLOSED);
        RECOVERY_CHECK(in.coordinator->active().journal.find_entry(0) == nullptr);
        RECOVERY_CHECK_EQ(AuthorizationRuntimeTestAccess::issue_nonce(*in.coordinator),
                          0U);
    }
}
