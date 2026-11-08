#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

namespace S3U::PathScan
{
	// -------------------------------------------------------------------------
	// Finding a mod manager's per-profile save folder.
	//
	// This lives in its own translation unit with no engine and no settings
	// dependency, so tools/path_test.cpp can run it against the real disk and
	// prove it finds the folder.  The first version of this code shared one
	// std::error_code across a whole directory loop and gave up on the first
	// entry it could not read - and the first entries of any drive root
	// ($RECYCLE.BIN, System Volume Information) are exactly those.  The scan
	// returned nothing, the save was reported "not found", and the backup
	// feature silently never ran.
	// -------------------------------------------------------------------------

	// Receives one candidate: the directory, what found it ("instance",
	// "gameroot", ...), and the profile folder name (empty when the directory
	// is not a profile's saves folder).
	using Emit = std::function<void(const std::filesystem::path& dir,
		std::string_view                            kind,
		const std::filesystem::path&                profile)>;

	// <root>/Profiles/<profile>/saves and the same with a lowercase
	// "profiles".  Never aborts on an unreadable entry.
	void AddProfiles(const std::filesystem::path& a_root, Emit a_emit);

	// <root> itself when it holds ModOrganizer.ini, plus every direct child
	// that does.  Bounded: one stat per child, at most 400 children.
	void ScanInstance(const std::filesystem::path& a_root, Emit a_emit);
}
