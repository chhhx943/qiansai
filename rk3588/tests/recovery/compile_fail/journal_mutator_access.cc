// EXPECT-DIAG: private
#include "recovery/authorization.hpp"

auto forbidden = &recovery::AuthorizationJournal::install_entry_locked;
