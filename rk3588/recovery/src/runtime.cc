#include "recovery/runtime.hpp"
#include "recovery/proof.hpp"

#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <sys/syscall.h>
#if __has_include(<sys/pidfd.h>)
#include <sys/pidfd.h>
#define RECOVERY_PIDFD_NONBLOCK PIDFD_NONBLOCK
#else
#define RECOVERY_PIDFD_NONBLOCK O_NONBLOCK
#endif
#include <type_traits>
#include <unistd.h>

namespace recovery {
namespace {
class LinuxProcessExitKernel final : public ProcessExitKernel {
public:
    std::optional<std::uint64_t> read_start_generation(ProcessId pid) override {
        std::ifstream stat{"/proc/" + std::to_string(pid) + "/stat"};
        std::string line;
        if (!std::getline(stat, line)) return std::nullopt;
        const auto close = line.rfind(')');
        if (close == std::string::npos) return std::nullopt;
        std::istringstream fields{line.substr(close + 2)};
        std::string field;
        for (unsigned index = 3; index <= 22; ++index) {
            if (!(fields >> field)) return std::nullopt;
            if (index == 22) {
                try { return std::stoull(field); } catch (...) { return std::nullopt; }
            }
        }
        return std::nullopt;
    }
    OwnedFd pidfd_open(ProcessId pid) override {
        constexpr unsigned int kPidfdNonblock = RECOVERY_PIDFD_NONBLOCK;
        // Some deployed sysroots provide <sys/pidfd.h> but their libc does not
        // export pidfd_open.  Use the stable kernel ABI on every platform so
        // host and target builds have identical PIDFD_NONBLOCK semantics.
        return OwnedFd{static_cast<int>(
            ::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), kPidfdNonblock))};
    }
};
LinuxProcessExitKernel linux_kernel;

Digest snapshot_digest(const FrozenRuntimeSnapshot& snapshot) {
    FrozenRuntimeSnapshot candidate = snapshot;
    candidate.snapshot_digest = {};
    return canonical_digest(candidate);
}

Digest owner_ack_view_digest(const OwnerSnapshotAck& ack) {
    OwnerSnapshotAck candidate = ack;
    candidate.local_view_digest = {};
    return canonical_digest(candidate);
}

bool snapshot_contents_match(const RecoveryInstanceRuntime& runtime) {
    return runtime.current_policy.contents && runtime.current_registry.contents
        && canonical_digest(*runtime.current_policy.contents) == runtime.current_policy.digest
        && canonical_digest(*runtime.current_registry.contents) == runtime.current_registry.digest;
}

bool expected_executors_match(const ValidatedRecoveryProgram& program,
                              const FrozenRuntimeSnapshot& snapshot,
                              const PolicySnapshot& policy) {
    const auto derived = canonical_action_closure(program.resource_plans(), snapshot, policy);
    const auto* dag = std::get_if<std::vector<ActionNode>>(&derived);
    if (!dag || dag->size() != program.action_dag().size()) return false;
    for (std::size_t i = 0; i < dag->size(); ++i)
        if ((*dag)[i].expected_executor != program.action_dag()[i].expected_executor) return false;
    return true;
}
}

