#include "PCH.h"

#include "Trend.h"
#include "Audit.h"
#include "Codec.h"
#include "Ini.h"

namespace S3U
{
	namespace Trend
	{
		namespace
		{
			// The record type within the co-save.  The co-save's unique id
			// ('S3SU') is set once per plugin in main.cpp; every record this
			// plugin writes carries a per-module type so the shared load
			// callback can route each block to its owner.
			constexpr std::uint32_t kRecordType = 'TRND';
			constexpr std::uint32_t kVersion    = 2;
			constexpr std::size_t   kMaxHistory = 32;

			std::vector<Entry> s_history;
			Entry              s_pending;
			bool               s_pendingValid = false;
			Entry              s_lastCaptured;
			bool               s_lastCapturedValid = false;

			Entry MakeEntry(std::uint32_t a_saveIndex)
			{
				Entry entry;
				entry.saveIndex = a_saveIndex;

				if (auto* calendar = RE::Calendar::GetSingleton()) {
					entry.gameDays = static_cast<std::uint32_t>(calendar->GetDaysPassed());
				}

				if (auto* dh = RE::TESDataHandler::GetSingleton()) {
					constexpr auto kMax = std::to_underlying(RE::FormType::Max);
					for (std::size_t i = 0; i < kMax; ++i) {
						entry.totalForms += dh->formArrays[i].size();
					}
				}

				if (auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton()) {
					// size() is a single 32-bit read; safe enough on the main
					// thread and never mutated mid-frame by anything we own.
					entry.scriptInstances = vm->attachedScripts.size();
				}

				// Reference counts cost a full cell walk, so they come from the
				// post-load audit instead of being recomputed here.
				if (Audit::HasResult()) {
					const auto& report = Audit::Last();
					entry.refs        = report.refs;
					entry.deletedRefs = report.deletedRefs;
				}

				return entry;
			}
		}

		void CaptureOnMainThread()
		{
			const auto index = s_history.empty() ? 1u : s_history.back().saveIndex + 1u;
			s_pending        = MakeEntry(index);
			s_pendingValid   = true;

			spdlog::debug("[3SU] captured before save #{}: forms={} instances={} refs={}",
				s_pending.saveIndex, s_pending.totalForms, s_pending.scriptInstances, s_pending.refs);
		}

		std::string EncodeHistoryBlob()
		{
			if (!s_pendingValid) {
				// No kSaveGame was seen (or the plugin loaded late): fall back to
				// whatever we last measured rather than writing a bogus zero row.
				s_pending      = MakeEntry(s_history.empty() ? 1u : s_history.back().saveIndex + 1u);
				s_pendingValid = true;
			}

			if (s_history.empty() || s_history.back().saveIndex != s_pending.saveIndex) {
				s_history.push_back(s_pending);
				if (s_history.size() > kMaxHistory) {
					s_history.erase(s_history.begin());
				}
			} else {
				s_history.back() = s_pending;
			}

			s_lastCaptured      = s_pending;
			s_lastCapturedValid = true;
			s_pendingValid      = false;

			// Serialise the history into one self-describing, checksummed block
			// instead of a raw struct memcpy.  A raw struct makes the co-save a
			// hostage to this exact build's struct layout: add a field, change
			// padding, or move to another compiler and every old save reads
			// garbage.  Field-by-field encoding with a version and a checksum
			// is what makes a damaged block a recoverable event rather than a
			// silent misinterpretation.
			Codec::Writer block;
			block.U32(kVersion);
			block.U32(static_cast<std::uint32_t>(s_history.size()));
			for (const auto& entry : s_history) {
				block.U32(entry.saveIndex);
				block.U32(entry.gameDays);
				block.U64(entry.totalForms);
				block.U64(entry.scriptInstances);
				block.U64(entry.refs);
				block.U64(entry.deletedRefs);
			}
			block.U32(Codec::Crc32(block.Data().data(), block.Size()));

			// 0.9.0: no independent co-save record.  These exact bytes become
			// StateStore's "trend/history" field, so a save has one
			// authoritative state block instead of two competing ones.
			spdlog::info("[3SU] trend history advanced: {} entries ({} bytes, carried by StateStore)",
				s_history.size(), block.Size());
			return std::string(
				reinterpret_cast<const char*>(block.Data().data()), block.Size());
		}

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			// Retained for API compatibility.  The caller in main.cpp no longer
			// invokes it: CollectState() asks for the blob directly.
			(void)a_intfc;
			EncodeHistoryBlob();
		}

