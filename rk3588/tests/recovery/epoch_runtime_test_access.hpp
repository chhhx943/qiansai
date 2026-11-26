#ifndef RECOVERY_EPOCH_RUNTIME_TEST_ACCESS_HPP
#define RECOVERY_EPOCH_RUNTIME_TEST_ACCESS_HPP

#include "recovery/runtime.hpp"

#include <algorithm>
#include <limits>

namespace recovery {

class EpochRuntimeTestAccess {
public:
    static ActionId commit_action_id(const RecoveryCoordinator& coordinator) {
        const auto* node =
            coordinator.find_unique_global_locked(OperationKind::COMMIT_EPOCH);
        return node->id;
    }

    static void set_committed_epochs(RecoveryCoordinator& coordinator,
                                     EpochSnapshot epochs) {
        coordinator.slot_.epochs = epochs;
    }

    static void set_snapshot_epochs(RecoveryCoordinator& coordinator,
                                    EpochSnapshot epochs) {
        coordinator.slot_.active->current_snapshot->epoch_snapshot = epochs;
    }

    static void set_compile_plan_old_epochs(CompileResult& result,
                                            std::size_t index,
                                            EpochSnapshot epochs) {
        auto& program = std::get<ValidatedRecoveryProgram>(result);
        program.resource_plans_.at(index).old_epochs = epochs;
        program.transaction_hash_ = compute_transaction_hash(program);
    }

    static void erase_commit_entry(RecoveryCoordinator& coordinator) {
        const auto id = commit_action_id(coordinator);
        coordinator.slot_.active->journal.entries_.at(id).reset();
    }

    static void set_commit_lifecycle(RecoveryCoordinator& coordinator,
                                     AuthorizationLifecycle lifecycle) {
        const auto id = commit_action_id(coordinator);
        coordinator.slot_.active->journal.entries_.at(id)->lifecycle = lifecycle;
    }

    static ActionId reopen_action_id(const RecoveryCoordinator& coordinator) {
        const auto* node = coordinator.find_unique_global_locked(
            OperationKind::REOPEN_ADMISSION);
        return node->id;
    }

    static const ActionNode& reopen_node(
        const RecoveryCoordinator& coordinator) {
        return *coordinator.find_unique_global_locked(
            OperationKind::REOPEN_ADMISSION);
    }

    static void erase_commit_receipt(RecoveryCoordinator& coordinator) {
        const auto id = commit_action_id(coordinator);
        coordinator.slot_.active->receipts.slots.at(id).reset();
    }

    static void corrupt_commit_receipt_binding(
        RecoveryCoordinator& coordinator) {
        const auto id = commit_action_id(coordinator);
        auto& receipt = *coordinator.slot_.active->receipts.slots.at(id);
        ++receipt.recovery_id.recovery_seq;
        receipt.receipt_hash = compute_receipt_hash(receipt);
    }

    static void duplicate_commit_in_accepted_order(
        RecoveryCoordinator& coordinator) {
        auto& receipts = coordinator.slot_.active->receipts;
        const auto id = commit_action_id(coordinator);
        receipts.accepted_order.at(0) = id;
    }

    static void hide_commit_from_accepted_count(
        RecoveryCoordinator& coordinator) {
        --coordinator.slot_.active->receipts.accepted_count;
    }

    static void corrupt_commit_entry_binding(RecoveryCoordinator& coordinator) {
        const auto id = commit_action_id(coordinator);
        ++coordinator.slot_.active->journal.entries_.at(id)->recovery_id.recovery_seq;
    }

    static void corrupt_local_complete_receipt_binding(
        RecoveryCoordinator& coordinator) {
        const auto& dag = coordinator.slot_.active->current_program->action_dag();
        const auto found = std::find_if(
            dag.begin(), dag.end(), [](const ActionNode& node) {
                return node.phase == ActionPhase::LOCAL_COMPLETE;
            });
        auto& receipt =
            *coordinator.slot_.active->receipts.slots.at(found->id);
        ++receipt.recovery_id.recovery_seq;
        receipt.receipt_hash = compute_receipt_hash(receipt);
    }

