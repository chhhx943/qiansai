#include "recovery/authorization.hpp"
#include "recovery/proof.hpp"
#include "recovery/runtime.hpp"

#include <limits>
#include <new>
#include <type_traits>

namespace recovery {

bool ReceiptIndex::preallocate(std::size_t size) noexcept {
    try {
        std::vector<std::optional<ActionReceipt>> prepared_slots(size);
        std::vector<ActionId> prepared_order(size);
        slots = std::move(prepared_slots);
        accepted_order = std::move(prepared_order);
        accepted_count = 0;
        return true;
    } catch (...) { return false; }
}

const RequestEntry* RequestIndex::find(
    const ProcessPrincipal& executor, DeliveryRequestId request) const noexcept {
    const auto found = by_request.find({executor, request});
    return found == by_request.end() ? nullptr : &found->second;
}

void RequestIndex::mark_consumed(
    const ProcessPrincipal& executor, DeliveryRequestId request) noexcept {
    const auto found = by_request.find({executor, request});
    if (found != by_request.end())
        found->second.lifecycle = AuthorizationLifecycle::CONSUMED;
}

AuthorizationDeliveryId delivery_id_of(const DeliveryBinding& binding) {
    return {canonical_digest(binding)};
}

PermitTokenDigest token_digest_of(const PermitTokenBinding& binding) {
    return canonical_digest(binding);
}

ReceiptHash compute_receipt_hash(const ActionReceipt& receipt) {
    auto candidate = receipt;
    candidate.receipt_hash = {};
    return canonical_digest(candidate);
}

bool action_result_success(const ActionNode& node,
                           const ActionResult& result) noexcept {
    return std::visit([&](const auto& typed) {
        using Result = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Result, StreamoffResult>)
            return node.operation == OperationKind::STREAMOFF && typed.rc == 0;
        else if constexpr (std::is_same_v<Result, CloseUnmapResult>)
            return node.operation == OperationKind::CLOSE_UNMAP_HANDLE && typed.completed;
        else if constexpr (std::is_same_v<Result, ReqbufsResult>)
            return node.operation == OperationKind::REQBUFS_ZERO && typed.rc == 0;
        else if constexpr (std::is_same_v<Result, CloseFdResult>)
            return node.operation == OperationKind::CLOSE_FD && typed.closed;
        else if constexpr (std::is_same_v<Result, RknnStopResult>)
            return node.operation == OperationKind::RKNN_STOP && typed.rc == 0;
        else if constexpr (std::is_same_v<Result, RgaStopResult>)
            return node.operation == OperationKind::RGA_STOP && typed.rc == 0;
        else if constexpr (std::is_same_v<Result, MppStopResult>)
            return node.operation == OperationKind::MPP_STOP && typed.rc == 0;
        else if constexpr (std::is_same_v<Result, PacketStopResult>)
            return node.operation == OperationKind::PACKET_STOP && typed.stopped;
        else if constexpr (std::is_same_v<Result, RtmpDisconnectResult>)
            return node.operation == OperationKind::RTMP_DISCONNECT && typed.disconnected;
        else if constexpr (std::is_same_v<Result, RecordStopResult>)
            return node.operation == OperationKind::RECORD_STOP && typed.stopped;
        else if constexpr (std::is_same_v<Result, EpochInvalidateResult>)
            return node.operation == OperationKind::INVALIDATE_EPOCH && typed.invalidated;
        else if constexpr (std::is_same_v<Result, ReinitResult>)
            return node.operation == OperationKind::REINIT && typed.rebuilt;
        else if constexpr (std::is_same_v<Result, LocalCompleteResult>)
            return node.operation == OperationKind::LOCAL_COMPLETE && typed.branch_complete;
        else if constexpr (std::is_same_v<Result, EpochCommitResult>)
            return node.operation == OperationKind::COMMIT_EPOCH && typed.committed;
        else
            return node.operation == OperationKind::REOPEN_ADMISSION && typed.reopened;
    }, result);
}