OwnedFd::~OwnedFd() noexcept { if (fd_ >= 0) ::close(fd_); }
OwnedFd::OwnedFd(OwnedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
OwnedFd& OwnedFd::operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

RecoveryInstanceRuntime::RecoveryInstanceRuntime(RecoveryInstanceId id, FaultFact first)
    : recovery_id(id), fault_batch{std::move(first)} {}

FallbackKind worst_fallback(FallbackKind a, FallbackKind b) noexcept {
    return (a == FallbackKind::BOARD_RESTART_REQUIRED
            || b == FallbackKind::BOARD_RESTART_REQUIRED)
        ? FallbackKind::BOARD_RESTART_REQUIRED : FallbackKind::PARENT_SERVICE_RESTART;
}

bool descriptors_registry_agree(const std::vector<ProcessExitBindingDescriptor>& descriptors,
                                const ProcessExitBindingRegistry& registry) noexcept {
    if (descriptors.size() != registry.size()) return false;
    for (const auto& descriptor : descriptors) {
        const auto found = registry.find(descriptor.id);
        if (found == registry.end() || found->first != found->second.descriptor.id
            || found->second.descriptor != descriptor || !found->second.pidfd.valid()) return false;
    }
    return true;
}

RecoveryCoordinator::RecoveryCoordinator(BootId boot, ProcessExitKernel* kernel)
    : kernel_(kernel ? kernel : &linux_kernel) { slot_.boot_id = boot; }

RecoveryCoordinatorSnapshot RecoveryCoordinator::snapshot() const {
    std::scoped_lock guard{mutex_};
    return {slot_.state, slot_.epochs, slot_.admission_open,
            slot_.closing_generation, slot_.active.has_value()};
}

#ifdef RECOVERY_TESTING
RecoveryCoordinator::RecoveryCoordinator(BootId boot, ProcessExitKernel* kernel,
                                         RuntimeFailureProvider* provider)
    : RecoveryCoordinator(boot, kernel) { failure_provider_ = provider; }

void RecoveryCoordinator::checkpoint(RuntimeFailureStage stage) {
    if (failure_provider_) failure_provider_->checkpoint(stage);
}
#endif

void RecoveryCoordinator::fail_closed_locked(const char* reason) noexcept {
    slot_.state = RecoverySlotState::FAIL_CLOSED;
    try { slot_.fail_reason = reason; }
    catch (...) { slot_.fail_reason.clear(); }
}

bool RecoveryCoordinator::report_fault(const FaultReport& report) noexcept {
    try {
        std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
        guard.lock();
        try {
        const auto make_fact = [this, &report](std::uint64_t sequence) {
            return FaultFact{FaultIdentity{slot_.boot_id, sequence}, report.kind,
                             report.subject, report.observed_epochs, report.evidence};
        };
        if (slot_.state == RecoverySlotState::IDLE) {
#ifdef RECOVERY_TESTING
            checkpoint(RuntimeFailureStage::REPORT_PREPARE);
#endif
            const auto next_sequence = slot_.recovery_seq + 1;
            const auto next_fault_sequence = slot_.fault_seq_counter + 1;
            RecoveryInstanceRuntime prepared{
                {slot_.boot_id, next_sequence, next_sequence},
                make_fact(next_fault_sequence)};
            slot_.active.emplace(std::move(prepared));
            slot_.fault_seq_counter = next_fault_sequence;
            slot_.recovery_seq = next_sequence;
            slot_.state = RecoverySlotState::PREPARING;
            return true;
        }
        if (slot_.state == RecoverySlotState::PREPARING
            || slot_.state == RecoverySlotState::FREEZING) {
            if (!slot_.active) {
                fail_closed_locked("active runtime missing"); return false;
            }
#ifdef RECOVERY_TESTING
            checkpoint(RuntimeFailureStage::REPORT_APPEND);
#endif
            const auto next_fault_sequence = slot_.fault_seq_counter + 1;
            auto prepared = make_fact(next_fault_sequence);
            slot_.active->fault_batch.push_back(std::move(prepared));
            slot_.fault_seq_counter = next_fault_sequence;
            return true;
        }
        if (slot_.state == RecoverySlotState::FAIL_CLOSED) return false;
        fail_closed_locked("fault after batch close");
        } catch (...) { fail_closed_locked("fault allocation failed"); }
    } catch (...) {
        return false;
    }
    return false;
}

std::optional<RecoveryCoordinator::PreparedFaultBatchClose>
RecoveryCoordinator::prepare_fault_batch_close() const {
    if (!slot_.active || slot_.active->fault_batch.empty()) return std::nullopt;
    PreparedFaultBatchClose prepared;
    prepared.fault_batch = slot_.active->fault_batch;
    prepared.fault_batch_cutoff = slot_.fault_seq_counter;
    std::sort(prepared.fault_batch.begin(), prepared.fault_batch.end(),
              [](const auto& left, const auto& right) { return left.id < right.id; });
    for (std::size_t index = 0; index < prepared.fault_batch.size(); ++index) {
        const auto& fact = prepared.fault_batch[index];
        if (fact.id.boot_id != slot_.boot_id
            || fact.id.fault_seq > prepared.fault_batch_cutoff
            || (index != 0 && !(prepared.fault_batch[index - 1].id < fact.id)))
            return std::nullopt;
    }
    if (prepared.fault_batch.back().id.fault_seq != prepared.fault_batch_cutoff)
        return std::nullopt;
    prepared.fault_batch_digest = canonical_digest(prepared.fault_batch);
    return prepared;
}

bool RecoveryCoordinator::begin_freeze(
    const std::vector<FrozenGrantRecord>& ledger,
    const std::vector<OwnerExecutorBinding>& owner_bindings) noexcept {
    std::scoped_lock guard{mutex_};
    if (slot_.state != RecoverySlotState::PREPARING || !slot_.active) return false;
    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::BEGIN_FREEZE_PREPARE);
#endif
        auto prepared_ledger = ledger;
        auto prepared_bindings = owner_bindings;
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::BEGIN_FREEZE_AFTER_COPY);
#endif
        GrantSeq prepared_cutoff{};
        for (const auto& grant : prepared_ledger)
            prepared_cutoff = std::max(prepared_cutoff, grant.identity.seq);
        const auto prepared_digest = canonical_digest(prepared_ledger);
        const FreezeSeq prepared_freeze_seq = slot_.freeze_seq + 1;
        FreezeRequest prepared_request{slot_.active->recovery_id, prepared_freeze_seq,
            slot_.boot_id, slot_.closing_generation, prepared_cutoff, prepared_digest};
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::BEGIN_FREEZE_AFTER_DIGEST);
#endif
        slot_.active->ledger_prefix = std::move(prepared_ledger);
        slot_.active->owner_executor_bindings = std::move(prepared_bindings);
        slot_.active->grant_cutoff = prepared_cutoff;
        slot_.active->ledger_prefix_digest = prepared_digest;
        slot_.active->freeze_request = std::move(prepared_request);
        slot_.freeze_seq = prepared_freeze_seq;
        slot_.admission_open = false;
        slot_.state = RecoverySlotState::FREEZING;
        return true;
    }
    catch (...) { return false; }
}