    static void erase_local_complete_receipt(
        RecoveryCoordinator& coordinator) {
        const auto& dag = coordinator.slot_.active->current_program->action_dag();
        const auto found = std::find_if(
            dag.begin(), dag.end(), [](const ActionNode& node) {
                return node.phase == ActionPhase::LOCAL_COMPLETE;
            });
        coordinator.slot_.active->receipts.slots.at(found->id).reset();
    }

    static void append_unbacked_commit_to_accepted_order(
        RecoveryCoordinator& coordinator) {
        auto& receipts = coordinator.slot_.active->receipts;
        receipts.accepted_order.at(receipts.accepted_count++) =
            commit_action_id(coordinator);
    }

    static void set_accepted_epoch(RecoveryCoordinator& coordinator,
                                   EpochSnapshot epochs) {
        coordinator.slot_.epochs = epochs;
    }

    static void remove_reopen_node(RecoveryCoordinator& coordinator) {
        auto program = mutable_program_copy(coordinator);
        std::erase_if(program->action_dag_, [](const ActionNode& node) {
            return node.operation == OperationKind::REOPEN_ADMISSION;
        });
        program->transaction_hash_ = compute_transaction_hash(*program);
        coordinator.slot_.active->current_program = std::move(program);
    }

    static void duplicate_reopen_node(RecoveryCoordinator& coordinator) {
        auto program = mutable_program_copy(coordinator);
        const auto found = std::find_if(
            program->action_dag_.begin(), program->action_dag_.end(),
            [](const ActionNode& node) {
                return node.operation == OperationKind::REOPEN_ADMISSION;
            });
        auto duplicate = *found;
        program->action_dag_.push_back(std::move(duplicate));
        program->transaction_hash_ = compute_transaction_hash(*program);
        coordinator.slot_.active->current_program = std::move(program);
    }

    static void clear_reopen_node_prerequisites(
        RecoveryCoordinator& coordinator) {
        auto program = mutable_program_copy(coordinator);
        auto found = std::find_if(
            program->action_dag_.begin(), program->action_dag_.end(),
            [](const ActionNode& node) {
                return node.operation == OperationKind::REOPEN_ADMISSION;
            });
        found->prerequisites.clear();
        program->transaction_hash_ = compute_transaction_hash(*program);
        coordinator.slot_.active->current_program = std::move(program);
    }

    static void erase_reopen_entry(RecoveryCoordinator& coordinator) {
        const auto id = reopen_action_id(coordinator);
        coordinator.slot_.active->journal.entries_.at(id).reset();
    }

    static void set_reopen_lifecycle(RecoveryCoordinator& coordinator,
                                     AuthorizationLifecycle lifecycle) {
        const auto id = reopen_action_id(coordinator);
        coordinator.slot_.active->journal.entries_.at(id)->lifecycle = lifecycle;
    }

    static void clear_reopen_entry_prerequisites(
        RecoveryCoordinator& coordinator) {
        const auto id = reopen_action_id(coordinator);
        coordinator.slot_.active->journal.entries_.at(id)
            ->prerequisite_receipts.clear();
    }

    static void set_closing_generation_to_max(
        RecoveryCoordinator& coordinator) {
        coordinator.slot_.closing_generation =
            std::numeric_limits<AdmissionGeneration>::max();
    }

    static std::uint64_t issue_nonce_counter(
        const RecoveryCoordinator& coordinator) {
        return coordinator.slot_.issue_nonce_counter;
    }

private:
    static std::shared_ptr<ValidatedRecoveryProgram> mutable_program_copy(
        const RecoveryCoordinator& coordinator) {
        const auto& source = *coordinator.slot_.active->current_program;
        return std::shared_ptr<ValidatedRecoveryProgram>{
            new ValidatedRecoveryProgram{
                source.recovery_id_, source.freeze_seq_, source.boot_id_,
                source.snapshot_digest_, source.policy_digest_,
                source.registry_version_, source.registry_digest_,
                source.faults_, source.resource_plans_, source.fault_coverage_,
                source.execution_predicates_, source.device_predicates_,
                source.capability_expectations_, source.action_dag_}};
    }
};

}  // namespace recovery

#endif