const AuthorizationEntry* AuthorizationJournal::find_entry(ActionId id) const noexcept {
    if (id >= entries_.size() || !entries_[id]) return nullptr;
    return &*entries_[id];
}

std::optional<AuthorizationLifecycle>
AuthorizationJournal::lifecycle_of(ActionId id) const noexcept {
    const auto* entry = find_entry(id);
    return entry ? std::optional{entry->lifecycle} : std::nullopt;
}

bool AuthorizationJournal::any_deliverable() const noexcept {
    for (const auto& entry : entries_)
        if (entry && entry->lifecycle == AuthorizationLifecycle::DELIVERABLE) return true;
    return false;
}

bool AuthorizationJournal::any_inflight() const noexcept {
    for (const auto& entry : entries_)
        if (entry && entry->lifecycle == AuthorizationLifecycle::CLAIMED) return true;
    return false;
}

bool AuthorizationJournal::preallocate(std::size_t size) noexcept {
    try {
        std::vector<std::optional<AuthorizationEntry>> prepared(size);
        entries_ = std::move(prepared);
        return true;
    } catch (...) { return false; }
}

bool AuthorizationJournal::lease_matches(
    const PermitHandle& handle, const ActionNode& node) const noexcept {
    try {
        if (!handle.valid() || handle.action_id_ != node.id) return false;
        const auto* entry = find_entry(handle.action_id_);
        return entry && entry->recovery_id == handle.recovery_id_
            && entry->transaction_hash == handle.transaction_hash_
            && entry->issue_nonce == handle.issue_nonce_ && entry->node == node
            && entry->token_digest == token_digest_of(
                {handle.issue_nonce_, handle.recovery_id_,
                 handle.transaction_hash_, node});
    } catch (...) {
        return false;
    }
}

void AuthorizationJournal::install_entry_locked(
    ActionId id, AuthorizationEntry&& entry) noexcept {
    entries_[id] = std::move(entry);
}

void AuthorizationJournal::claim_entry_locked(
    ActionId id, const ProcessPrincipal& executor,
    DeliveryRequestId request) noexcept {
    entries_[id]->lifecycle = AuthorizationLifecycle::CLAIMED;
    entries_[id]->claimed_by = executor;
    entries_[id]->delivery_request = request;
}

void AuthorizationJournal::consume_locked(PermitHandle& handle) noexcept {
    entries_[handle.action_id_]->lifecycle = AuthorizationLifecycle::CONSUMED;
    handle.action_id_ = 0;
    handle.issue_nonce_ = 0;
}