bool RecoveryCoordinator::commit_freeze() noexcept {
    std::scoped_lock guard{mutex_};
    if (slot_.state == RecoverySlotState::FROZEN) {
        fail_closed_locked("freeze already published"); return false;
    }
    if (slot_.state != RecoverySlotState::FREEZING || !slot_.active) return false;
    if (slot_.active->current_snapshot || !slot_.active->process_exit_binding_registry.empty()) {
        fail_closed_locked("freeze already published"); return false;
    }
    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_TARGETS);
        checkpoint(RuntimeFailureStage::FREEZE_OWNER_ACK);
#endif
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_BATCH_PREPARE);
#endif
        const auto prepared_fault_batch = prepare_fault_batch_close();
        if (!prepared_fault_batch) {
            fail_closed_locked("fault batch not freezeable"); return false;
        }
        std::set<OwnerId> all_owners;
        for (const auto& grant : slot_.active->ledger_prefix) all_owners.insert(grant.owner);
        if (all_owners.size() != slot_.active->owner_acks.size()) {
            fail_closed_locked("owner ack missing"); return false;
        }
        for (const auto owner : all_owners) {
            if (!slot_.active->owner_acks.contains(owner)) {
                fail_closed_locked("owner ack missing"); return false;
            }
        }
        if (canonical_digest(slot_.active->ledger_prefix)
            != slot_.active->ledger_prefix_digest) {
            fail_closed_locked("prefix digest"); return false;
        }
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_PREFIX_DIGEST);
        checkpoint(RuntimeFailureStage::FREEZE_BATCH_DIGEST);
#endif
        std::set<ProcessIdentity> targets;
        for (const auto& grant : slot_.active->ledger_prefix)
            for (const auto& target : grant.execution_targets)
                if (const auto* process = std::get_if<ProcessIdentity>(&target)) targets.insert(*process);
        for (const auto& fault : prepared_fault_batch->fault_batch)
            if (fault.kind == FaultKind::WORKER_PROCESS_EXIT && fault.subject.process)
                targets.insert(*fault.subject.process);

        std::vector<ProcessExitBindingDescriptor> descriptors;
        ProcessExitBindingRegistry registry;
        descriptors.reserve(targets.size());
        ProcessExitBindingSeq sequence{};
        for (const auto& process : targets) {
            const auto before = kernel_->read_start_generation(process.pid);
            if (!before) { fail_closed_locked("READ_BEFORE_FAILED"); return false; }
            if (*before != process.start_generation) { fail_closed_locked("GENERATION_MISMATCH_BEFORE"); return false; }
            OwnedFd fd = kernel_->pidfd_open(process.pid);
            if (!fd.valid()) { fail_closed_locked("PIDFD_OPEN_FAILED"); return false; }
            const auto after = kernel_->read_start_generation(process.pid);
            if (!after) { fail_closed_locked("READ_AFTER_FAILED"); return false; }
            if (*after != process.start_generation) { fail_closed_locked("GENERATION_MISMATCH_AFTER"); return false; }
            ProcessExitBindingDescriptor descriptor{{slot_.active->recovery_id, slot_.freeze_seq, sequence++}, process};
            descriptors.push_back(descriptor);
            registry.emplace(descriptor.id, OwnedProcessExitBinding{descriptor, std::move(fd)});
        }
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_AFTER_BINDINGS);
        checkpoint(RuntimeFailureStage::FREEZE_SNAPSHOT);
