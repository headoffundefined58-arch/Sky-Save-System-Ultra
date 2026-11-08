#pragma once

// -----------------------------------------------------------------------------
// StateStoreEngine - the engine-facing half of the new state system.
//
// StateStore.h/cpp are deliberately engine-free so the entire save/load
// lifecycle can be compiled and proven in a console program.  This header
// adds the few operations that genuinely need RE/SKSE types, in the SAME
// namespace, so the rest of the plugin sees one coherent StateStore.
//
// Vault.h is now a thin alias over this namespace.  Vault.cpp is no longer
// part of the build: the old "any key, persisted forever" implementation is
// gone, not wrapped.
// -----------------------------------------------------------------------------

#include "StateStore.h"

#include <string_view>

namespace RE
{
	class TESForm;
}

namespace SKSE
{
	class SerializationInterface;
}

namespace S3U::StateStore
{
	// Call once from SKSEPlugin_Load.  Creates the state directory, installs
	// the real derive provider, and writes the human-readable classification
	// table so the player can inspect the retention rules at any time.
	void InitializeEngine();

	// Co-save writer.  One record, one format, one authoritative state block.
	void OnSave(SKSE::SerializationInterface* a_intfc);

	// Compatibility for the old Vault::SetForm / GetForm calls.  Under the new
	// classification player/* fields are kDerive: SetForm updates only the
	// in-memory view (it can never reach disk), and GetForm resolves against
	// the live engine rather than a stored, possibly dangling, FormID.
	bool         SetForm(std::string_view a_key, const RE::TESForm* a_form);
	RE::TESForm* GetForm(std::string_view a_key);
}