AuthorizationAdvanceResult RecoveryCoordinator::advance_authorizations() noexcept {
    using Code = AuthorizationAdvanceCode;
    try {
    std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
    guard.lock();
    if (!slot_.active || !slot_.active->current_program)
        return {Code::PHASE_COMPLETE};
    if (slot_.state != RecoverySlotState::EXECUTING
        && slot_.state != RecoverySlotState::COMMITTING)
        return {Code::PHASE_COMPLETE};
    if (slot_.state == RecoverySlotState::EXECUTING) {
        if (slot_.active->journal.any_deliverable())
            return {Code::ALREADY_DELIVERABLE};
        if (slot_.active->journal.any_inflight())
            return {Code::WAITING_INFLIGHT};
    }

    std::vector<const ActionNode*> eligible;
    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::AUTH_ELIGIBILITY);
#endif
        if (slot_.state == RecoverySlotState::EXECUTING) {
            for (const auto& node : slot_.active->current_program->action_dag()) {
                if (node.phase == ActionPhase::REOPEN_ADMISSION
                    || slot_.active->journal.lifecycle_of(node.id)) continue;
                bool ready = false;
                if (node.prerequisites.empty()) {
                    ready = slot_.active->barrier
                        && slot_.active->barrier->recovery_id
                            == slot_.active->recovery_id
                        && slot_.active->barrier->transaction_hash
                            == slot_.active->current_program->transaction_hash()
                        && compute_barrier_digest(*slot_.active->barrier)
                            == slot_.active->barrier->barrier_digest;
                } else if (node.phase == ActionPhase::COMMIT_EPOCH) {
                    ready = all_local_complete_receipts_accepted_locked();
                } else {
                    ready = true;
                    for (const auto prerequisite : node.prerequisites) {
                        if (prerequisite >= slot_.active->receipts.slots.size()
                            || !slot_.active->receipts.slots[prerequisite]) {
                            ready = false;
                            break;
                        }
                    }
                }
                if (ready) eligible.push_back(&node);
            }
        } else {
            const auto* commit_receipt = accepted_commit_receipt_locked();
            if (!commit_receipt || !commit_receipt->new_epochs
                || *commit_receipt->new_epochs != slot_.epochs) {
                return {Code::WAITING_INFLIGHT};
            }
            const auto* reopen_node =
                find_unique_global_locked(OperationKind::REOPEN_ADMISSION);
            if (!reopen_node) {
                fail_closed_locked("reopen node missing or duplicated");
                return {Code::INTERNAL_FAILURE};
            }
            if (slot_.active->journal.lifecycle_of(reopen_node->id))
                return {Code::PHASE_COMPLETE};
            const auto prerequisites =
                accepted_prerequisites_locked(*reopen_node);
            if (!prerequisites
                || *prerequisites
                    != std::set<ReceiptHash>{commit_receipt->receipt_hash}) {
                fail_closed_locked("reopen prerequisite mismatch");
                return {Code::INTERNAL_FAILURE};
            }
            eligible.push_back(reopen_node);
        }
    } catch (...) {
        fail_closed_locked("authorization eligibility failed");
        return {Code::INTERNAL_FAILURE};
    }

    if (eligible.empty()) {
        if (slot_.active->journal.any_deliverable()) return {Code::ALREADY_DELIVERABLE};
        if (slot_.active->journal.any_inflight()) return {Code::WAITING_INFLIGHT};
        return {Code::PHASE_COMPLETE};
    }
    if (slot_.issue_nonce_counter == std::numeric_limits<std::uint64_t>::max()
        || std::numeric_limits<std::uint64_t>::max() - slot_.issue_nonce_counter
            < eligible.size()) {
        fail_closed_locked("authorization nonce exhausted");
        return {Code::INTERNAL_FAILURE};
    }

    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::AUTH_PREPARE);
#endif
        PreparedAuthorizationAdvance prepared;
        prepared.entries.reserve(eligible.size());
        auto nonce = slot_.issue_nonce_counter;
        for (const auto* node : eligible) {
            ++nonce;
            AuthorizationEntry entry{
                slot_.active->recovery_id, slot_.active->transaction_hash, *node, {}, nonce,
                {}, AuthorizationLifecycle::DELIVERABLE, node->expected_executor,
                std::nullopt, std::nullopt};
            for (const auto prerequisite : node->prerequisites)
                entry.prerequisite_receipts.insert(
                    slot_.active->receipts.slots[prerequisite]->receipt_hash);
            entry.token_digest = token_digest_of(
                {nonce, entry.recovery_id, entry.transaction_hash, entry.node});
            prepared.entries.push_back(std::move(entry));
        }
        prepared.final_nonce = nonce;
        static_assert(std::is_nothrow_move_assignable_v<AuthorizationEntry>);
        for (auto& entry : prepared.entries) {
            const auto id = entry.node.id;
            slot_.active->journal.install_entry_locked(id, std::move(entry));
        }
        slot_.issue_nonce_counter = prepared.final_nonce;
        if (slot_.state == RecoverySlotState::COMMITTING)
            slot_.state = RecoverySlotState::REOPENING;
        return {Code::CREATED_DELIVERABLE};
    } catch (...) {
        fail_closed_locked("authorization preparation failed");
        return {Code::INTERNAL_FAILURE};
    }
    } catch (...) {
        return {Code::INTERNAL_FAILURE};
    }
}