#endif
        FrozenRuntimeSnapshot snapshot{slot_.active->recovery_id, slot_.freeze_seq,
            slot_.boot_id, slot_.closing_generation, slot_.active->grant_cutoff,
            slot_.active->ledger_prefix, slot_.active->ledger_prefix_digest,
            slot_.active->owner_ack_canonical_digest, prepared_fault_batch->fault_batch,
            prepared_fault_batch->fault_batch_cutoff, prepared_fault_batch->fault_batch_digest,
            slot_.active->owner_executor_bindings, std::move(descriptors),
            slot_.epochs, {}};
        if (!process_exit_bindings_are_legal(snapshot)
            || !descriptors_registry_agree(snapshot.process_exit_bindings, registry)) {
            fail_closed_locked("process exit bindings illegal"); return false;
        }
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_BINDING_LEGALITY);
#endif
        snapshot.snapshot_digest = snapshot_digest(snapshot);
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FREEZE_SNAPSHOT_DIGEST);
#endif
        static_assert(std::is_nothrow_move_assignable_v<ProcessExitBindingRegistry>);
        static_assert(std::is_nothrow_move_assignable_v<FrozenRuntimeSnapshot>);
        static_assert(std::is_nothrow_move_assignable_v<std::optional<FrozenRuntimeSnapshot>>);
        slot_.active->process_exit_binding_registry = std::move(registry);
        slot_.active->current_snapshot = std::move(snapshot);
        slot_.state = RecoverySlotState::FROZEN;
        return true;
    } catch (...) { fail_closed_locked("freeze commit build failed"); return false; }
}

bool RecoveryCoordinator::submit_owner_snapshot_ack(const OwnerSnapshotAck& ack) noexcept {
    std::scoped_lock guard{mutex_};
    if (slot_.state != RecoverySlotState::FREEZING || !slot_.active
        || ack.recovery_id != slot_.active->recovery_id
        || ack.freeze_seq != slot_.freeze_seq || ack.boot_id != slot_.boot_id
        || ack.closing_generation != slot_.closing_generation
        || owner_ack_view_digest(ack) != ack.local_view_digest
        || slot_.active->owner_acks.contains(ack.owner)) return false;
    std::set<GrantIdentity> expected_grants;
    std::set<LeaseIdentity> expected_leases;
    for (const auto& grant : slot_.active->ledger_prefix) {
        if (grant.owner != ack.owner) continue;
        expected_grants.insert(grant.identity);
        if (grant.lease) expected_leases.insert(*grant.lease);
    }
    if (expected_grants.empty() || ack.locally_observed_grants != expected_grants
        || ack.outstanding_leases != expected_leases) return false;
    try {
        auto candidate_acks = slot_.active->owner_acks;
        candidate_acks.emplace(ack.owner, ack);
        std::vector<OwnerSnapshotAck> canonical;
        canonical.reserve(candidate_acks.size());
        for (const auto& entry : candidate_acks) canonical.push_back(entry.second);
        const auto candidate_digest = canonical_digest(canonical);
        static_assert(std::is_nothrow_move_assignable_v<decltype(candidate_acks)>);
        slot_.active->owner_acks = std::move(candidate_acks);
        slot_.active->owner_ack_canonical_digest = candidate_digest;
        return true;
    } catch (...) { fail_closed_locked("owner ack allocation failed"); return false; }
}

bool RecoveryCoordinator::begin_compile(PolicySnapshot policy,
                                         CapabilityRegistrySnapshot registry) noexcept {
    std::scoped_lock guard{mutex_};
    if (slot_.state != RecoverySlotState::FROZEN || !slot_.active) {
        fail_closed_locked("state"); return false;
    }
#ifdef RECOVERY_TESTING
    try { checkpoint(RuntimeFailureStage::BEGIN_COMPILE_PUBLISH); }
    catch (...) { fail_closed_locked("compile publish failed"); return false; }
#endif
    slot_.active->current_policy = std::move(policy);
    slot_.active->current_registry = std::move(registry);
    slot_.state = RecoverySlotState::COMPILING;
    return true;
}

void RecoveryCoordinator::finish_compile(CompileResult result) noexcept {
    std::scoped_lock guard{mutex_};
    if (slot_.state != RecoverySlotState::COMPILING || !slot_.active) {
        fail_closed_locked("state"); return;
    }
    if (auto* failure = std::get_if<CompileFailure>(&result)) {
        if (failure->kind == CompileFailure::Kind::FALLBACK_REQUIRED) {
            if (!failure->fallback) {
                fail_closed_locked("no fallback action");
            } else {
                slot_.active->fallback_pending = true;
                slot_.active->pending_fallback = failure->fallback;
            }
        } else fail_closed_locked("compile rejected");
        return;
    }
    try {
#ifdef RECOVERY_TESTING
        checkpoint(RuntimeFailureStage::FINISH_COMPILE_INSTALL);
#endif
        auto program = std::make_shared<const ValidatedRecoveryProgram>(
            std::get<ValidatedRecoveryProgram>(std::move(result)));
        if (!slot_.active->current_snapshot
            || !snapshot_contents_match(*slot_.active)
            || program->snapshot_digest() != slot_.active->current_snapshot->snapshot_digest
            || program->policy_digest() != slot_.active->current_policy.digest
            || program->registry_digest() != slot_.active->current_registry.digest) {
            fail_closed_locked("compiled input mismatch"); return;
        }
        slot_.active->compiled_program = std::move(program);
    } catch (...) { fail_closed_locked("compile install failed"); }
}

