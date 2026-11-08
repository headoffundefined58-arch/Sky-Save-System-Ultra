#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace S3U
{
	// -------------------------------------------------------------------------
	// Migrate - "this mod needs a new game" without a new game.
	//
	// A mod that needs a fresh start usually needs one specific thing: a
	// Start-Game-Enabled quest that has never run, or a starting item/spell/
	// perk the player never received.  Both are ordinary game state, and both
	// can be produced at load time.  What cannot be inferred is *which* of the
	// eighty quests in a load order is the one that matters - so this runs only
	// what a manifest tells it to run, and nothing else.
	//
	// Two independent mechanisms:
	//
	//   1. Automatic detection.  A plugin that is loaded now but was absent
	//      when the save was written has, by definition, never executed a
	//      single line.  Its Start-Game-Enabled quests are therefore guaranteed
	//      not to be running.  That is a precise signal, not a guess, and it is
	//      what produces the candidate list in the report.
	//
	//   2. Declarative manifests.  Data\SKSE\Plugins\3SU\Migrations\
	//      <name>.ini lists the exact commands to apply.  This is the part that
	//      can actually fix a mod, because only the mod's author knows whether
	//      it wants a quest, an item, or a global.
	//
	// Every application is recorded in the Vault under migrate/<name>/v, so a
	// quest is started once and not once per load.  The record commits with the
	// next save, which is the correct scope: an unsaved session has not
	// happened yet.
	// -------------------------------------------------------------------------
	namespace Migrate
	{
		struct Result
		{
			std::uint32_t manifests{ 0 };
			std::uint32_t applied{ 0 };
			std::uint32_t skipped{ 0 };
			std::uint32_t failed{ 0 };
			std::uint32_t candidates{ 0 };
			std::uint32_t autoStarted{ 0 };
			std::vector<std::string> lines;   // per-command outcome, capped
		};

		const Result& Last();

		// Main thread, after the world is live and the engine's own alias work
		// has settled.  Never called during the save path.
		void RunOnLoad();

		void WriteReportSection(std::ostream& a_out);
	}
}
