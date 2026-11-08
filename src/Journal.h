#pragma once

// -----------------------------------------------------------------------------
// Journal - append-only, hash-chained transaction log for load-time state.
//
// This replaces "write the whole state over the previous state" with the model
// databases use: a snapshot plus an ordered log of facts.  The reason it
// matters here is that an initialisation performed during a load must be
// durable *before* the player saves.  If the receipt lived only in the Vault
// (which commits on save), then load -> init -> quit without saving -> reload
// would run the initialisation a second time, and additem is not idempotent.
//
// Each record is:
//   magic, format, seq, stamp, kind, prevDigest, payloadLen, payload, digest
//
// digest is FNV-1a/64 over every byte of the record *except* the digest field
// itself.  prevDigest links to the preceding record, so a torn write, a
// dropped record or a flipped bit anywhere in the log is detected, and replay
// stops at the last intact record and truncates the tail.
//
// FNV-1a is not a cryptographic MAC.  It is here to detect interrupted writes
// and bit rot, not an adversary; a mod manager is not an attacker.
//
// No engine, SKSE or filesystem-manager dependencies: tools/journal_test.cpp
// compiles this exact header and exercises it without Skyrim.
// -----------------------------------------------------------------------------

#include "Codec.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace S3U::Journal
{
	inline constexpr std::uint32_t kMagic  = 0x334A5333u;  // "3JS3" on disk
	inline constexpr std::uint16_t kFormat = 1;
	inline constexpr std::uint32_t kMaxPayload = 1u << 20;

	enum class Kind : std::uint32_t
	{
		kInitPending = 1,  // written BEFORE touching the world
		kInitApplied = 2,  // written after all commands succeeded
		kInitFailed  = 3,  // written after a partial/failed attempt
		kSchema      = 4,  // schema version upgrade applied
		kCommit      = 5,  // ordinary save-side commit marker
		kRollback    = 6
	};

	enum class InitState : std::uint8_t
	{
		kNone,         // never attempted
		kPending,      // a crash happened mid-initialisation: do NOT retry
		kApplied,      // done, never run again
		kFailed        // attempted and failed, needs an explicit retry
	};

	struct Record
	{
		std::uint64_t seq{ 0 };
		std::uint64_t stamp{ 0 };
		Kind          kind{ Kind::kCommit };
		std::uint64_t prev{ 0 };
		std::string   payload;
		std::uint64_t digest{ 0 };
	};

	struct Replay
	{
		std::vector<Record> records;
		std::uint64_t       lastSeq{ 0 };
		std::uint64_t       lastDigest{ 0 };
		std::size_t         goodBytes{ 0 };
		bool                repaired{ false };
		std::string         note;
	};

	using Receipts = std::unordered_map<std::string, InitState>;

	inline std::uint64_t Digest(const void* a_data, std::size_t a_len) noexcept
	{
		constexpr std::uint64_t kOffset = 14695981039346656037ull;
		constexpr std::uint64_t kPrime  = 1099511628211ull;
		auto                    hash   = kOffset;
		const auto*             p      = static_cast<const std::uint8_t*>(a_data);
		for (std::size_t i = 0; i < a_len; ++i) {
			hash ^= p[i];
			hash *= kPrime;
		}
		return hash;
	}

	inline std::uint64_t NowSeconds() noexcept
	{
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
	}

	inline std::vector<std::uint8_t> Encode(const Record& a_record)
	{
		Codec::Writer w;
		w.U32(kMagic);
		w.U16(kFormat);
		w.U16(0);
		w.U64(a_record.seq);
		w.U64(a_record.stamp);
		w.U32(static_cast<std::uint32_t>(a_record.kind));
		w.U64(a_record.prev);
		const auto len = static_cast<std::uint32_t>(
			std::min<std::size_t>(a_record.payload.size(), kMaxPayload));
		w.U32(len);
		w.Raw(a_record.payload.data(), len);

		const auto body = w.Data();
		w.U64(Digest(body.data(), body.size()));
		return w.Data();
	}

	// Returns false for anything incomplete or damaged.  On success `a_consumed`
	// is the exact size of this record, so replay can advance byte by byte.
	inline bool DecodeOne(const std::uint8_t* a_data, std::size_t a_len,
		Record& a_out, std::size_t& a_consumed)
	{
		Codec::Reader r(a_data, a_len);
		if (r.U32() != kMagic || r.U16() != kFormat) {
			return false;
		}
		r.U16();  // reserved

		Record rec;
		rec.seq   = r.U64();
		rec.stamp = r.U64();
		rec.kind  = static_cast<Kind>(r.U32());
		rec.prev  = r.U64();
		const auto len = r.U32();
		if (!r.Ok() || len > kMaxPayload) {
			return false;
		}

		std::string payload(len, '\0');
		if (!r.Raw(payload.data(), len)) {
			return false;
		}
		const auto bodyLen = r.Cursor();
		const auto digest  = r.U64();
		if (!r.Ok()) {
			return false;
		}
		if (Digest(a_data, bodyLen) != digest) {
			return false;
		}

		rec.payload = std::move(payload);
		rec.digest  = digest;
		a_consumed  = r.Cursor();
		a_out       = std::move(rec);
		return true;
	}

	// Payload is a stable, line-oriented key=value format.  It stays readable
	// in a text editor because a log a human cannot inspect is a log nobody
	// will trust when something goes wrong.
	inline std::string MakePayload(
		std::initializer_list<std::pair<std::string_view, std::string_view>> a_fields)
	{
		std::string out;
		for (const auto& [key, value] : a_fields) {
			out.append(key);
			out.push_back('=');
			out.append(value);
			out.push_back('\n');
		}
		return out;
	}

	inline std::string Field(std::string_view a_payload, std::string_view a_key)
	{
		std::size_t start = 0;
		while (start < a_payload.size()) {
			auto end = a_payload.find('\n', start);
			if (end == std::string_view::npos) {
				end = a_payload.size();
			}
			const auto line = a_payload.substr(start, end - start);
			start = end + 1;
			const auto eq = line.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			if (line.substr(0, eq) == a_key) {
				return std::string{ line.substr(eq + 1) };
			}
		}
		return {};
	}

	// Fold the log into "what is the initialisation state of each plugin".
	// Later records win, except that a Pending with no following terminal
	// record stays Pending - that is the whole point of a write-ahead receipt.
	inline Receipts DeriveInitStates(const std::vector<Record>& a_records)
	{
		Receipts out;
		for (const auto& rec : a_records) {
			const auto plugin = Field(rec.payload, "plugin");
			if (plugin.empty()) {
				continue;
			}
			switch (rec.kind) {
			case Kind::kInitPending:
				out[plugin] = InitState::kPending;
				break;
			case Kind::kInitApplied:
				out[plugin] = InitState::kApplied;
				break;
			case Kind::kInitFailed:
				out[plugin] = InitState::kFailed;
				break;
			case Kind::kRollback:
				out[plugin] = InitState::kNone;
				break;
			default:
				break;
			}
		}
		return out;
	}

	// Per-save view of the same log.
	//
	// A receipt is a statement about ONE .ess: "in this save, that plugin's
	// startup quest has already been started".  Applying it to a different
	// save of the same character is how the "already initialised" lie got
	// told: the older save never ran it and never will, because the receipt
	// said there was nothing to do.
	//
	// Records written before 1.0.0 carry no save binding at all.  Their
	// verdict is graded rather than trusted blindly:
	//
	//   pending / failed  -> still honoured.  Ignoring a crashed transaction
	//                        is how additem gets paid out twice.
	//   applied           -> ignored and counted.  The authoritative receipt
	//                        for an already-initialised save is now carried by
	//                        the co-save inside that .ess; if this .ess does
	//                        not carry one, the plugin genuinely never ran.
	struct ReceiptView
	{
		Receipts    states;
		std::size_t bound{ 0 };      // records belonging to this save
		std::size_t foreign{ 0 };    // records belonging to another save
		std::size_t legacy{ 0 };     // records with no save binding
		std::size_t legacyIgnored{ 0 };  // ...of which applied receipts were dropped
	};

	inline ReceiptView DeriveInitStatesForSave(const std::vector<Record>& a_records,
		std::string_view a_save)
	{
		ReceiptView out;
		for (const auto& rec : a_records) {
			const auto plugin = Field(rec.payload, "plugin");
			if (plugin.empty()) {
				continue;
			}
			const auto save = Field(rec.payload, "save");
			if (save.empty()) {
				++out.legacy;
				if (rec.kind == Kind::kInitApplied) {
					++out.legacyIgnored;
					continue;
				}
			} else if (a_save.empty() || save != a_save) {
				++out.foreign;
				continue;
			} else {
				++out.bound;
			}

			switch (rec.kind) {
			case Kind::kInitPending:
				out.states[plugin] = InitState::kPending;
				break;
			case Kind::kInitApplied:
				out.states[plugin] = InitState::kApplied;
				break;
			case Kind::kInitFailed:
				out.states[plugin] = InitState::kFailed;
				break;
			case Kind::kRollback:
				// A rolled-back transaction is not a verdict: the plugin is
				// retryable, and the log has to say so explicitly, otherwise
				// the preceding kInitPending would keep it blocked forever.
				out.states[plugin] = InitState::kNone;
				break;
			default:
				break;
			}
		}
		return out;
	}

	class Log
	{
	public:
		explicit Log(std::filesystem::path a_file) :
			m_file(std::move(a_file))
		{}

		const std::filesystem::path& Path() const noexcept { return m_file; }
		const Replay&                State() const noexcept { return m_state; }
		[[nodiscard]] bool           Ready() const noexcept { return m_opened; }

		// Read the whole log, verify the chain, and repair a torn tail in place.
		// A broken middle record truncates everything after it: losing the most
		// recent facts is recoverable, silently trusting a forged sequence is not.
		Replay& Open()
		{
			m_state  = Replay{};
			m_opened = true;

			std::error_code ec;
			std::filesystem::create_directories(m_file.parent_path(), ec);
			if (ec) {
				m_state.note = "cannot create journal directory: " + ec.message();
				return m_state;
			}

			std::ifstream in(m_file, std::ios::binary);
			if (!in.is_open()) {
				return m_state;  // a fresh log is normal
			}
			std::vector<std::uint8_t> bytes(
				(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

			std::size_t at = 0;
			while (at < bytes.size()) {
				Record     rec;
				std::size_t used = 0;
				if (!DecodeOne(bytes.data() + at, bytes.size() - at, rec, used) || used == 0) {
					break;
				}
				if (m_state.records.empty()) {
					if (rec.prev != 0) {
						break;
					}
				} else if (rec.seq != m_state.lastSeq + 1 || rec.prev != m_state.lastDigest) {
					break;
				}

				m_state.records.push_back(rec);
				m_state.lastSeq    = rec.seq;
				m_state.lastDigest = rec.digest;
				at += used;
			}

			m_state.goodBytes = at;
			if (at != bytes.size()) {
				m_state.repaired = true;
				m_state.note = "日志尾部损坏，已截断 " +
				               std::to_string(bytes.size() - at) + " 字节；保留 " +
				               std::to_string(m_state.records.size()) + " 条完好记录";

				// Truncate rather than delete: every intact record stays.
				std::filesystem::resize_file(m_file, static_cast<std::uintmax_t>(at), ec);
				if (ec) {
					m_state.note += "（截断失败: " + ec.message() + "）";
				}
			}
			return m_state;
		}

		// Append one fact and flush it to disk immediately.  A receipt that is
		// still sitting in a user-space buffer when the game crashes is not a
		// receipt.
		bool Append(Kind a_kind, std::string_view a_payload, std::string& a_note)
		{
			if (!m_opened && Open().note.find("cannot create") != std::string::npos) {
				a_note = m_state.note;
				return false;
			}

			Record rec;
			rec.seq     = m_state.lastSeq + 1;
			rec.stamp   = NowSeconds();
			rec.kind    = a_kind;
			rec.prev    = m_state.lastDigest;
			rec.payload = std::string{ a_payload };
			const auto bytes = Encode(rec);
			rec.digest = Digest(bytes.data(), bytes.size() - 8);

			std::ofstream out(m_file, std::ios::binary | std::ios::app);
			if (!out.is_open()) {
				a_note = "cannot open journal for append";
				return false;
			}
			out.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			out.flush();
			if (!out) {
				a_note = "short write to journal";
				return false;
			}

			m_state.records.push_back(rec);
			m_state.lastSeq    = rec.seq;
			m_state.lastDigest = rec.digest;
			m_state.goodBytes += bytes.size();
			return true;
		}

		Receipts InitStates() const { return DeriveInitStates(m_state.records); }

		// Bounded growth.  Every load appends a pending and a terminal record
		// per plugin, so a character played for months accumulates a log no
		// one will ever read.  Compaction keeps exactly one record per
		// (save, plugin) - the newest - which is precisely the set that
		// DeriveInitStatesForSave computes a verdict from.  A pending record
		// is the newest record for its key by definition, so an interrupted
		// transaction can never be compacted away into a false "applied".
		bool Compact(std::size_t a_threshold, std::string& a_note)
		{
			if (m_state.records.size() <= a_threshold) {
				return true;
			}

			std::map<std::string, std::size_t> newest;
			for (std::size_t i = 0; i < m_state.records.size(); ++i) {
				const auto& rec    = m_state.records[i];
				const auto  plugin = Field(rec.payload, "plugin");
				if (plugin.empty()) {
					continue;
				}
				newest[Field(rec.payload, "save") + "\x1f" + plugin] = i;
			}
			if (newest.empty()) {
				return true;
			}

			std::vector<std::size_t> order;
			order.reserve(newest.size());
			for (const auto& [key, index] : newest) {
				order.push_back(index);
			}
			std::sort(order.begin(), order.end());

			std::vector<std::uint8_t> bytes;
			std::uint64_t             prev = 0;
			std::uint64_t             seq  = 1;
			for (const auto index : order) {
				Record rec = m_state.records[index];
				rec.seq    = seq++;
				rec.prev   = prev;
				auto encoded = Encode(rec);
				rec.digest   = Digest(encoded.data(), encoded.size() - 8);
				prev         = rec.digest;
				bytes.insert(bytes.end(), encoded.begin(), encoded.end());
			}

			auto temp = m_file;
			temp += ".compact";
			{
				std::ofstream out(temp, std::ios::binary | std::ios::trunc);
				if (!out.is_open()) {
					a_note = "cannot open compaction target";
					return false;
				}
				out.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
				out.flush();
				if (!out) {
					a_note = "short write while compacting";
					out.close();
					std::error_code ec;
					std::filesystem::remove(temp, ec);
					return false;
				}
			}

			std::error_code ec;
			std::filesystem::rename(temp, m_file, ec);
			if (ec) {
				a_note = "cannot replace journal during compaction: " + ec.message();
				std::filesystem::remove(temp, ec);
				return false;
			}

			// Re-read what actually landed rather than trusting the buffer:
			// a compaction that silently loses a pending receipt would be a
			// worse bug than the unbounded growth it fixes.
			const auto before = m_state.records.size();
			m_state = Replay{};
			m_opened = false;
			Open();
			if (m_state.records.size() != order.size()) {
				a_note = "compaction read back " + std::to_string(m_state.records.size()) +
					" of " + std::to_string(order.size()) + " records";
				return false;
			}
			a_note = "log compacted: " + std::to_string(before) + " -> " +
				std::to_string(m_state.records.size()) + " records";
			return true;
		}

	private:
		std::filesystem::path m_file;
		Replay                m_state;
		bool                  m_opened{ false };
	};
}
