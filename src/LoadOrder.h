#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace S3U
{
	// -------------------------------------------------------------------------
	// LoadOrder - the shadow copy of "which plugins were installed when this
	// save was written".
	//
	// The engine's save header does contain a plugin table, but reading it back
	// out of the .ess means reverse-engineering a compressed structure that has
	// no published specification.  There is no reason to do that: at save time
	// we already know the exact load order, because we are running inside it.
	// Writing it into the Vault costs one string and turns "why did this save
	// break" into a set difference.
	//
	// Three lists come out of the comparison, and they mean different things:
	//
	//   missing  - installed when the save was written, gone now.
	//              This is the dangerous one: every record it owned is about to
	//              become a null pointer or, worse, somebody else's object.
	//   added    - installed now, absent when the save was written.
	//              Nothing it owns has ever run.  This is the "needs a new
	//              game" case, and it is fixable by initialization.
	//   shifted  - present in both, but its compile index moved.
	//              The engine remaps its own references by plugin name, so
	//              vanilla data survives this.  Anything that stored a *raw*
	//              FormID - a Papyrus int, another plugin's co-save, a json
	//              config - does not, and now points at whatever took the old
	//              number.  This list is therefore a warning about third-party
	//              storage, not about the engine.
	// -------------------------------------------------------------------------
	namespace LoadOrder
	{
		struct Mod
		{
			std::string   name;   // lower-case file name
			std::uint8_t  index{ 0 };
			bool          light{ false };
		};

		struct Diff
		{
			bool                     haveBaseline{ false };  // a saved list existed
			std::vector<std::string> missing;
			std::vector<std::string> added;
			std::vector<std::string> shifted;
			std::string              note;

			[[nodiscard]] bool Risky() const noexcept
			{
				return !missing.empty() || !shifted.empty();
			}
		};

		const Diff& Last();

		[[nodiscard]] std::vector<Mod> Snapshot();

		// Called on the main thread just before Vault::Commit, so the load order
		// that gets stored is the one this save is actually being written under.
		void StoreToVault();

		// Called right after Vault::OnLoad, when the in-memory vault holds the
		// list belonging to the save being read.
		void CompareWithVault();

		// Human-readable section, appended to modguard-latest.txt.
		void WriteReportSection(std::ostream& a_out);
	}
}
