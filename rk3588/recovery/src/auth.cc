#include "recovery/auth.hpp"
#include "recovery/crypto.hpp"

namespace recovery {

std::mutex ExecutorAuthenticator::mutex_;
std::map<ChannelId, ExecutorAuthenticator::ChannelEntry>
    ExecutorAuthenticator::channels_;
std::uint64_t ExecutorAuthenticator::next_channel_id_ = 1;

bool ExecutorAuthenticator::verify_context(
    const AuthenticatedExecutorContext& context) noexcept {
    try {
        if (std::holds_alternative<CoordinatorPrincipal>(context.principal_)) return false;
        std::scoped_lock guard{mutex_};
        const auto found = channels_.find(context.channel_);
        return found != channels_.end()
            && found->second.principal == context.principal_
            && found->second.session_nonce == context.session_nonce_
            && found->second.credential_digest == context.credential_digest_;
    } catch (...) { return false; }
}

std::optional<AuthenticatedExecutorContext>
ExecutorAuthenticator::issue_trusted(ProcessPrincipal principal,
                                     std::uint64_t session_nonce,
                                     Digest credential_digest) noexcept {
    try {
        std::scoped_lock guard{mutex_};
        if (next_channel_id_ == 0) return std::nullopt;
        const ChannelId channel{next_channel_id_++};
        ExecutorPrincipal executor{principal};
        const auto [found, inserted] = channels_.emplace(
            channel, ChannelEntry{executor, session_nonce, credential_digest});
        if (!inserted) return std::nullopt;
        return AuthenticatedExecutorContext{
            found->second.principal, channel, found->second.session_nonce,
            found->second.credential_digest};
    } catch (...) { return std::nullopt; }
}

bool ExecutorAuthenticator::revoke_trusted(ChannelId channel) noexcept {
    try {
        std::scoped_lock guard{mutex_};
        return channels_.erase(channel) == 1;
    } catch (...) { return false; }
}

#ifdef RECOVERY_TESTING
AuthenticatedExecutorContext ExecutorAuthenticator::issue_for_testing(
    ProcessPrincipal principal) {
    const auto digest = canonical_digest(principal);
    auto context = issue_trusted(principal, 1, digest);
    if (!context) throw std::bad_alloc{};
    return std::move(*context);
}
#endif

}  // namespace recovery
