#ifndef RECOVERY_CROSS_RECOVERY_TEST_ACCESS_HPP
#define RECOVERY_CROSS_RECOVERY_TEST_ACCESS_HPP

#include "recovery/runtime.hpp"

#include <optional>
#include <vector>

namespace recovery {

struct CrossRecoveryAuthorizationSnapshot {
    ActionId action_id;
    OperationKind operation;
    std::uint64_t issue_nonce;
    AuthorizationLifecycle lifecycle;
    std::optional<DeliveryRequestId> delivery_request;
    bool binding_is_current;
    auto operator<=>(const CrossRecoveryAuthorizationSnapshot&) const = default;
};

struct CrossRecoveryRequestSnapshot {
    ProcessPrincipal executor;
    DeliveryRequestId request;
    ActionId action_id;
    AuthorizationDeliveryId delivery_id;
    AuthorizationLifecycle lifecycle;
    auto operator<=>(const CrossRecoveryRequestSnapshot&) const = default;
};

struct CrossRecoveryRuntimeSnapshot {
    RecoverySlotState state;
    EpochSnapshot epochs;
    FreezeSeq freeze_seq;
    RecoveryInstanceId recovery_id;
    TransactionHash transaction_hash;
    AdmissionGeneration closing_generation;
    std::uint64_t issue_nonce_counter;
    std::uint64_t fault_seq_counter;
    std::vector<FrozenGrantRecord> ledger_prefix;
    std::vector<FaultFact> fault_batch;
    std::vector<FaultFact> frozen_fault_batch;
    std::vector<OwnerSnapshotAck> owner_acks;
    std::vector<std::optional<ActionReceipt>> receipt_slots;
    std::vector<ActionId> accepted_order;
    std::size_t accepted_count;
    std::vector<CrossRecoveryRequestSnapshot> requests;
    std::vector<CrossRecoveryAuthorizationSnapshot> authorizations;
    bool has_current_snapshot;
    bool has_compiled_program;
    bool has_current_program;
    bool has_proofs;
    bool has_barrier;
    auto operator<=>(const CrossRecoveryRuntimeSnapshot&) const = default;
};

class CrossRecoveryTestAccess {
public:
    static std::optional<CrossRecoveryRuntimeSnapshot> snapshot(
        const RecoveryCoordinator& coordinator) {
        std::scoped_lock guard{coordinator.mutex_};
        if (!coordinator.slot_.active) return std::nullopt;
        const auto& runtime = *coordinator.slot_.active;
        CrossRecoveryRuntimeSnapshot result{
            coordinator.slot_.state,
            coordinator.slot_.epochs,
            coordinator.slot_.freeze_seq,
            runtime.recovery_id,
            runtime.transaction_hash,
            coordinator.slot_.closing_generation,
            coordinator.slot_.issue_nonce_counter,
            coordinator.slot_.fault_seq_counter,
            runtime.ledger_prefix,
            runtime.fault_batch,
            runtime.current_snapshot
                ? runtime.current_snapshot->fault_batch
                : std::vector<FaultFact>{},
            {},
            runtime.receipts.slots,
            runtime.receipts.accepted_order,
            runtime.receipts.accepted_count,
            {},
            {},
            runtime.current_snapshot.has_value(),
            static_cast<bool>(runtime.compiled_program),
            static_cast<bool>(runtime.current_program),
            static_cast<bool>(runtime.proofs),
            static_cast<bool>(runtime.barrier)};
        result.owner_acks.reserve(runtime.owner_acks.size());
        for (const auto& [owner, ack] : runtime.owner_acks) {
            (void)owner;
            result.owner_acks.push_back(ack);
        }
        result.requests.reserve(runtime.requests.by_request.size());
        for (const auto& [key, request] : runtime.requests.by_request) {
            result.requests.push_back({key.first, key.second, request.action_id,
                                       request.delivery_id, request.lifecycle});
        }
        if (runtime.current_program) {
            for (const auto& node : runtime.current_program->action_dag()) {
                const auto* entry = runtime.journal.find_entry(node.id);
                if (!entry) continue;
                result.authorizations.push_back(
                    {node.id, node.operation, entry->issue_nonce,
                     entry->lifecycle, entry->delivery_request,
                     entry->recovery_id == runtime.recovery_id
                         && entry->transaction_hash == runtime.transaction_hash
                         && entry->token_digest == token_digest_of(
                             {entry->issue_nonce, runtime.recovery_id,
                              runtime.transaction_hash, node})});
            }
        }
        return result;
    }
};

}  // namespace recovery

#endif
