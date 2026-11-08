#pragma once

// -----------------------------------------------------------------------------
// InitEngine - load-time initialisation as a first-class part of the save.
//
// Why a mod "requires a new game" is not mysterious: its startup quest never
// ran in this save, its aliases were never filled, its globals were never set.
// The engine only performs that work on New Game and never replays it.  So an
// old save carries the plugin but none of its state.
//
// This engine makes initialisation replayable.  On load it compares the load
// order against the baseline stored in the save, finds every plugin that was
// not present when the save was written, locates the Start-Game-Enabled quests
// that plugin owns, and starts them - at a safe point in the game loop, not
// inside the load event itself.
//
// Correctness comes from a four-step transaction per plugin, recorded in the
// write-ahead log of Journal.h:
//
//   1. PENDING is written and flushed BEFORE the first quest is touched.
//   2. the quests are started;
//   3. they are VERIFIED - a quest that returned true from Start() but is not
//      running afterwards is a failure, not a success;
//   4. on success APPLIED is written; on any failure the quests THIS
//      transaction started are stopped and reset, and ROLLBACK is written.
//
// Step 4 is what makes the transaction atomic from the world's point of view:
// without it a plugin with three startup quests that got two of them running
// would sit in a half-initialised state with no way back and no retry, because
// a "failed" receipt would block it forever.  A rollback record is explicitly
// NOT a verdict - the plugin becomes retryable on the next load.
//
// Every record carries the save it belongs to.  A receipt is a statement about
// one .ess: "in this save, that startup quest already ran".  Applying it to a
// different save of the same character would silently suppress the very
// initialisation that save still needs.
//
// A crash between (1) and (4) leaves PENDING, which is deliberately never
// retried automatically: replaying a half-finished init could hand out a
// second copy of a starting item.  The report names the plugin so the player
// can decide.  This receipt lives on disk, independent of the .ess, so
// "load -> init -> quit without saving -> reload" cannot double-apply either.
//
// This module is the state-changing counterpart to the read-only audit.  It
// does not modify the .ess bytes; it restores the missing game state through
// the engine's own quest startup path, which is exactly what New Game does.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace S3U::InitEngine
{
	struct Result
	{
		std::uint32_t newPlugins{ 0 };      // in the load order, absent from baseline
		std::uint32_t alreadyDone{ 0 };     // has an APPLIED receipt for THIS save
		std::uint32_t indeterminate{ 0 };   // has a PENDING receipt, not retried
		std::uint32_t previouslyFailed{ 0 };
		std::uint32_t units{ 0 };           // plugins scheduled for initialisation
		std::uint32_t questsFound{ 0 };
		std::uint32_t questsStarted{ 0 };
		std::uint32_t questsVerified{ 0 };  // still running after the start call
		std::uint32_t failures{ 0 };
		std::uint32_t rolledBack{ 0 };      // units compensated by Stop+Reset
		std::uint64_t generation{ 0 };      // init transactions committed so far
		std::size_t   receiptsBound{ 0 };   // journal records bound to this save
		std::size_t   receiptsForeign{ 0 }; // journal records of other saves
		std::size_t   receiptsUnbound{ 0 }; // pre-1.0.0 records without a save
		bool          journalReady{ false };
		bool          finished{ false };    // deferred task has actually run
		std::string   saveName;
		std::string   note;
		std::vector<std::string> lines;
	};

	const Result& Last();

	// Main thread, from TESLoadGameEvent.  Only *schedules* the world mutation;
	// a_onFinished runs once the deferred task completes, so reports written
	// from it show the real outcome instead of "scheduled".
	void PrepareOnLoad(std::function<void()> a_onFinished);

	// Main thread, on save.  Freezes this session's successful initialisations
	// into the Vault so they travel with the .ess, and writes a commit marker.
	void OnSave();

	void WriteReportSection(std::ostream& a_out);
}
