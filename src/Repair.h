#pragma once

// -----------------------------------------------------------------------------
// Repair - load-time self-healing for saves carrying data risks.
//
// The old builds stopped at "measure and warn".  This module performs the
// repair itself, in the first safe frame after TESLoadGameEvent:
//
//   1. detect   - missing plugins from the saved load-order baseline, and
//                 references in attached cells whose base object is gone
//   2. classify - RepairPolicy decides what may be touched; quest aliases,
//                 scripted objects, runtime forms and the player are never
//                 deleted, because a live handle pointing at a deleted ref
//                 is a guaranteed CTD
//   3. clean    - SetDelete(true) on safe dangling refs so the
//                 engine serialises them as deleted changeforms on next save;
//                 erase state receipts owned by missing plugins; append a
//                 rollback receipt to the write-ahead journal so a plugin
//                 that is re-installed later is initialised again
//   4. baseline - rewrite the load-order baseline to the CURRENT order, so the
//                 next load reports zero missing plugins instead of warning
//                 forever.  This is the "safe initialisation" step.
//   5. verify   - rescan dangling refs, re-read the baseline, confirm the
//                 state commit round-tripped
//   6. prompt   - only on verified success: an explicit message box plus HUD
//                 lines telling the player to SAVE then RELOAD.  The save is
//                 what persists the delete flags; the reload proves it.
//
// Nothing here writes .ess bytes directly.  That would require an offline
// save rewriter.  Everything is done through the engine's own reference and
// changeform machinery, which is why the save+reload instruction is required
// rather than decorative.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace S3U::Repair
{
	struct Result
	{
		bool        enabled{ true };
		bool        ran{ false };
		bool        finished{ false };
		bool        success{ false };
		bool        verified{ false };
		bool        promptShown{ false };

		std::size_t missingBefore{ 0 };       // plugins in baseline, gone now
		std::vector<std::string> missingNames;

		std::size_t danglingFound{ 0 };       // refs with a null base object
		std::size_t refsCleaned{ 0 };
		std::size_t refsSkipped{ 0 };
		std::size_t skippedAlias{ 0 };
		std::size_t skippedScripted{ 0 };
		std::size_t skippedRuntime{ 0 };
		std::size_t residualDangling{ 0 };    // after the pass

		std::size_t keysPurged{ 0 };          // state receipts erased
		std::size_t receiptsReset{ 0 };       // journal rollback records
		bool        baselineRewritten{ false };
		bool        baselineClean{ false };
		bool        stateCommitted{ false };

		std::size_t recordsRemoved{ 0 };      // historic txt/log/.bak cleaned
		std::string note;
		std::vector<std::string> lines;
	};

	// Queue the repair on SKSE's main-thread task queue.  a_onFinished runs
	// after the pass, so the report reflects what actually happened.
	void          PrepareOnLoad(std::function<void()> a_onFinished);
	const Result& Last();

	void          WriteReportSection(std::ostream& a_out);
	void          WriteActionLog(const std::filesystem::path& a_dir);

	// Called once at plugin startup: enforce the one-txt / one-log rule.
	std::size_t   PruneRecordsOnStartup();

	// Called on pre-load, once Guard knows where the .ess files live: sweep
	// the legacy 3SUBackup folder and re-enforce retention.
	void          SweepLegacyBackups();
}
