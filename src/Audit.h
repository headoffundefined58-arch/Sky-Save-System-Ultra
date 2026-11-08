#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace S3U
{
	// -------------------------------------------------------------------------
	// A read-only snapshot of the currently loaded world.
	//
	// Nothing in here is ever written back into the save.  The whole point of
	// v0.1 is to *measure* the things that make a save heavy or fragile, so
	// that later (reversible) repair work can be justified with numbers
	// instead of folklore.
	// -------------------------------------------------------------------------
	struct Report
	{
		bool valid{ false };

		std::uint32_t pluginCount{ 0 };
		std::uint32_t lightPluginCount{ 0 };
		std::uint32_t esmCount{ 0 };

		std::uint64_t totalForms{ 0 };
		std::uint64_t questForms{ 0 };
		std::uint64_t npcForms{ 0 };

		// Papyrus
		std::uint64_t scriptedObjects{ 0 };     // VMHandles carrying at least one script
		std::uint64_t scriptInstances{ 0 };     // individual AttachedScript entries
		std::uint64_t uninitializedScripts{ 0 };

		// References (only cells that are currently attached are walked)
		std::uint64_t refs{ 0 };
		std::uint64_t deletedRefs{ 0 };         // kDeleted flag or marked for deletion
		std::uint64_t disabledRefs{ 0 };        // disabled but NOT deleted: the real leak
		std::uint64_t dynamicRefs{ 0 };         // runtime 0xFF______ objects
		std::uint64_t nullBaseRefs{ 0 };        // reference whose base form is gone
		std::uint32_t cellsScanned{ 0 };
		std::uint32_t cellsAttached{ 0 };

		std::uint64_t elapsedMs{ 0 };

		std::vector<std::pair<std::string, std::uint32_t>> topScripts;
		std::vector<std::pair<std::string, std::uint32_t>> typeCounts;
	};

	namespace Audit
	{
		// Runs the scan synchronously.  Safe to call only from the main thread
		// after a save has finished loading (the VM and the cell graphs are
		// quiescent at that point).
		bool          Run();

		const Report& Last();
		bool          HasResult();

		// Appends the diagnostic section to the SINGLE report-latest.txt.
		// v2.0.0: no timestamped log, no second txt.  The report is written by
		// main.cpp; this only contributes its own section.
		void          WriteSection(std::ostream& a_out, const Report& a_report);
	}
}
