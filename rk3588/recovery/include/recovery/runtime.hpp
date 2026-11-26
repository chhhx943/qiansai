#ifndef RECOVERY_RUNTIME_HPP
#define RECOVERY_RUNTIME_HPP

#include "recovery/authorization.hpp"
#include "recovery/compiler.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace recovery {

struct BarrierCommit;
struct ProofIndex;

#ifdef RECOVERY_TESTING
class RecoveryRuntimeTestAccess;
class AuthorizationRuntimeTestAccess;
class EpochRuntimeTestAccess;
class CrossRecoveryTestAccess;
struct RecoveryTestAccess;
enum class RuntimeFailureStage : std::uint32_t {
    REPORT_PREPARE, REPORT_APPEND, BEGIN_FREEZE_PREPARE,
    BEGIN_FREEZE_AFTER_COPY, BEGIN_FREEZE_AFTER_DIGEST,
    FREEZE_TARGETS, FREEZE_OWNER_ACK, FREEZE_PREFIX_DIGEST,
    FREEZE_BATCH_PREPARE, FREEZE_BATCH_DIGEST, FREEZE_AFTER_BINDINGS,
    FREEZE_BINDING_LEGALITY, FREEZE_SNAPSHOT, FREEZE_SNAPSHOT_DIGEST,
    BEGIN_COMPILE_PUBLISH, FINISH_COMPILE_INSTALL, SEAL_PUBLISH,
    AUTH_ELIGIBILITY, AUTH_PREPARE, CLAIM_REQUEST_INSERT, POLL_COLLECT,
    RECEIPT_VALIDATE_PREREQS, RECEIPT_PREPARE
};
class RuntimeFailureProvider {
public:
    virtual ~RuntimeFailureProvider() = default;
    virtual void checkpoint(RuntimeFailureStage) = 0;
};
#endif

enum class RecoverySlotState : std::uint32_t {
    IDLE, PREPARING, FREEZING, FROZEN, COMPILING, SEALED, QUIESCING,
    EXECUTING, COMMITTING, REOPENING, FAIL_CLOSED
};

class OwnedFd {
public:
    OwnedFd() noexcept = default;
    explicit OwnedFd(int fd) noexcept : fd_(fd) {}
    ~OwnedFd() noexcept;
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept;
    OwnedFd& operator=(OwnedFd&& other) noexcept;
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int get() const noexcept { return fd_; }
private:
    int fd_{-1};
};

struct OwnedProcessExitBinding {
    ProcessExitBindingDescriptor descriptor;
    OwnedFd pidfd;
};
struct FreezeRequest {
    RecoveryInstanceId recovery_id;
    FreezeSeq freeze_seq;
    BootId boot_id;
    AdmissionGeneration closing_generation;
    GrantSeq grant_cutoff;
    Digest ledger_prefix_digest;
    auto operator<=>(const FreezeRequest&) const = default;
};
using ProcessExitBindingRegistry =
    std::map<ProcessExitBindingId, OwnedProcessExitBinding>;

enum class ProcessExitBindFailure : std::uint32_t {
    READ_BEFORE_FAILED, GENERATION_MISMATCH_BEFORE, PIDFD_OPEN_FAILED,
    READ_AFTER_FAILED, GENERATION_MISMATCH_AFTER
};

class ProcessExitKernel {
public:
    virtual ~ProcessExitKernel() = default;
    virtual std::optional<std::uint64_t> read_start_generation(ProcessId) = 0;
    virtual OwnedFd pidfd_open(ProcessId) = 0;
};

struct RecoveryInstanceRuntime {
    RecoveryInstanceId recovery_id;
    std::vector<FaultFact> fault_batch;
    std::vector<FrozenGrantRecord> ledger_prefix;
    GrantSeq grant_cutoff{};
    Digest ledger_prefix_digest{};
    std::optional<FreezeRequest> freeze_request;
    std::vector<OwnerExecutorBinding> owner_executor_bindings;
    std::map<OwnerId, OwnerSnapshotAck> owner_acks;
    Digest owner_ack_canonical_digest{};
    std::optional<FrozenRuntimeSnapshot> current_snapshot;
    ProcessExitBindingRegistry process_exit_binding_registry;
    PolicySnapshot current_policy{};
    CapabilityRegistrySnapshot current_registry{};
    std::shared_ptr<const ValidatedRecoveryProgram> compiled_program;
    std::shared_ptr<const ValidatedRecoveryProgram> current_program;
    TransactionHash transaction_hash{};
    bool fallback_pending{};
    std::optional<FallbackAction> pending_fallback;
    std::shared_ptr<ProofIndex> proofs;
    std::shared_ptr<BarrierCommit> barrier;
    AuthorizationJournal journal;
    ReceiptIndex receipts;
    RequestIndex requests;

    RecoveryInstanceRuntime(RecoveryInstanceId id, FaultFact first);
    RecoveryInstanceRuntime(RecoveryInstanceRuntime&&) noexcept = default;
    RecoveryInstanceRuntime& operator=(RecoveryInstanceRuntime&&) noexcept = default;
    RecoveryInstanceRuntime(const RecoveryInstanceRuntime&) = delete;
    RecoveryInstanceRuntime& operator=(const RecoveryInstanceRuntime&) = delete;
};

struct RecoverySlot {
    BootId boot_id{};
    RecoverySeq recovery_seq{};
    FreezeSeq freeze_seq{};
    RecoverySlotState state{RecoverySlotState::IDLE};
    EpochSnapshot epochs{};
    AdmissionGeneration closing_generation{};
    bool admission_open{true};
    std::uint64_t issue_nonce_counter{};
    std::uint64_t fault_seq_counter{};
    std::optional<RecoveryInstanceRuntime> active;
    std::string fail_reason;
};