ClaimResult RecoveryCoordinator::claim_authorization(
    const AuthenticatedExecutorContext& context,
    DeliveryRequestId request) noexcept {
    try {
    std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
    guard.lock();
    if (slot_.state != RecoverySlotState::EXECUTING
        || !slot_.active || !slot_.active->current_program)
        return {ClaimCode::NONE_AVAILABLE, std::nullopt, std::nullopt};
    if (!ExecutorAuthenticator::verify_context(context))
        return {ClaimCode::UNAUTHORIZED, std::nullopt, std::nullopt};
    const auto executor = context.process();
    if (!executor)
        return {ClaimCode::UNAUTHORIZED, std::nullopt, std::nullopt};

    if (const auto* existing = slot_.active->requests.find(*executor, request)) {
        const auto* entry = slot_.active->journal.find_entry(existing->action_id);
        if (entry && existing->lifecycle == AuthorizationLifecycle::CLAIMED
            && entry->lifecycle == AuthorizationLifecycle::CLAIMED) {
            AuthorizationLease lease{
                existing->delivery_id, existing->action_id, entry->node.operation,
                entry->node.scope, PermitHandle{entry->recovery_id,
                    entry->transaction_hash, existing->action_id, entry->issue_nonce}};
            return {ClaimCode::REDELIVERED, std::move(lease), std::nullopt};
        }
        return {ClaimCode::ALREADY_CONSUMED, std::nullopt,
                existing->delivery_id};
    }

    bool any_deliverable = false;
    try {
        for (const auto& node : slot_.active->current_program->action_dag()) {
            if (node.phase == ActionPhase::COMMIT_EPOCH
                || node.phase == ActionPhase::REOPEN_ADMISSION) continue;
            const auto* entry = slot_.active->journal.find_entry(node.id);
            if (!entry || entry->lifecycle != AuthorizationLifecycle::DELIVERABLE) continue;
            any_deliverable = true;
            if (entry->expected_executor != ExecutorPrincipal{*executor}) continue;
            const auto delivery = delivery_id_of(
                {entry->recovery_id, entry->transaction_hash, node.id, entry->issue_nonce});
            AuthorizationLease lease{
                delivery, node.id, node.operation, node.scope,
                PermitHandle{entry->recovery_id, entry->transaction_hash,
                             node.id, entry->issue_nonce}};
            ClaimResult prepared{
                ClaimCode::CLAIMED, std::move(lease), std::nullopt};
            static_assert(std::is_nothrow_move_constructible_v<ClaimResult>);
#ifdef RECOVERY_TESTING
            checkpoint(RuntimeFailureStage::CLAIM_REQUEST_INSERT);
#endif
            const auto [ignored, inserted] = slot_.active->requests.by_request.emplace(
                std::make_pair(*executor, request),
                RequestEntry{node.id, delivery, AuthorizationLifecycle::CLAIMED});
            (void)ignored;
            if (!inserted)
                return {ClaimCode::NONE_AVAILABLE, std::nullopt, std::nullopt};
            slot_.active->journal.claim_entry_locked(node.id, *executor, request);
            return prepared;
        }
    } catch (...) {
        return {ClaimCode::NONE_AVAILABLE, std::nullopt, std::nullopt};
    }
    return {any_deliverable ? ClaimCode::UNAUTHORIZED : ClaimCode::NONE_AVAILABLE,
            std::nullopt, std::nullopt};
    } catch (...) {
        return {ClaimCode::NONE_AVAILABLE, std::nullopt, std::nullopt};
    }
}

