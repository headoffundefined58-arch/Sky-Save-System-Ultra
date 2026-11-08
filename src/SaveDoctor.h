#pragma once
// -----------------------------------------------------------------------------
// SaveDoctor - the DLL-side coordinator for the automatic file-level repair.
//
// The engine refuses to load a save whose plugin table references plugins
// that are no longer installed, and it aborts inside BGSSaveLoadManager long
// before any post-load callback exists.  No in-memory repair can ever run.
//
// So the DLL does this, entirely BEFORE the engine parses the .ess:
//
//   1. detect  - at kPreLoadGame, decompress the .ess body and read its
//                plugin table; compare against the engine's real load order
//                (RE::TESDataHandler).  If names are missing, this save is the
//                "references removed mods" corruption we can fix.
//   2. launch  - write a contract file <saveDir>/SSS-Ultra.fix (save path +
//                the current load order) and silently start
//                SSS-Ultra-SaveDoctor.exe with CREATE_NO_WINDOW.  The exe
//                repairs the file in place and writes
//                <saveDir>/SSS-Ultra.fix.status.
//   3. reload  - poll the status file on the game thread; on "OK", call
//                BGSSaveLoadManager::Load() to reload the now-healthy save
//                and continue into the world.  On "FAIL"/timeout, leave the
//                engine to its own "save corrupted" message.
//
// The detection is read-only and side-effect free until a broken save is
// confirmed: a healthy save is never touched, and the exe is never launched
// for one.  The game startup flow is never blocked - everything is fire-and-
// forget with a bounded poll.
//
// No engine calls on the LZ4/parse path (doctor/ess.hpp is pure C++), so the
// detection cannot deadlock the load.
// -----------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace S3U::SaveDoctor
{
	// Result of a pre-load probe of one .ess.
	struct Probe
	{
		bool                   parsed{ false };   // decompress + table read OK
		std::size_t           plugins{ 0 };
		std::vector<std::string> missing;         // names absent from the load order
		std::string           note;
	};

	// Read-only probe: decompress the .ess body and diff its plugin table
	// against the engine's current load order.  Never writes, never launches.
	Probe Inspect(const std::filesystem::path& a_saveFile);

	// Called from kPreLoadGame with the engine's own file name.  Returns true
	// when a broken save was confirmed and the repair was dispatched (the exe
	// is running and the caller should begin polling PollReload()).
	bool OnPreLoadGame(const std::filesystem::path& a_saveFile,
		const std::filesystem::path& a_saveDir);

	// Called once at kDataLoaded, BEFORE the player opens the load menu.
	// Scans every candidate save directory and silently launches the exe in
	// preflight mode to repair all broken saves up-front, so the engine's
	// "missing content" confirmation never pops.  Fire-and-forget: the exe
	// finishes and exits on its own; the DLL does not poll or reload here.
	void Preflight();

	// Poll the exe's status file.  Returns:
	//   kRunning  - still working (or not started yet)
	//   kReady    - repaired, call ReloadNow()
	//   kDone     - nothing to repair / already fixed (reload the same save)
	//   kFailed   - repair failed; report and bail
	enum class PollState { kRunning, kReady, kDone, kFailed };
	PollState PollReload();

	// Trigger an in-engine reload of the repaired save.
	void ReloadNow();

	// Reset per-load state (called at the start of each pre-load).
	void Reset();
}
