// EXPECT-DIAG: no member named .active.|has no member named .active.
#include "recovery/runtime.hpp"

void forbidden(const recovery::RecoveryCoordinator& coordinator) {
    (void)coordinator.active();
}