void RecoveryCoordinator::execute_fallback(std::optional<FallbackAction> fallback) noexcept {
    std::scoped_lock guard{mutex_};
    if (slot_.state != RecoverySlotState::COMPILING || !slot_.active
        || !slot_.active->fallback_pending
        || fallback != slot_.active->pending_fallback) {
        fail_closed_locked("state"); return;
    }
    fail_closed_locked("fallback");
}

bool RecoveryCoordinator::seal_validated_program(
    const std::shared_ptr<const ValidatedRecoveryProgram>& program) noexcept {
    std::scoped_lock guard{mutex_};
    if (slot_.state != RecoverySlotState::COMPILING || !slot_.active
        || !program || program.get() != slot_.active->compiled_program.get()
        || !slot_.active->current_snapshot
        || compute_transaction_hash(*program) != program->transaction_hash()
        || snapshot_digest(*slot_.active->current_snapshot)
             != slot_.active->current_snapshot->snapshot_digest
        || program->snapshot_digest() != slot_.active->current_snapshot->snapshot_digest
        || program->policy_digest() != slot_.active->current_policy.digest
        || program->registry_digest() != slot_.active->current_registry.digest
        || program->faults() != slot_.active->current_snapshot->fault_batch) {
        fail_closed_locked("seal validation"); return false;
    }
    if (!snapshot_contents_match(*slot_.active)
        || !expected_executors_match(*program, *slot_.active->current_snapshot,
                                     slot_.active->current_policy)) {
        fail_closed_locked("seal contents validation"); return false;
    }
#ifdef RECOVERY_TESTING
    try { checkpoint(RuntimeFailureStage::SEAL_PUBLISH); }
    catch (...) { fail_closed_locked("seal publish failed"); return false; }
#endif
    slot_.active->current_program = slot_.active->compiled_program;
    slot_.active->transaction_hash = program->transaction_hash();
    slot_.state = RecoverySlotState::SEALED;
    return true;
}

void RecoveryCoordinator::begin_quiesce() noexcept {
    try {
    std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
    guard.lock();
    if (slot_.state != RecoverySlotState::SEALED || !slot_.active
        || !slot_.active->current_program
        || slot_.active->current_program->action_dag().empty()) {
        fail_closed_locked("quiesce precondition");
        return;
    }
    try {
        slot_.active->proofs = std::make_shared<ProofIndex>();
        slot_.state = RecoverySlotState::QUIESCING;
    } catch (...) {
        fail_closed_locked("proof index allocation failed");
    }
    } catch (...) {
        return;
    }
}

void RecoveryCoordinator::commit_barrier() noexcept {
    try {
    std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
    guard.lock();
    if (slot_.state != RecoverySlotState::QUIESCING || !slot_.active
        || !slot_.active->current_program || !slot_.active->proofs
        || !barrier_complete(*slot_.active->current_program, *slot_.active->proofs)) {
        fail_closed_locked("barrier incomplete");
        return;
    }
    try {
        BarrierCommit barrier{slot_.active->recovery_id,
                              slot_.active->current_program->transaction_hash(),
                              {}, {}, {}};
        for (const auto& predicate : slot_.active->current_program->execution_predicates())
            barrier.satisfied_predicates.insert(predicate.key);
        for (const auto& predicate : slot_.active->current_program->device_predicates())
            barrier.satisfied_predicates.insert(predicate.key);
        for (const auto& [key, record] : slot_.active->proofs->by_key) {
            (void)key;
            barrier.consumed_proofs.insert(proof_hash_of(record));
        }
        barrier.barrier_digest = compute_barrier_digest(barrier);
        const auto table_size = slot_.active->current_program->action_dag().size();
        if (!slot_.active->journal.preallocate(table_size)
            || !slot_.active->receipts.preallocate(table_size)) {
            fail_closed_locked("runtime table preallocation failed");
            return;
        }
        slot_.active->barrier = std::make_shared<BarrierCommit>(std::move(barrier));
        slot_.state = RecoverySlotState::EXECUTING;
    } catch (...) {
        fail_closed_locked("barrier publication failed");
    }
    } catch (...) {
        return;
    }
}

const ActionNode* RecoveryCoordinator::find_unique_global_locked(
    OperationKind operation) const noexcept {
    if (!slot_.active || !slot_.active->current_program) return nullptr;
    const ActionNode* result = nullptr;
    for (const auto& node : slot_.active->current_program->action_dag()) {
        if (node.operation != operation) continue;
        if (!std::holds_alternative<GlobalScope>(node.scope) || result) return nullptr;
        result = &node;
    }
    return result;
}