struct RecoveryCoordinatorSnapshot {
    RecoverySlotState state;
    EpochSnapshot epochs;
    bool admission_open;
    AdmissionGeneration closing_generation;
    bool has_active;
    auto operator<=>(const RecoveryCoordinatorSnapshot&) const = default;
};

[[nodiscard]] FallbackKind worst_fallback(FallbackKind, FallbackKind) noexcept;
[[nodiscard]] bool descriptors_registry_agree(
    const std::vector<ProcessExitBindingDescriptor>&,
    const ProcessExitBindingRegistry&) noexcept;

class RecoveryCoordinator {
public:
    explicit RecoveryCoordinator(BootId, ProcessExitKernel* = nullptr);
#ifdef RECOVERY_TESTING
    RecoveryCoordinator(BootId, ProcessExitKernel*, RuntimeFailureProvider*);
#endif
    [[nodiscard]] bool report_fault(const FaultReport&) noexcept;
    [[nodiscard]] bool begin_freeze(
        const std::vector<FrozenGrantRecord>&,
        const std::vector<OwnerExecutorBinding>& = {}) noexcept;
    [[nodiscard]] bool commit_freeze() noexcept;
    [[nodiscard]] bool submit_owner_snapshot_ack(const OwnerSnapshotAck&) noexcept;
    [[nodiscard]] bool begin_compile(PolicySnapshot = {},
                                     CapabilityRegistrySnapshot = {}) noexcept;
    void finish_compile(CompileResult) noexcept;
    void execute_fallback(std::optional<FallbackAction>) noexcept;
    [[nodiscard]] bool seal_validated_program(
        const std::shared_ptr<const ValidatedRecoveryProgram>&) noexcept;
    void begin_quiesce() noexcept;
    void commit_barrier() noexcept;
    [[nodiscard]] AuthorizationAdvanceResult advance_authorizations() noexcept;
    [[nodiscard]] ClaimResult claim_authorization(
        const AuthenticatedExecutorContext&, DeliveryRequestId) noexcept;
    [[nodiscard]] PollResult poll_authorizations(
        const AuthenticatedExecutorContext&) noexcept;
    [[nodiscard]] SubmitReceiptResult submit_action_receipt(
        const ActionReceipt&, PermitHandle&&) noexcept;
    [[nodiscard]] CommitResult commit_epoch() noexcept;
    [[nodiscard]] CommitResult commit_reopen() noexcept;
    [[nodiscard]] RecoveryCoordinatorSnapshot snapshot() const;
    [[nodiscard]] RecoverySlotState state() const {
        return snapshot().state;
    }
    [[nodiscard]] EpochSnapshot epochs() const { return snapshot().epochs; }
    [[nodiscard]] bool admission_open() const {
        return snapshot().admission_open;
    }
    [[nodiscard]] AdmissionGeneration closing_generation() const {
        return snapshot().closing_generation;
    }
    [[nodiscard]] bool has_active() const { return snapshot().has_active; }
#ifdef RECOVERY_TESTING
    [[nodiscard]] const RecoveryInstanceRuntime& active() const { return *slot_.active; }
#endif
private:
#ifdef RECOVERY_TESTING
    friend class RecoveryRuntimeTestAccess;
    friend class AuthorizationRuntimeTestAccess;
    friend class EpochRuntimeTestAccess;
    friend class CrossRecoveryTestAccess;
    friend struct RecoveryTestAccess;
#endif
    mutable std::mutex mutex_;
    RecoverySlot slot_;
    ProcessExitKernel* kernel_;
#ifdef RECOVERY_TESTING
    RuntimeFailureProvider* failure_provider_{};
    void checkpoint(RuntimeFailureStage);
#endif
    struct PreparedFaultBatchClose {
        std::vector<FaultFact> fault_batch;
        std::uint64_t fault_batch_cutoff{};
        Digest fault_batch_digest{};
    };
    [[nodiscard]] std::optional<PreparedFaultBatchClose>
    prepare_fault_batch_close() const;
    [[nodiscard]] const ActionNode* find_node_locked(ActionId) const noexcept;
    [[nodiscard]] const ActionNode* find_unique_global_locked(
        OperationKind) const noexcept;
    [[nodiscard]] std::optional<std::set<ReceiptHash>>
    accepted_prerequisites_locked(const ActionNode&) const noexcept;
    [[nodiscard]] bool all_local_complete_receipts_accepted_locked() const;
    [[nodiscard]] const ActionReceipt*
    accepted_commit_receipt_locked() const;
    [[nodiscard]] std::optional<ActionReceipt> make_commit_receipt_locked(
        const ActionNode&, const EpochSnapshot&, const EpochSnapshot&) noexcept;
    [[nodiscard]] std::optional<ActionReceipt> make_reopen_receipt_locked(
        const ActionNode&) noexcept;
    [[nodiscard]] ReceiptValidateResult validate_receipt_locked(
        const ActionReceipt&, const ActionNode&, const PermitHandle&,
        const RecoveryInstanceRuntime&) noexcept;
    [[nodiscard]] PrepareReceiptCommitResult prepare_receipt_commit_locked(
        const ActionReceipt&, const ActionNode&,
        const RecoveryInstanceRuntime&) noexcept;
    void commit_receipt_locked(PreparedReceiptCommit&&, const ActionNode&,
                               PermitHandle&, RecoveryInstanceRuntime&) noexcept;
    void destroy_active_runtime_locked() noexcept;
    void fail_closed_locked(const char*) noexcept;
};

}  // namespace recovery
#endif
