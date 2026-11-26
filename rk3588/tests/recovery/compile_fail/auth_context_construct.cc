// EXPECT-DIAG: private
#include "recovery/auth.hpp"

recovery::AuthenticatedExecutorContext forbidden() {
    return {recovery::ExecutorPrincipal{recovery::ProcessPrincipal{{1, 2}}},
            recovery::ChannelId{1}, 1, {}};
}
