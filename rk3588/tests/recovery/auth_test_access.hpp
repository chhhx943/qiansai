#ifndef RECOVERY_AUTH_TEST_ACCESS_HPP
#define RECOVERY_AUTH_TEST_ACCESS_HPP

#include "recovery/auth.hpp"

namespace recovery {

class AuthenticatedExecutorContextTestAccess {
public:
    static AuthenticatedExecutorContext issue(ProcessPrincipal principal) {
        return ExecutorAuthenticator::issue_for_testing(principal);
    }
};

}  // namespace recovery

#endif