PollResult RecoveryCoordinator::poll_authorizations(
    const AuthenticatedExecutorContext& context) noexcept {
    try {
    std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
    guard.lock();
    if (!ExecutorAuthenticator::verify_context(context))
        return PollError{PollErrorCode::UNAUTHORIZED};
    const auto executor = context.process();
    if (!executor) return PollError{PollErrorCode::UNAUTHORIZED};
    try {
        std::vector<LeaseDescriptor> descriptors;
        if (!slot_.active) return descriptors;
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::POLL_COLLECT);
#endif
        for (const auto& [key, request] : slot_.active->requests.by_request) {
            if (key.first != *executor) continue;
            const auto* entry = slot_.active->journal.find_entry(request.action_id);
            if (!entry) continue;
            descriptors.push_back({request.delivery_id, request.action_id,
                                   entry->node.operation, request.lifecycle});
        }
        return descriptors;
    } catch (...) {
        return PollError{PollErrorCode::ALLOCATION_FAILED};
    }
    } catch (...) {
        return PollError{PollErrorCode::ALLOCATION_FAILED};
    }
}

const ActionNode* RecoveryCoordinator::find_node_locked(ActionId id) const noexcept {
    if (!slot_.active || !slot_.active->current_program) return nullptr;
    for (const auto& node : slot_.active->current_program->action_dag())
        if (node.id == id) return &node;
    return nullptr;
}

ReceiptValidateResult RecoveryCoordinator::validate_receipt_locked(
    const ActionReceipt& receipt, const ActionNode& node,
    const PermitHandle& handle, const RecoveryInstanceRuntime& runtime) noexcept {
    using Error = ReceiptValidateErrorCode;
    try {
        if (receipt.recovery_id != runtime.recovery_id
            || receipt.transaction_hash != runtime.transaction_hash)
            return ReceiptValidateError{Error::RECOVERY_TX_MISMATCH};
        const auto* entry = runtime.journal.find_entry(receipt.action_id);
        if (!entry || entry->recovery_id != runtime.recovery_id
            || entry->transaction_hash != runtime.transaction_hash
            || entry->node != node)
            return ReceiptValidateError{Error::PERMIT_MISMATCH};
        if (entry->lifecycle != AuthorizationLifecycle::DELIVERABLE
            && entry->lifecycle != AuthorizationLifecycle::CLAIMED)
            return ReceiptValidateError{Error::PERMIT_MISMATCH};
        if (!runtime.journal.lease_matches(handle, node))
            return ReceiptValidateError{Error::PERMIT_MISMATCH};
        if (receipt.action_id != node.id || receipt.operation != node.operation
            || receipt.scope != node.scope || receipt.old_epochs != node.old_epochs
            || receipt.new_epochs != node.new_epochs)
            return ReceiptValidateError{Error::NODE_MISMATCH};

        std::set<ReceiptHash> prerequisites;
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::RECEIPT_VALIDATE_PREREQS);
#endif
        for (const auto prerequisite : node.prerequisites) {
            if (prerequisite >= runtime.receipts.slots.size()
                || !runtime.receipts.slots[prerequisite])
                return ReceiptValidateError{Error::PREREQ_MISMATCH};
            prerequisites.insert(
                runtime.receipts.slots[prerequisite]->receipt_hash);
        }
        if (entry->prerequisite_receipts != prerequisites
            || receipt.prerequisite_receipts != prerequisites)
            return ReceiptValidateError{Error::PREREQ_MISMATCH};
        if (receipt.outcome != ActionOutcome::SUCCEEDED)
            return ReceiptValidateError{Error::OUTCOME_NOT_SUCCESS};
        if (!action_result_success(node, receipt.result))
            return ReceiptValidateError{Error::RESULT_MISMATCH};
        if (compute_receipt_hash(receipt) != receipt.receipt_hash)
            return ReceiptValidateError{Error::HASH_MISMATCH};
        if (receipt.action_id >= runtime.receipts.slots.size()
            || runtime.receipts.slots[receipt.action_id])
            return ReceiptValidateError{Error::ALREADY_ACCEPTED};
        return ReceiptValidateSuccess{};
    } catch (...) {
        return ReceiptValidateError{Error::ALLOCATION_FAILED};
    }
}

