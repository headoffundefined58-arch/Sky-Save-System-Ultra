#pragma once

#include <cstdint>
#include <vector>

namespace SKSE
{
	class SerializationInterface;
}

namespace S3U
{
	// -------------------------------------------------------------------------
	// Save-over-save trend tracking.
	//
	// Every save appends one tiny record to the SKSE co-save; every load reads
	// the whole (bounded) history back.  Comparing the newest entry with the one
	// before it is what turns "my game got slower" into a number.
	//
	// This is the only thing the plugin writes anywhere, and it writes it into
	// its own co-save block: no game state, no save-file bytes, nothing that can
	// break a save if the plugin is later removed.
	// -------------------------------------------------------------------------
	namespace Trend
	{
		struct Entry
		{
			std::uint32_t saveIndex{ 0 };
			std::uint32_t gameDays{ 0 };
			std::uint64_t totalForms{ 0 };
			std::uint64_t scriptInstances{ 0 };
			std::uint64_t refs{ 0 };
			std::uint64_t deletedRefs{ 0 };
		};

		// Must be called on the main thread.  kSaveGame is dispatched there,
		// before the save is serialised, so the numbers we cache are consistent
		// and the co-save callback (which runs on the save thread) only has to
		// memcpy them.
		void CaptureOnMainThread();

		// As of 0.9.0 the trend history is a field of StateStore, not an
		// independent co-save record.  These two pure-byte functions let the
		// store own it: there is exactly one authoritative state block.
		// EncodeHistoryBlob also advances the history with the pending capture.
		std::string EncodeHistoryBlob();
		bool        RestoreHistoryBlob(const void* a_data, std::size_t a_len);

		void OnSave(SKSE::SerializationInterface* a_intfc);
		void OnRevert(SKSE::SerializationInterface* a_intfc);

		// See Vault::RecordType: one module owns the record walk, everybody
		// else receives their own block as bytes.
		std::uint32_t RecordType();
		void OnLoadRecord(const void* a_data, std::size_t a_len);

		// Logs a [GROWTH] warning when the newest entry is materially larger
		// than the previous one.  No-op when there is no history yet.
		void CompareAndReport();

		const std::vector<Entry>& History();
	}
}
