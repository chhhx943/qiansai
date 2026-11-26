#ifndef RECOVERY_AUTHORIZATION_HPP
#define RECOVERY_AUTHORIZATION_HPP

#include "recovery/auth.hpp"
#include "recovery/program.hpp"

#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <variant>
#include <vector>

namespace recovery {

#ifdef RECOVERY_TESTING
class EpochRuntimeTestAccess;
#endif

using ReceiptId = std::uint64_t;
using ReceiptHash = Digest;
using DeliveryRequestId = std::uint64_t;
using PermitTokenDigest = Digest;

struct StreamoffResult { int rc; auto operator<=>(const StreamoffResult&) const = default; };
struct CloseUnmapResult { bool completed; auto operator<=>(const CloseUnmapResult&) const = default; };
struct ReqbufsResult { int rc; auto operator<=>(const ReqbufsResult&) const = default; };
struct CloseFdResult { bool closed; auto operator<=>(const CloseFdResult&) const = default; };
struct RknnStopResult { int rc; auto operator<=>(const RknnStopResult&) const = default; };
struct RgaStopResult { int rc; auto operator<=>(const RgaStopResult&) const = default; };
struct MppStopResult { int rc; auto operator<=>(const MppStopResult&) const = default; };
struct PacketStopResult { bool stopped; auto operator<=>(const PacketStopResult&) const = default; };
struct RtmpDisconnectResult { bool disconnected; auto operator<=>(const RtmpDisconnectResult&) const = default; };
struct RecordStopResult { bool stopped; auto operator<=>(const RecordStopResult&) const = default; };
struct EpochInvalidateResult { bool invalidated; auto operator<=>(const EpochInvalidateResult&) const = default; };
struct ReinitResult { bool rebuilt; auto operator<=>(const ReinitResult&) const = default; };
struct LocalCompleteResult { bool branch_complete; auto operator<=>(const LocalCompleteResult&) const = default; };
struct EpochCommitResult { bool committed; auto operator<=>(const EpochCommitResult&) const = default; };
struct ReopenResult { bool reopened; auto operator<=>(const ReopenResult&) const = default; };

using ActionResult = std::variant<
    StreamoffResult, CloseUnmapResult, ReqbufsResult, CloseFdResult,
    RknnStopResult, RgaStopResult, MppStopResult, PacketStopResult,
    RtmpDisconnectResult, RecordStopResult, EpochInvalidateResult, ReinitResult,
    LocalCompleteResult, EpochCommitResult, ReopenResult>;

enum class ActionOutcome : std::uint32_t { SUCCEEDED, FAILED };
struct ActionReceipt {
    ReceiptId receipt_id;
    RecoveryInstanceId recovery_id;
    TransactionHash transaction_hash;
    ActionId action_id;
    OperationKind operation;
    ActionScope scope;
    EpochSnapshot old_epochs;
    std::optional<EpochSnapshot> new_epochs;
    ActionOutcome outcome;
    ActionResult result;
    std::set<ReceiptHash> prerequisite_receipts;
    ReceiptHash receipt_hash{};
    auto operator<=>(const ActionReceipt&) const = default;
};

struct CommitSuccess { ActionReceipt receipt; };
enum class CommitErrorCode : std::uint32_t {
    WRONG_PHASE, LOCAL_INCOMPLETE, PERMIT_MISMATCH, PERMIT_NOT_ISSUED,
    OLD_EPOCH_MISMATCH, DOMAIN_CONFLICT, EPOCH_OVERFLOW, RECEIPT_REJECT
};
struct CommitError { CommitErrorCode code; };
using CommitResult = std::variant<CommitSuccess, CommitError>;

static_assert(std::is_nothrow_move_constructible_v<CommitSuccess>);
static_assert(std::is_nothrow_move_assignable_v<CommitSuccess>);
static_assert(std::is_nothrow_move_constructible_v<CommitResult>);
static_assert(std::is_nothrow_move_assignable_v<CommitResult>);

enum class ReceiptSubmitResult : std::uint32_t {
    ACCEPTED, REJECT, REJECT_USE_COMMIT_REOPEN
};
struct SubmitReceiptResult { ReceiptSubmitResult status; };

enum class AuthorizationLifecycle : std::uint32_t {
    DELIVERABLE, CLAIMED, CONSUMED
};
struct DeliveryBinding {
    RecoveryInstanceId recovery_id;
    TransactionHash transaction_hash;
    ActionId action_id;
    std::uint64_t issue_nonce;
};
struct AuthorizationDeliveryId final {
    Digest value{};
    auto operator<=>(const AuthorizationDeliveryId&) const = default;
};
struct PermitTokenBinding {
    std::uint64_t issue_nonce;
    RecoveryInstanceId recovery_id;
    TransactionHash transaction_hash;
    ActionNode node;
};

struct AuthorizationEntry {
    RecoveryInstanceId recovery_id;
    TransactionHash transaction_hash;
    ActionNode node;
    std::set<ReceiptHash> prerequisite_receipts;
    std::uint64_t issue_nonce;
    PermitTokenDigest token_digest;
    AuthorizationLifecycle lifecycle;
    ExecutorPrincipal expected_executor;
    std::optional<ProcessPrincipal> claimed_by;
    std::optional<DeliveryRequestId> delivery_request;
};

struct ReceiptIndex {
    std::vector<std::optional<ActionReceipt>> slots;
    std::vector<ActionId> accepted_order;
    std::size_t accepted_count{};
    bool preallocate(std::size_t) noexcept;
};
struct RequestEntry {
    ActionId action_id;
    AuthorizationDeliveryId delivery_id;
    AuthorizationLifecycle lifecycle;
};
struct RequestIndex {
    std::map<std::pair<ProcessPrincipal, DeliveryRequestId>, RequestEntry> by_request;
    [[nodiscard]] const RequestEntry* find(
        const ProcessPrincipal&, DeliveryRequestId) const noexcept;
    void mark_consumed(const ProcessPrincipal&, DeliveryRequestId) noexcept;
};

enum class AuthorizationAdvanceCode : std::uint32_t {
    CREATED_DELIVERABLE, ALREADY_DELIVERABLE, WAITING_INFLIGHT,
    PHASE_COMPLETE, INTERNAL_FAILURE
};
struct AuthorizationAdvanceResult { AuthorizationAdvanceCode code; };

struct AuthorizationLease {
    AuthorizationDeliveryId delivery_id;
    ActionId action_id;
    OperationKind operation;
    ActionScope scope;
    PermitHandle handle;
};
enum class ClaimCode : std::uint32_t {
    CLAIMED, REDELIVERED, UNAUTHORIZED, ALREADY_CONSUMED, NONE_AVAILABLE
};
struct ClaimResult {
    ClaimCode code;
    std::optional<AuthorizationLease> lease;
    std::optional<AuthorizationDeliveryId> delivery_id;
};
struct LeaseDescriptor {
    AuthorizationDeliveryId delivery_id;
    ActionId action_id;
    OperationKind operation;
    AuthorizationLifecycle lifecycle;
    auto operator<=>(const LeaseDescriptor&) const = default;
};
enum class PollErrorCode : std::uint32_t { ALLOCATION_FAILED, UNAUTHORIZED };
struct PollError { PollErrorCode code; };
using PollResult = std::variant<std::vector<LeaseDescriptor>, PollError>;
struct PreparedAuthorizationAdvance {
    std::vector<AuthorizationEntry> entries;
    std::uint64_t final_nonce;
};
enum class PrepareAuthorizationErrorCode : std::uint32_t {
    NONCE_EXHAUSTED, ALLOCATION_FAILED
};
struct PrepareAuthorizationError { PrepareAuthorizationErrorCode code; };
using PrepareAuthorizationResult =
    std::variant<PreparedAuthorizationAdvance, PrepareAuthorizationError>;

struct PreparedReceiptCommit { ActionReceipt receipt; ActionId action_id; };
struct ReceiptValidateSuccess {};
enum class ReceiptValidateErrorCode : std::uint32_t {
    PERMIT_MISMATCH, NODE_MISMATCH, PREREQ_MISMATCH,
    OUTCOME_NOT_SUCCESS, RESULT_MISMATCH, HASH_MISMATCH, ALREADY_ACCEPTED,
    RECOVERY_TX_MISMATCH, ALLOCATION_FAILED
};
struct ReceiptValidateError { ReceiptValidateErrorCode code; };
using ReceiptValidateResult =
    std::variant<ReceiptValidateSuccess, ReceiptValidateError>;
enum class PrepareReceiptErrorCode : std::uint32_t {
    SLOT_NOT_PREALLOCATED, SLOT_OCCUPIED, CAPACITY_EXHAUSTED, ALLOCATION_FAILED
};
struct PrepareReceiptError { PrepareReceiptErrorCode code; };
using PrepareReceiptCommitResult =
    std::variant<PreparedReceiptCommit, PrepareReceiptError>;

class RecoveryCoordinator;
class AuthorizationJournal {
public:
    [[nodiscard]] bool lease_matches(
        const PermitHandle&, const ActionNode&) const noexcept;
    [[nodiscard]] const AuthorizationEntry* find_entry(ActionId) const noexcept;
    [[nodiscard]] std::optional<AuthorizationLifecycle>
    lifecycle_of(ActionId) const noexcept;
    [[nodiscard]] bool any_deliverable() const noexcept;
    [[nodiscard]] bool any_inflight() const noexcept;
    bool preallocate(std::size_t) noexcept;
private:
    friend class RecoveryCoordinator;
#ifdef RECOVERY_TESTING
    friend class EpochRuntimeTestAccess;
#endif
    void install_entry_locked(ActionId, AuthorizationEntry&&) noexcept;
    void claim_entry_locked(
        ActionId, const ProcessPrincipal&, DeliveryRequestId) noexcept;
    void consume_locked(PermitHandle&) noexcept;
    std::vector<std::optional<AuthorizationEntry>> entries_;
};

[[nodiscard]] AuthorizationDeliveryId delivery_id_of(const DeliveryBinding&);
[[nodiscard]] PermitTokenDigest token_digest_of(const PermitTokenBinding&);
[[nodiscard]] ReceiptHash compute_receipt_hash(const ActionReceipt&);
[[nodiscard]] bool action_result_success(
    const ActionNode&, const ActionResult&) noexcept;

inline void canonical_write(CanonicalWriter& writer, const DeliveryBinding& value) {
    canonical_write(writer, value.recovery_id);
    canonical_write(writer, value.transaction_hash);
    canonical_write(writer, value.action_id);
    canonical_write(writer, value.issue_nonce);
}
inline void canonical_write(CanonicalWriter& writer, const PermitTokenBinding& value) {
    canonical_write(writer, value.issue_nonce);
    canonical_write(writer, value.recovery_id);
    canonical_write(writer, value.transaction_hash);
    canonical_write(writer, value.node);
}

#define RECOVERY_ACTION_RESULT_CANONICAL(type, member) \
    inline void canonical_write(CanonicalWriter& writer, const type& value) { \
        canonical_write(writer, value.member); \
    }
RECOVERY_ACTION_RESULT_CANONICAL(StreamoffResult, rc)
RECOVERY_ACTION_RESULT_CANONICAL(CloseUnmapResult, completed)
RECOVERY_ACTION_RESULT_CANONICAL(ReqbufsResult, rc)
RECOVERY_ACTION_RESULT_CANONICAL(CloseFdResult, closed)
RECOVERY_ACTION_RESULT_CANONICAL(RknnStopResult, rc)
RECOVERY_ACTION_RESULT_CANONICAL(RgaStopResult, rc)
RECOVERY_ACTION_RESULT_CANONICAL(MppStopResult, rc)
RECOVERY_ACTION_RESULT_CANONICAL(PacketStopResult, stopped)
RECOVERY_ACTION_RESULT_CANONICAL(RtmpDisconnectResult, disconnected)
RECOVERY_ACTION_RESULT_CANONICAL(RecordStopResult, stopped)
RECOVERY_ACTION_RESULT_CANONICAL(EpochInvalidateResult, invalidated)
RECOVERY_ACTION_RESULT_CANONICAL(ReinitResult, rebuilt)
RECOVERY_ACTION_RESULT_CANONICAL(LocalCompleteResult, branch_complete)
RECOVERY_ACTION_RESULT_CANONICAL(EpochCommitResult, committed)
RECOVERY_ACTION_RESULT_CANONICAL(ReopenResult, reopened)
#undef RECOVERY_ACTION_RESULT_CANONICAL

inline void canonical_write(CanonicalWriter& writer, const ActionReceipt& value) {
    canonical_write(writer, value.receipt_id);
    canonical_write(writer, value.recovery_id);
    canonical_write(writer, value.transaction_hash);
    canonical_write(writer, value.action_id);
    canonical_write(writer, value.operation);
    canonical_write(writer, value.scope);
    canonical_write(writer, value.old_epochs);
    canonical_write(writer, value.new_epochs);
    canonical_write(writer, value.outcome);
    canonical_write(writer, value.result);
    canonical_write(writer, value.prerequisite_receipts);
    canonical_write(writer, value.receipt_hash);
}

}  // namespace recovery

#endif