PrepareReceiptCommitResult RecoveryCoordinator::prepare_receipt_commit_locked(
    const ActionReceipt& receipt, const ActionNode& node,
    const RecoveryInstanceRuntime& runtime) noexcept {
    using Error = PrepareReceiptErrorCode;
    try {
        if (node.id >= runtime.receipts.slots.size())
            return PrepareReceiptError{Error::SLOT_NOT_PREALLOCATED};
        if (runtime.receipts.slots[node.id])
            return PrepareReceiptError{Error::SLOT_OCCUPIED};
        if (runtime.receipts.accepted_count
            >= runtime.receipts.accepted_order.size())
            return PrepareReceiptError{Error::CAPACITY_EXHAUSTED};
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::RECEIPT_PREPARE);
#endif
        return PreparedReceiptCommit{receipt, node.id};
    } catch (...) {
        return PrepareReceiptError{Error::ALLOCATION_FAILED};
    }
}

void RecoveryCoordinator::commit_receipt_locked(
    PreparedReceiptCommit&& prepared, const ActionNode& node,
    PermitHandle& handle, RecoveryInstanceRuntime& runtime) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<ActionReceipt>);
    static_assert(std::is_nothrow_move_assignable_v<std::optional<ActionReceipt>>);
    static_assert(std::is_nothrow_move_assignable_v<AuthorizationEntry>);
    static_assert(
        std::is_nothrow_move_assignable_v<std::optional<AuthorizationEntry>>);
    const auto consumed_id = handle.action_id_;
    runtime.journal.consume_locked(handle);
    if (const auto* entry = runtime.journal.find_entry(consumed_id);
        entry && entry->claimed_by && entry->delivery_request) {
        runtime.requests.mark_consumed(
            *entry->claimed_by, *entry->delivery_request);
    }
    runtime.receipts.slots[prepared.action_id] = std::move(prepared.receipt);
    runtime.receipts.accepted_order[runtime.receipts.accepted_count++] =
        prepared.action_id;
    (void)node;
}

SubmitReceiptResult RecoveryCoordinator::submit_action_receipt(
    const ActionReceipt& receipt, PermitHandle&& handle) noexcept {
    using Status = ReceiptSubmitResult;
    try {
    std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
    guard.lock();
    if (slot_.state != RecoverySlotState::EXECUTING
        || !slot_.active || !slot_.active->current_program)
        return {Status::REJECT};
    if (receipt.operation == OperationKind::REOPEN_ADMISSION)
        return {Status::REJECT_USE_COMMIT_REOPEN};
    if (receipt.operation == OperationKind::COMMIT_EPOCH)
        return {Status::REJECT};
    const auto* node = find_node_locked(receipt.action_id);
    if (!node) return {Status::REJECT};

    const auto validated = validate_receipt_locked(
        receipt, *node, handle, *slot_.active);
    if (const auto* error = std::get_if<ReceiptValidateError>(&validated)) {
        if (error->code == ReceiptValidateErrorCode::OUTCOME_NOT_SUCCESS
            || error->code == ReceiptValidateErrorCode::RESULT_MISMATCH)
            fail_closed_locked("outcome/result invalid");
        return {Status::REJECT};
    }
    auto prepared = prepare_receipt_commit_locked(
        receipt, *node, *slot_.active);
    if (std::holds_alternative<PrepareReceiptError>(prepared)) {
        fail_closed_locked("prepare receipt commit failed");
        return {Status::REJECT};
    }
    commit_receipt_locked(
        std::move(std::get<PreparedReceiptCommit>(prepared)),
        *node, handle, *slot_.active);
    return {Status::ACCEPTED};
    } catch (...) {
        return {Status::REJECT};
    }
}

}  // namespace recovery
