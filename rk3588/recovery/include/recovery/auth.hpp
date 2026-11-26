#ifndef RECOVERY_AUTH_HPP
#define RECOVERY_AUTH_HPP

#include "recovery/canonical.hpp"

#include <map>
#include <mutex>
#include <optional>

namespace recovery {

class TrustedIpcAcceptor;
#ifdef RECOVERY_TESTING
class AuthenticatedExecutorContextTestAccess;
#endif

struct ChannelId {
    std::uint64_t value{};
    [[nodiscard]] bool valid() const noexcept { return value != 0; }
    auto operator<=>(const ChannelId&) const = default;
};

class ExecutorAuthenticator;
class AuthenticatedExecutorContext {
public:
    [[nodiscard]] const ExecutorPrincipal& principal() const noexcept {
        return principal_;
    }
    [[nodiscard]] std::optional<ProcessPrincipal> process() const noexcept {
        if (const auto* process = std::get_if<ProcessPrincipal>(&principal_))
            return *process;
        return std::nullopt;
    }
    AuthenticatedExecutorContext(const AuthenticatedExecutorContext&) = delete;
    AuthenticatedExecutorContext& operator=(const AuthenticatedExecutorContext&) = delete;
    AuthenticatedExecutorContext(AuthenticatedExecutorContext&&) noexcept = default;
    AuthenticatedExecutorContext& operator=(AuthenticatedExecutorContext&&) noexcept = default;
private:
    friend class ExecutorAuthenticator;
    ExecutorPrincipal principal_;
    ChannelId channel_;
    std::uint64_t session_nonce_{};
    Digest credential_digest_{};

    AuthenticatedExecutorContext(ExecutorPrincipal principal, ChannelId channel,
                                 std::uint64_t session_nonce,
                                 Digest credential_digest) noexcept
        : principal_(std::move(principal)), channel_(channel),
          session_nonce_(session_nonce),
          credential_digest_(std::move(credential_digest)) {}
};

class ExecutorAuthenticator {
public:
    [[nodiscard]] static bool verify_context(
        const AuthenticatedExecutorContext&) noexcept;
private:
    friend class TrustedIpcAcceptor;
#ifdef RECOVERY_TESTING
    friend class AuthenticatedExecutorContextTestAccess;
    static AuthenticatedExecutorContext issue_for_testing(ProcessPrincipal);
#endif
    struct ChannelEntry {
        ExecutorPrincipal principal;
        std::uint64_t session_nonce;
        Digest credential_digest;
    };
    static std::mutex mutex_;
    static std::map<ChannelId, ChannelEntry> channels_;
    static std::uint64_t next_channel_id_;
    static std::optional<AuthenticatedExecutorContext>
    issue_trusted(ProcessPrincipal, std::uint64_t, Digest) noexcept;
    static bool revoke_trusted(ChannelId) noexcept;
};

}  // namespace recovery

#endif
