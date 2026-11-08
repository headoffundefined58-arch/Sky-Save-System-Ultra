#pragma once

// -----------------------------------------------------------------------------
// Vault.h is now a compatibility alias, nothing more.
//
// The old Vault.cpp implemented "any caller may persist any key forever".
// That implementation is no longer compiled (removed from CMakeLists) and its
// implicit retention rules are gone with it.  The authoritative state system
// is StateStore: a closed, classified schema with an explicit Create / Load /
// Update / Commit lifecycle and graded fault recovery.
//
// Keeping the alias means the consumer modules (LoadOrder, Migrate,
// InitEngine, main) need no textual change, while every one of their reads
// and writes is now arbitrated by StateSchema.h.  A key the schema classifies
// as derivable or ephemeral can be SET but can never reach a save.
//
// This header, together with StateSchema.h / StateStore.h / StateStore.cpp /
// StateStoreEngine.h / StateStoreEngine.cpp, is the drop-in replacement for
// the previous Vault.h + Vault.cpp pair.
// -----------------------------------------------------------------------------

#include "StateStoreEngine.h"
#include "FormRef.h"

namespace S3U
{
	namespace Vault = StateStore;
}
