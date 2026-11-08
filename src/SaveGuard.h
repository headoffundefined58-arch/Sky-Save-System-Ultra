#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace S3U
{
	// Paths end up in UTF-8 logs and reports, so they must not go through
	// path::string(): that converts to the ANSI code page and turns any Chinese
	// folder name ("Profiles\测试\saves") into mojibake.
	std::string PathToUtf8(const std::filesystem::path& a_path);

	// -------------------------------------------------------------------------
	// SaveGuard - the part of this plugin that actually protects a save.
	//
	// It cannot "repair" corruption: by the time a save is written, the damage
	// is already in the data.  What it *can* do is make corruption survivable:
	//
	//   1. before every save, copy the previous version of the file about to be
	//      overwritten aside (rotation)
	//   2. before every load, verify the file's structure (magic, version, size)
	//      and say so loudly - but only when the file was actually found
	//   3. keep the last N backups so a broken save never costs a playthrough
	//
	// Every operation is pure filesystem work on the .ess files themselves.
	// Nothing is written into the game world, the save format or the co-save.
	//
	// On directory resolution: a mod manager with per-profile saves puts the
	// .ess somewhere other than Documents\My Games\...\Saves, and its virtual
	// filesystem may hide the real directory from a directory listing while
	// still serving the file itself.  So the resolver never trusts "does this
	// directory exist" - it opens the actual save and reads its header.  A
	// path that yields a valid TESV header is a path that works.
	// -------------------------------------------------------------------------
	namespace Guard
	{
		struct Status
		{
			bool                  dirFound{ false };
			std::filesystem::path saveDir;    // where the save actually opened
			std::filesystem::path backupDir;

			// Every candidate directory that was probed.  Printed when
			// resolution fails, because that list is the only thing that makes
			// the failure actionable instead of mysterious.
			std::size_t candidatesTried{ 0 };
			std::string candidatesList;

			bool          verified{ false };
			bool          located{ false };   // the file itself was opened
			bool          ok{ true };
			std::string   note;
			std::filesystem::path lastFile;
			std::uintmax_t        lastSize{ 0 };
			std::string           locatedVia;  // which candidate matched

			std::uint32_t backupsKept{ 0 };
			bool          lastBackupOk{ false };
			std::string   lastBackupNote;
		};

		const Status& Last();

		// Builds the candidate list and creates the backup directory.  Safe to
		// call repeatedly; the work is only done once.
		bool Initialize();

		// Every candidate save directory that was discovered, in probe order.
		// Used by SaveDoctor's pre-flight pass to scan and repair broken saves
		// BEFORE the player opens the load menu (so the engine's "missing
		// content" confirmation never pops).  Empty until Initialize() ran.
		std::vector<std::filesystem::path> CandidateDirs();

		// a_fileName comes straight from SKSE's kPreLoadGame / kSaveGame and is
		// untrusted: it is validated before use and may be ignored entirely.
		void OnPreLoadGame(const char* a_fileName);
		void OnSaveGame(const char* a_fileName);

		// Conservative structural check: a save that merely looks unusual is
		// reported, never deleted or replaced.
		bool VerifyFile(const std::filesystem::path& a_file,
			std::string&                         a_note,
			std::uintmax_t&                      a_size);
	}
}