		std::uint32_t RecordType()
		{
			return kRecordType;
		}

		bool RestoreHistoryBlob(const void* a_data, std::size_t a_len)
		{
			s_history.clear();

			if (!a_data || a_len < 16) {
				return false;
			}

			// A bound on the total number of entries we will ever trust, so a
			// corrupt length cannot make us allocate or loop unboundedly.
			Codec::Reader r(a_data, a_len);
			const auto     version = r.U32();
			const auto     count   = r.U32();
			if (version < 1 || version > kVersion) {
				spdlog::warn("[3SU] trend blob version {} unsupported - ignoring", version);
				return false;
			}
			if (count > kMaxHistory * 4) {
				spdlog::warn("[3SU] implausible trend history {} - ignoring", count);
				return false;
			}

			for (std::uint32_t i = 0; i < count; ++i) {
				Entry entry;
				entry.saveIndex      = r.U32();
				entry.gameDays       = r.U32();
				entry.totalForms     = r.U64();
				entry.scriptInstances = r.U64();
				entry.refs           = r.U64();
				entry.deletedRefs    = r.U64();
				if (!r.Ok()) {
					spdlog::warn("[3SU] trend blob truncated at entry {} - using {} read so far",
						i, s_history.size());
					break;
				}
				s_history.push_back(entry);
			}

			// The trailing checksum is advisory: it proves the blob is intact,
			// but entries already read under a version bound are kept so a
			// truncated tail costs the tail, not the whole history.
			if (a_len >= 4) {
				std::uint32_t stored = 0;
				std::memcpy(&stored,
					static_cast<const std::uint8_t*>(a_data) + a_len - 4, sizeof(stored));
				if (Codec::Crc32(a_data, a_len - 4) != stored) {
					spdlog::warn("[3SU] trend blob checksum mismatch (entries still trusted individually)");
				}
			}

			if (s_history.size() > kMaxHistory) {
				s_history.erase(s_history.begin(), s_history.end() - static_cast<std::ptrdiff_t>(kMaxHistory));
			}

			spdlog::info("[3SU] trend history restored: {} entries", s_history.size());
			return !s_history.empty();
		}

		void OnLoadRecord(const void* a_data, std::size_t a_len)
		{
			// Old saves may still carry the standalone TRND block.  Reading it
			// is harmless: if StateStore also has trend/history it wins below.
			RestoreHistoryBlob(a_data, a_len);
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			// Reverting (quit to main menu / new game) drops the world; keeping
			// stale numbers would poison the next trend comparison.
			s_history.clear();
			s_pendingValid      = false;
			s_lastCapturedValid = false;
		}

		void CompareAndReport()
		{
			if (s_history.size() < 2) {
				return;
			}

			const auto& previous = s_history[s_history.size() - 2];
			const auto& current  = s_history.back();

			const auto percent = [](std::uint64_t a_prev, std::uint64_t a_now) -> double {
				if (a_prev == 0) {
					return a_now == 0 ? 0.0 : 100.0;
				}
				return 100.0 * (static_cast<double>(a_now) - static_cast<double>(a_prev)) /
				       static_cast<double>(a_prev);
			};

			const double formsPct = percent(previous.totalForms, current.totalForms);
			const double refsPct  = percent(previous.refs, current.refs);
			const auto   threshold = static_cast<double>(std::max(1, Ini::Get().growthWarnPercent));

			spdlog::info("[3SU] trend #{}->#{}: forms {}->{} ({:+.1f}%), refs {}->{} ({:+.1f}%), deleted {}->{}",
				previous.saveIndex, current.saveIndex,
				previous.totalForms, current.totalForms, formsPct,
				previous.refs, current.refs, refsPct,
				previous.deletedRefs, current.deletedRefs);

			if (formsPct >= threshold || refsPct >= threshold) {
				spdlog::warn("[3SU] [GROWTH] 本次存档比上一次增长超过 {:.0f}% —— "
							 "forms {:+.1f}% / refs {:+.1f}%。若持续增长，说明有对象在被反复创建而不回收。",
					threshold, formsPct, refsPct);
				spdlog::warn("[3SU] [GROWTH] 上次: forms={} refs={} | 本次: forms={} refs={}",
					previous.totalForms, previous.refs, current.totalForms, current.refs);
			}
		}

		const std::vector<Entry>& History()
		{
			return s_history;
		}
	}
}