std::optional<std::set<ReceiptHash>>
RecoveryCoordinator::accepted_prerequisites_locked(
    const ActionNode& node) const noexcept {
    try {
        if (!slot_.active) return std::nullopt;
        std::set<ReceiptHash> result;
        for (const auto prerequisite : node.prerequisites) {
            if (prerequisite >= slot_.active->receipts.slots.size()
                || !slot_.active->receipts.slots[prerequisite]) {
                return std::nullopt;
            }
            result.insert(
                slot_.active->receipts.slots[prerequisite]->receipt_hash);
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool RecoveryCoordinator::all_local_complete_receipts_accepted_locked() const {
    if (!slot_.active || !slot_.active->current_program) return false;
    const auto& runtime = *slot_.active;
    if (runtime.receipts.accepted_count > runtime.receipts.accepted_order.size())
        return false;

    std::set<ActionId> required;
    for (const auto& node : runtime.current_program->action_dag()) {
        if (node.phase == ActionPhase::LOCAL_COMPLETE) required.insert(node.id);
    }
    if (required.empty()) return false;

    std::set<ActionId> accepted;
    for (std::size_t index = 0; index < runtime.receipts.accepted_count; ++index) {
        const auto id = runtime.receipts.accepted_order[index];
        const auto* node = find_node_locked(id);
        if (!node || id >= runtime.receipts.slots.size()
            || !runtime.receipts.slots[id]) {
            return false;
        }
        if (node->phase == ActionPhase::LOCAL_COMPLETE
            && !accepted.insert(id).second) {
            return false;
        }
    }
    if (accepted != required) return false;

    for (const auto id : required) {
        const auto* node = find_node_locked(id);
        const auto* entry = runtime.journal.find_entry(id);
        if (!node || !entry
            || entry->lifecycle != AuthorizationLifecycle::CONSUMED
            || entry->recovery_id != runtime.recovery_id
            || entry->transaction_hash != runtime.transaction_hash
            || entry->node != *node) {
            return false;
        }
        const auto& receipt = *runtime.receipts.slots[id];
        if (receipt.recovery_id != runtime.recovery_id
            || receipt.transaction_hash != runtime.transaction_hash
            || receipt.action_id != node->id
            || receipt.operation != node->operation
            || receipt.scope != node->scope
            || receipt.old_epochs != node->old_epochs
            || receipt.new_epochs != node->new_epochs
            || receipt.outcome != ActionOutcome::SUCCEEDED
            || !action_result_success(*node, receipt.result)
            || compute_receipt_hash(receipt) != receipt.receipt_hash) {
            return false;
        }
    }
    return true;
}

const ActionReceipt*
RecoveryCoordinator::accepted_commit_receipt_locked() const {
    if (!slot_.active || !slot_.active->current_program) return nullptr;
    const auto& runtime = *slot_.active;
    const auto* node =
        find_unique_global_locked(OperationKind::COMMIT_EPOCH);
    if (!node || node->id >= runtime.receipts.slots.size()) return nullptr;

    const auto* entry = runtime.journal.find_entry(node->id);
    if (!entry || entry->lifecycle != AuthorizationLifecycle::CONSUMED
        || entry->recovery_id != runtime.recovery_id
        || entry->transaction_hash != runtime.transaction_hash
        || entry->node != *node) {
        return nullptr;
    }

    const auto& receipts = runtime.receipts;
    if (receipts.accepted_count > receipts.accepted_order.size())
        return nullptr;
    std::size_t occupied = 0;
    for (const auto& slot : receipts.slots)
        if (slot) ++occupied;
    if (occupied != receipts.accepted_count) return nullptr;

    bool commit_seen = false;
    for (std::size_t index = 0; index < receipts.accepted_count; ++index) {
        const auto accepted_id = receipts.accepted_order[index];
        if (accepted_id >= receipts.slots.size()
            || !receipts.slots[accepted_id]) {
            return nullptr;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier)
            if (receipts.accepted_order[earlier] == accepted_id)
                return nullptr;
        if (accepted_id == node->id) commit_seen = true;
    }
    if (!commit_seen || !receipts.slots[node->id]) return nullptr;

    const auto& receipt = *receipts.slots[node->id];
    if (entry->prerequisite_receipts != receipt.prerequisite_receipts)
        return nullptr;
    std::size_t unique_prerequisites = 0;
    for (auto prerequisite = node->prerequisites.begin();
         prerequisite != node->prerequisites.end(); ++prerequisite) {
        if (*prerequisite >= receipts.slots.size()
            || !receipts.slots[*prerequisite]) {
            return nullptr;
        }
        const auto& hash = receipts.slots[*prerequisite]->receipt_hash;
        if (!entry->prerequisite_receipts.contains(hash)) return nullptr;
        bool first_hash = true;
        for (auto earlier = node->prerequisites.begin(); earlier != prerequisite;
             ++earlier) {
            if (receipts.slots[*earlier]
                && receipts.slots[*earlier]->receipt_hash == hash) {
                first_hash = false;
                break;
            }
        }
        if (first_hash) ++unique_prerequisites;
    }
    if (entry->prerequisite_receipts.size() != unique_prerequisites
        || receipt.receipt_id != node->id
        || receipt.recovery_id != runtime.recovery_id
        || receipt.transaction_hash != runtime.transaction_hash
        || receipt.action_id != node->id
        || receipt.operation != OperationKind::COMMIT_EPOCH
        || receipt.scope != node->scope
        || receipt.old_epochs != node->old_epochs
        || receipt.new_epochs != node->new_epochs
        || !receipt.new_epochs
        || receipt.outcome != ActionOutcome::SUCCEEDED
        || !action_result_success(*node, receipt.result)
        || compute_receipt_hash(receipt) != receipt.receipt_hash) {
        return nullptr;
    }
    return &receipt;
}

std::optional<ActionReceipt> RecoveryCoordinator::make_commit_receipt_locked(
    const ActionNode& node, const EpochSnapshot& old_epochs,
    const EpochSnapshot& new_epochs) noexcept {
    try {
        if (!slot_.active) return std::nullopt;
        auto prerequisites = accepted_prerequisites_locked(node);
        if (!prerequisites) return std::nullopt;
        ActionReceipt receipt{
            node.id, slot_.active->recovery_id, slot_.active->transaction_hash,
            node.id, OperationKind::COMMIT_EPOCH, node.scope, old_epochs,
            new_epochs, ActionOutcome::SUCCEEDED, EpochCommitResult{true},
            std::move(*prerequisites), {}};
        receipt.receipt_hash = compute_receipt_hash(receipt);
        return receipt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ActionReceipt> RecoveryCoordinator::make_reopen_receipt_locked(
    const ActionNode& node) noexcept {
    try {
        if (!slot_.active) return std::nullopt;
        auto prerequisites = accepted_prerequisites_locked(node);
        if (!prerequisites) return std::nullopt;
        ActionReceipt receipt{
            node.id, slot_.active->recovery_id, slot_.active->transaction_hash,
            node.id, OperationKind::REOPEN_ADMISSION, node.scope,
            node.old_epochs, node.new_epochs, ActionOutcome::SUCCEEDED,
            ReopenResult{true}, std::move(*prerequisites), {}};
        receipt.receipt_hash = compute_receipt_hash(receipt);
        return receipt;
    } catch (...) {
        return std::nullopt;
    }
}

void RecoveryCoordinator::destroy_active_runtime_locked() noexcept {
    slot_.active.reset();
}

CommitResult RecoveryCoordinator::commit_epoch() noexcept {
    using Error = CommitErrorCode;
    try {
        std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
        guard.lock();
        try {
            if (slot_.state != RecoverySlotState::EXECUTING) {
                if (slot_.state == RecoverySlotState::COMMITTING)
                    fail_closed_locked("duplicate commit_epoch");
                return CommitError{Error::WRONG_PHASE};
            }
            if (!slot_.active || !slot_.active->current_program
                || !slot_.active->current_snapshot) {
                fail_closed_locked("commit runtime missing");
                return CommitError{Error::WRONG_PHASE};
            }
            if (!all_local_complete_receipts_accepted_locked()) {
                fail_closed_locked("local incomplete");
                return CommitError{Error::LOCAL_INCOMPLETE};
            }

            const auto* commit_node =
                find_unique_global_locked(OperationKind::COMMIT_EPOCH);
            if (!commit_node) {
                fail_closed_locked("no commit node");
                return CommitError{Error::WRONG_PHASE};
            }
            const auto* commit_entry =
                slot_.active->journal.find_entry(commit_node->id);
            if (!commit_entry
                || commit_entry->lifecycle
                    != AuthorizationLifecycle::DELIVERABLE) {
                return CommitError{Error::PERMIT_NOT_ISSUED};
            }
            PermitHandle commit_lease{
                commit_entry->recovery_id, commit_entry->transaction_hash,
                commit_node->id, commit_entry->issue_nonce};

            if (slot_.active->current_snapshot->epoch_snapshot
                != commit_node->old_epochs) {
                fail_closed_locked("old epoch");
                return CommitError{Error::OLD_EPOCH_MISMATCH};
            }
            const auto& plans = slot_.active->current_program->resource_plans();
            if (!epoch_domains_consistent(plans)) {
                fail_closed_locked("domain conflict");
                return CommitError{Error::DOMAIN_CONFLICT};
            }
            const auto domains = canonical_epoch_domains(plans);
            const auto new_epochs = advance_epochs(slot_.epochs, domains);
            if (!new_epochs) {
                fail_closed_locked("epoch overflow");
                return CommitError{Error::EPOCH_OVERFLOW};
            }
            if (!commit_node->new_epochs
                || *commit_node->new_epochs != *new_epochs) {
                fail_closed_locked("commit new_epochs mismatch");
                return CommitError{Error::OLD_EPOCH_MISMATCH};
            }

            auto receipt = make_commit_receipt_locked(
                *commit_node, slot_.epochs, *new_epochs);
            if (!receipt) {
                fail_closed_locked("make commit receipt failed");
                return CommitError{Error::RECEIPT_REJECT};
            }
            const auto validation = validate_receipt_locked(
                *receipt, *commit_node, commit_lease, *slot_.active);
            if (!std::holds_alternative<ReceiptValidateSuccess>(validation)) {
                fail_closed_locked("commit receipt invalid");
                return CommitError{Error::RECEIPT_REJECT};
            }
            auto prepared = prepare_receipt_commit_locked(
                *receipt, *commit_node, *slot_.active);
            auto* prepared_commit = std::get_if<PreparedReceiptCommit>(&prepared);
            if (!prepared_commit) {
                fail_closed_locked("prepare commit failed");
                return CommitError{Error::RECEIPT_REJECT};
            }

            static_assert(std::is_nothrow_move_constructible_v<CommitResult>);
            slot_.epochs = *new_epochs;
            commit_receipt_locked(std::move(*prepared_commit), *commit_node,
                                  commit_lease, *slot_.active);
            slot_.state = RecoverySlotState::COMMITTING;
            return CommitSuccess{std::move(*receipt)};
        } catch (...) {
            fail_closed_locked("commit_epoch allocation failed");
            return CommitError{Error::RECEIPT_REJECT};
        }
    } catch (...) {
        return CommitError{Error::RECEIPT_REJECT};
    }
}

CommitResult RecoveryCoordinator::commit_reopen() noexcept {
    using Error = CommitErrorCode;
    try {
        std::unique_lock<std::mutex> guard{mutex_, std::defer_lock};
        guard.lock();
        try {
            if (slot_.state != RecoverySlotState::REOPENING)
                return CommitError{Error::WRONG_PHASE};
            if (!slot_.active || !slot_.active->current_program) {
                fail_closed_locked("reopen runtime missing");
                return CommitError{Error::WRONG_PHASE};
            }
            if (slot_.closing_generation
                == std::numeric_limits<AdmissionGeneration>::max()) {
                fail_closed_locked("admission generation overflow");
                return CommitError{Error::EPOCH_OVERFLOW};
            }

            const auto* reopen_node =
                find_unique_global_locked(OperationKind::REOPEN_ADMISSION);
            if (!reopen_node) {
                fail_closed_locked("no reopen node");
                return CommitError{Error::WRONG_PHASE};
            }
            const auto* reopen_entry =
                slot_.active->journal.find_entry(reopen_node->id);
            if (!reopen_entry
                || reopen_entry->lifecycle
                    != AuthorizationLifecycle::DELIVERABLE) {
                return CommitError{Error::PERMIT_NOT_ISSUED};
            }
            PermitHandle reopen_lease{
                reopen_entry->recovery_id, reopen_entry->transaction_hash,
                reopen_node->id, reopen_entry->issue_nonce};

            auto receipt_opt = make_reopen_receipt_locked(*reopen_node);
            if (!receipt_opt) {
                fail_closed_locked("make reopen receipt failed");
                return CommitError{Error::RECEIPT_REJECT};
            }
            ActionReceipt receipt = std::move(*receipt_opt);
            const auto validation = validate_receipt_locked(
                receipt, *reopen_node, reopen_lease, *slot_.active);
            if (!std::holds_alternative<ReceiptValidateSuccess>(validation)) {
                fail_closed_locked("reopen receipt invalid");
                return CommitError{Error::RECEIPT_REJECT};
            }
            auto prepared = prepare_receipt_commit_locked(
                receipt, *reopen_node, *slot_.active);
            auto* prepared_commit =
                std::get_if<PreparedReceiptCommit>(&prepared);
            if (!prepared_commit) {
                fail_closed_locked("prepare reopen failed");
                return CommitError{Error::RECEIPT_REJECT};
            }

            static_assert(std::is_nothrow_move_constructible_v<CommitResult>);
            commit_receipt_locked(std::move(*prepared_commit), *reopen_node,
                                  reopen_lease, *slot_.active);
            ++slot_.closing_generation;
            slot_.admission_open = true;
            slot_.state = RecoverySlotState::IDLE;
            destroy_active_runtime_locked();
            return CommitSuccess{std::move(receipt)};
        } catch (...) {
            fail_closed_locked("commit_reopen allocation failed");
            return CommitError{Error::RECEIPT_REJECT};
        }
    } catch (...) {
        return CommitError{Error::RECEIPT_REJECT};
    }
}
}  // namespace recovery
