#include "StateStore.h"

#include "Codec.h"
#include "Journal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace S3U::StateStore
{
	namespace
	{
		using State::Kind;
		using State::Policy;
		using State::Scope;
		using State::Storage;

		// ---------------------------------------------------------------------
		// Four disjoint views.  Their separation is the whole design: only
		// s_live is ever serialised.  A value in s_derived or s_ephemeral is
		// structurally incapable of reaching the disk, no matter who writes it.
		// ---------------------------------------------------------------------
		FieldMap               s_live;       // kPersist + kSnapshot
		FieldMap               s_wal;        // kPersist + kWal, replayed read-only
		FieldMap               s_derived;    // kDerive, recomputed each load
		FieldMap               s_ephemeral;  // kEphemeral, reset each load
		Status                 s_status;
		LoadReport             s_report;
		std::filesystem::path  s_dataDir;
		std::filesystem::path  s_snapshot;
		std::filesystem::path  s_walFile;
		DeriveFn               s_derive;
		std::uint64_t          s_tx{ 0 };
		std::size_t            s_rejected{ 0 };
		bool                   s_initialized{ false };

		// The .ess currently being worked on.  These two strings are the
		// authority for scope adjudication: they are taken from the save the
		// game is loading or writing, never from the state being read.
		std::string            s_character;
		std::string            s_saveName;

		std::string NowStamp()
		{
			const auto secs = Journal::NowSeconds();
			std::time_t t = static_cast<std::time_t>(secs);
			std::tm     tm{};
#ifdef _WIN32
			localtime_s(&tm, &t);
#else
			localtime_r(&t, &tm);
#endif
			char buffer[32]{};
			std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
			return buffer;
		}

		// "Save5_D46E04D7_0_..._1_1.ess" -> "D46E04D7".  Falls back to a
		// stable hash of the whole name so two characters never share a store.
		std::string DeriveIdentity(std::string_view a_name)
		{
			if (a_name.empty()) {
				return "default";
			}
			std::string_view stem{ a_name };
			if (const auto slash = stem.find_last_of("\\/"); slash != std::string_view::npos) {
				stem.remove_prefix(slash + 1);
			}
			auto first = stem.find('_');
			if (first != std::string_view::npos) {
				auto rest = stem.substr(first + 1);
				auto second = rest.find('_');
				if (second != std::string_view::npos) {
					auto candidate = rest.substr(0, second);
					if (candidate.size() == 8) {
						bool hex = true;
						for (const char c : candidate) {
							if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
							      (c >= 'a' && c <= 'f'))) {
								hex = false;
								break;
							}
						}
						if (hex) {
							return std::string{ candidate };
						}
					}
				}
			}
			char buffer[24]{};
			std::snprintf(buffer, sizeof(buffer), "%016llX",
				static_cast<unsigned long long>(Journal::Digest(stem.data(), stem.size())));
			return buffer;
		}

		std::filesystem::path WalPathFor(std::string_view a_identity)
		{
			// .wal, not .log: Records::Prune removes every historical .log in
			// the data directory, and a receipt is functional state - deleting
			// it would make load-time initialisation hand out starting items a
			// second time.  Keeping it out of the record namespace makes the
			// "one txt + one log" rule unambiguous.
			return s_dataDir / "Journal" / std::string{ a_identity } / "init.wal";
		}

		// One store per character.  The 0.9.0 build kept a single
		// State/state.bin for the whole installation, which meant the second
		// character you played inherited the first one's load-order baseline.
		std::filesystem::path SnapshotPathFor(std::string_view a_identity)
		{
			if (s_dataDir.empty()) {
				return {};
			}
			if (a_identity.empty()) {
				return s_dataDir / "state.bin";   // legacy single-file location
			}
			return s_dataDir / std::string{ a_identity } / "state.bin";
		}

		// The identity of the current save always wins over whatever a state
		// file claims.  Called after every Create/Load: without it a snapshot
		// written by another character would rewrite our own identity and the
		// next save would be labelled with the wrong name.
		void NormalizeIdentity()
		{
			if (s_character.empty()) {
				return;
			}
			s_live["identity/character"] = Value::Text(s_character);
			s_live["identity/saveName"] = Value::Text(s_saveName);
		}

		// A state that belongs to another character is not "slightly wrong
		// data" - it describes a different world.  Adopting it would attach
		// somebody else's baseline and receipts to this character, so the
		// whole source is refused and a fresh state is created instead.
		bool CheckOwner(const FieldMap& a_fields, LoadReport& a_report)
		{
			const auto it = a_fields.find("identity/character");
			if (it == a_fields.end() || it->second.s.empty() || s_character.empty()) {
				return true;
			}
			if (it->second.s == s_character) {
				return true;
			}
			a_report.Add(Fault::kForeignOwner,
				"状态属于角色 " + it->second.s + "，本次读入的是 " + s_character +
				"；整份状态不予继承");
			return false;
		}

		// Disk snapshots are a cache, and a cache can be stale by exactly one
		// save.  Save-scoped fields therefore survive only when the snapshot
		// says it was written for this very .ess.  Character-scoped history
		// (save count, trend) is still adopted: it is true for the character
		// regardless of which of their saves is being loaded.
		std::size_t DropForeignSaveFields(FieldMap& a_fields, LoadReport& a_report)
		{
			const auto it = a_fields.find("identity/saveName");
			const std::string stored = it == a_fields.end() ? std::string{} : it->second.s;
			const bool        same   = s_saveName.empty() || stored.empty() ||
			                    stored == s_saveName;
			a_report.sameSave = same;
			if (same) {
				return 0;
			}

			std::size_t dropped = 0;
			for (auto i = a_fields.begin(); i != a_fields.end();) {
				const auto* spec = State::Find(i->first);
				if (spec && spec->policy == Policy::kPersist && spec->scope == Scope::kSave) {
					a_report.events.push_back(
						"存档级字段属于另一份存档（" + (stored.empty() ? "未记录" : stored) +
						"），本次读入 " + s_saveName + "，已舍弃: " + i->first);
					i = a_fields.erase(i);
					++dropped;
				} else {
					++i;
				}
			}
			a_report.scopeDropped += dropped;
			if (dropped > 0) {
				a_report.Add(Fault::kForeignSave,
					"舍弃 " + std::to_string(dropped) +
					" 个存档级字段：它们描述的是另一份存档，继承会导致新增插件被误判为「一个都没加」");
			}
			return dropped;
		}

		// Writes bytes to a sibling temporary, verifies the bytes on disk, then
		// renames.  A failed rename removes ONLY the temporary - the previous
		// snapshot is never deleted first, so there is no window in which the
		// last good state is gone.
		bool WriteAtomic(const std::filesystem::path& a_target,
			const std::vector<std::uint8_t>& a_bytes, std::string& a_note)
		{
			std::error_code ec;
			std::filesystem::create_directories(a_target.parent_path(), ec);
			if (ec) {
				a_note = "无法创建状态目录: " + ec.message();
				return false;
			}

			auto temp = a_target;
			temp += ".tmp";
			{
				std::ofstream out(temp, std::ios::binary | std::ios::trunc);
				if (!out.is_open()) {
					a_note = "无法打开临时状态文件";
					return false;
				}
				out.write(reinterpret_cast<const char*>(a_bytes.data()),
					static_cast<std::streamsize>(a_bytes.size()));
				out.flush();
				if (!out) {
					a_note = "状态写入中断";
					out.close();
					std::filesystem::remove(temp, ec);
					return false;
				}
			}

			// File-level read-back: what the OS actually stored must match.
			{
				std::ifstream in(temp, std::ios::binary);
				std::vector<std::uint8_t> got(
					(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				if (got != a_bytes) {
					a_note = "落盘回读字节不一致";
					std::filesystem::remove(temp, ec);
					return false;
				}
			}

			std::filesystem::rename(temp, a_target, ec);
			if (ec) {
				a_note = "原子替换失败，已保留旧状态: " + ec.message();
				std::filesystem::remove(temp, ec);
				return false;
			}
			return true;
		}

		bool Clamp(const State::Spec& a_spec, Value& a_value, LoadReport& a_report,
			std::string_view a_key)
		{
			bool changed = false;
			if (a_spec.kind == Kind::kInt) {
				const auto bound = static_cast<std::int64_t>(kMaxGeneration);
				if (a_value.i > bound) { a_value.i = bound; changed = true; }
				if (a_value.i < -bound) { a_value.i = -bound; changed = true; }
			} else if (a_spec.kind == Kind::kFloat) {
				if (!std::isfinite(a_value.f)) { a_value.f = 0.0; changed = true; }
			}
			if (changed) {
				++a_report.clampedFields;
				a_report.Add(Fault::kClamped,
					"字段 " + std::string{ a_key } + " 的值越界，已钳制到安全范围");
			}
			return !changed;
		}

		void WriteValue(Codec::Writer& a_out, const Value& a_value)
		{
			switch (a_value.kind) {
			case Kind::kInt:
				a_out.U64(static_cast<std::uint64_t>(a_value.i));
				break;
			case Kind::kFloat:
				a_out.F64(a_value.f);
				break;
			case Kind::kBool:
				a_out.U8(a_value.b ? 1 : 0);
				break;
			case Kind::kString:
			case Kind::kBlob:
			{
				const auto len = static_cast<std::uint32_t>(
					std::min<std::size_t>(a_value.s.size(), kMaxValueBytes));
				a_out.U32(len);
				a_out.Raw(a_value.s.data(), len);
				break;
			}
			}
		}

		bool ReadValue(Codec::Reader& a_in, Kind a_kind, Value& a_out)
		{
			a_out.kind = a_kind;
			switch (a_kind) {
			case Kind::kInt:
				a_out.i = static_cast<std::int64_t>(a_in.U64());
				break;
			case Kind::kFloat:
				a_out.f = a_in.F64();
				break;
			case Kind::kBool:
				a_out.b = a_in.U8() != 0;
				break;
			case Kind::kString:
			case Kind::kBlob:
			{
				const auto len = a_in.U32();
				if (!a_in.Ok() || len > kMaxValueBytes) {
					return false;
				}
				std::string s(len, '\0');
				if (!a_in.Raw(s.data(), len)) {
					return false;
				}
				a_out.s = std::move(s);
				break;
			}
			default:
				return false;
			}
			return a_in.Ok();
		}

		// The one and only encoder.  Serialize() and Commit() both go through
		// it, so the bytes the co-save receives and the bytes the disk receives
		// are produced by identical code - there is no second format to drift.
		// Only kPersist+kSnapshot fields are eligible; the schema is consulted
		// again here even though Set() already filtered, because defence in
		// depth is cheap and a stray field is expensive.
		std::vector<std::uint8_t> BuildBytes(const FieldMap& a_fields,
			std::uint64_t a_tx, std::uint32_t& a_written)
		{
			Codec::Writer out;
			out.U32(kMagic);
			out.U16(kFormat);
			out.U16(0);
			out.U64(a_tx);
			out.U64(Journal::NowSeconds());
			constexpr std::size_t kCountOffset = 4 + 2 + 2 + 8 + 8;
			out.U32(0);  // record count, back-filled once known
			a_written = 0;

			for (const auto& [key, value] : a_fields) {
				const auto* spec = State::Find(key);
				if (!spec || spec->policy != Policy::kPersist ||
					spec->storage != Storage::kSnapshot) {
					continue;
				}
				Codec::Writer rec;
				rec.Str(key);
				rec.U8(static_cast<std::uint8_t>(value.kind));
				WriteValue(rec, value);
				const auto len = static_cast<std::uint32_t>(rec.Size());
				out.U32(len);
				out.Raw(rec.Data().data(), len);
				out.U32(Codec::Crc32(rec.Data().data(), len));
				++a_written;
			}

			out.OverwriteU32(kCountOffset, a_written);
			out.U32(Codec::Crc32(out.Data().data(), out.Size()));
			return out.Data();
		}

		// Rebuild the derived/ephemeral views from scratch.  Called at the end
		// of every Create and Load: there is no code path that can leave a
		// stale derived value behind.
		void RebuildViews(LoadReport& a_report, DeriveFn& a_derive)
		{
			s_derived.clear();
			s_ephemeral.clear();

			for (const auto& spec : State::kSpecs) {
				if (State::IsFamily(spec)) {
					continue;
				}
				Value v;
				v.kind = spec.kind;
				if (spec.policy == Policy::kDerive) {
					if (a_derive) {
						try {
							a_derive(spec.key, v);   // provider may decline; default stands
						} catch (...) {
							// A derive provider talks to the live engine.  One
							// bad probe must not abort the whole load.
							a_report.Add(Fault::kClamped,
								std::string{ "重算字段 " } + spec.key + " 失败，已取默认值");
						}
					}
					s_derived.emplace(spec.key, std::move(v));
					++a_report.derived;
				} else if (spec.policy == Policy::kEphemeral) {
					s_ephemeral.emplace(spec.key, std::move(v));  // declared default
					++a_report.reset;
				}
			}
		}

		void ReplayWal(LoadReport& a_report)
		{
			s_wal.clear();
			if (s_walFile.empty()) {
				return;
			}
			Journal::Log log{ s_walFile };
			auto& replay = log.Open();
			if (!replay.records.empty()) {
				// Receipts are statements about ONE .ess.  Reading them
				// without the save binding is what let an older save believe
				// it had already been initialised.
				const auto view = Journal::DeriveInitStatesForSave(replay.records, s_saveName);
				if (view.legacyIgnored > 0) {
					a_report.events.push_back(
						"忽略 " + std::to_string(view.legacyIgnored) +
						" 条无存档绑定的旧收据：旧版按角色记账，本次改按存档，交由存档内收据裁决");
				}
				for (const auto& [plugin, state] : view.states) {
					const char* text = "none";
					switch (state) {
					case Journal::InitState::kPending: text = "pending"; break;
					case Journal::InitState::kApplied: text = "applied"; break;
					case Journal::InitState::kFailed:  text = "failed";  break;
					default: break;
					}
					std::string key = "init/receipts/" + plugin;
					if (const auto* spec = State::Find(key);
						spec && spec->policy == Policy::kPersist &&
						spec->storage == Storage::kWal) {
						s_wal.emplace(std::move(key), Value::Text(text));
					}
				}
			}
			if (replay.repaired) {
				a_report.Add(Fault::kTruncated, replay.note);
			}
		}

		void ApplyStatus(const LoadReport& a_report, Status::Source a_source)
		{
			s_status.tx              = a_report.tx;
			s_status.entries         = s_live.size();
			s_status.source          = a_source;
			s_status.droppedRecords  = a_report.damagedFields;
			s_status.migrated        = a_report.discardedByPolicy;
			s_status.derivedFields   = a_report.derived;
			s_status.resetFields     = a_report.reset;
			s_status.discardedFields = a_report.discardedByPolicy + a_report.discardedUnknown;
			s_status.roundTripOk     = a_report.roundTripOk;
			s_status.formRefs        = 0;   // no FormIDs are persisted any more
			s_status.tombstoned      = 0;
			s_status.dir             = s_dataDir.string();
			s_status.saveName        = s_saveName;
			s_status.scopeDropped    = a_report.scopeDropped;
			s_status.foreignOwner    = a_report.faults.end() != std::find(
				a_report.faults.begin(), a_report.faults.end(), Fault::kForeignOwner);
			s_status.sameSave        = a_report.sameSave;
			s_status.note            = a_report.Summary();
		}
	}

	// -------------------------------------------------------------------------
	// Value
	// -------------------------------------------------------------------------
	Value Value::Int(std::int64_t a_v)
	{
		Value v; v.kind = Kind::kInt; v.i = a_v; return v;
	}
	Value Value::Float(double a_v)
	{
		Value v; v.kind = Kind::kFloat; v.f = a_v; return v;
	}
	Value Value::Bool(bool a_v)
	{
		Value v; v.kind = Kind::kBool; v.b = a_v; return v;
	}
	Value Value::Text(std::string_view a_v)
	{
		Value v; v.kind = Kind::kString; v.s.assign(a_v); return v;
	}
	Value Value::Blob(std::string_view a_v)
	{
		Value v; v.kind = Kind::kBlob; v.s.assign(a_v); return v;
	}

	bool Value::SameAs(const Value& a_other) const noexcept
	{
		if (kind != a_other.kind) { return false; }
		switch (kind) {
		case Kind::kInt:    return i == a_other.i;
		case Kind::kFloat:  return f == a_other.f;
		case Kind::kBool:   return b == a_other.b;
		case Kind::kString:
		case Kind::kBlob:   return s == a_other.s;
		default:            return false;
		}
	}

	std::string Value::Describe() const
	{
		std::ostringstream out;
		switch (kind) {
		case Kind::kInt:    out << i; break;
		case Kind::kFloat:  out << std::setprecision(9) << f; break;
		case Kind::kBool:   out << (b ? "true" : "false"); break;
		case Kind::kString: out << s; break;
		case Kind::kBlob:   out << "<" << s.size() << " 字节>"; break;
		default:            out << "?"; break;
		}
		return out.str();
	}

	// -------------------------------------------------------------------------
	// LoadReport
	// -------------------------------------------------------------------------
	void LoadReport::Add(Fault a_fault, std::string a_text)
	{
		if (a_fault != Fault::kNone) {
			faults.push_back(a_fault);
		}
		events.push_back(std::move(a_text));
	}

	bool LoadReport::Clean() const noexcept
	{
		return faults.empty();
	}

	std::string LoadReport::Summary() const
	{
		if (Clean()) {
			return "状态完整：持久化 " + std::to_string(persisted) +
			       " · 重算 " + std::to_string(derived) +
			       " · 重置 " + std::to_string(reset);
		}
		std::vector<Fault> unique = faults;
		std::sort(unique.begin(), unique.end());
		unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
		std::string out;
		for (const auto f : unique) {
			if (!out.empty()) { out += "；"; }
			out += FaultName(f);
		}
		return out;
	}

	// -------------------------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------------------------
	void Initialize(const std::filesystem::path& a_dataDir)
	{
		s_dataDir = a_dataDir;
		s_snapshot = s_dataDir / "state.bin";
		s_initialized = true;
	}

	void SetDeriveProvider(DeriveFn a_fn)
	{
		s_derive = std::move(a_fn);
	}

	const Status& Last() noexcept { return s_status; }
	const LoadReport& LastReport() noexcept { return s_report; }
	std::size_t RejectedKeys() noexcept { return s_rejected; }

	const std::string& CurrentSaveName()
	{
		static const std::string kNone;
		return s_saveName.empty() ? kNone : s_saveName;
	}

	const std::string& CurrentIdentity()
	{
		static const std::string kDefault{ "default" };
		return s_character.empty() ? kDefault : s_character;
	}

	std::filesystem::path CurrentWalPath()
	{
		return s_walFile;
	}

	std::vector<std::string> Keys(std::string_view a_prefix)
	{
		std::vector<std::string> out;
		for (const auto& [key, value] : s_live) {
			if (key.size() >= a_prefix.size() && key.compare(0, a_prefix.size(), a_prefix) == 0) {
				out.push_back(key);
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	LoadReport Create(std::string_view a_identity, DeriveFn a_derive)
	{
		LoadReport report;
		s_live.clear();
		s_wal.clear();
		s_tx = 0;

		s_live.emplace("schema/version", Value::Text(kPluginVersion));
		s_live.emplace("meta/version", Value::Text(kPluginVersion));
		s_live.emplace("identity/character", Value::Text(a_identity));
		s_live.emplace("identity/saveName", Value::Text(s_saveName));
		s_live.emplace("meta/firstSeen", Value::Text(NowStamp()));
		s_live.emplace("meta/saveCount", Value::Int(0));
		s_live.emplace("tx/generation", Value::Int(0));
		s_live.emplace("init/generation", Value::Int(0));

		report.Add(Fault::kMissing, "L0 无既有状态，已按分类表新建：" +
			std::to_string(s_live.size()) + " 个持久化字段取默认值");
		report.persisted = s_live.size();
		report.tx = 0;

		RebuildViews(report, a_derive);
		ReplayWal(report);
		report.authoritative = true;
		report.roundTripOk = true;
		return report;
	}

	void Reset()
	{
		DeriveFn fn = s_derive;
		s_report = Create("", std::move(fn));
		s_status = Status{};
		s_status.dir = s_dataDir.string();
		ApplyStatus(s_report, Status::Source::kNone);
	}

	void ObserveSaveName(const char* a_fileName)
	{
		std::string name = a_fileName ? a_fileName : "";
		// The message carries a bare file name, but a path can arrive from a
		// caller that resolved the save itself.  Only the name identifies the
		// save; a path would break the scope comparison on the next run.
		if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) {
			name.erase(0, slash + 1);
		}

		s_saveName  = name;
		s_character = DeriveIdentity(name);
		s_status.identity = s_character;
		s_status.saveName = s_saveName;
		s_walFile = WalPathFor(s_character);
		s_snapshot = SnapshotPathFor(s_character);

		// Disk is the provisional authority: it is the only source when the
		// .ess being loaded carries no co-save block.  It is loaded through
		// the scope filter, so a snapshot written for another save of this
		// character can no longer hand us its baseline, and a snapshot
		// written by another character is refused outright.
		if (s_initialized) {
			auto report = LoadFromDisk(s_derive);
			s_report = std::move(report);
			s_status.diskOk = s_report.authoritative;
			s_status.diskTx = s_report.tx;
			ApplyStatus(s_report, s_report.authoritative ?
				Status::Source::kDisk : Status::Source::kNone);
		} else {
			s_live["identity/character"] = Value::Text(s_character);
			s_live["identity/saveName"] = Value::Text(s_saveName);
		}

		// The identity of the CURRENT save is the authority, always.  Without
		// this a state file written by another character would relabel this
		// process and the next save would be filed under the wrong name.
		NormalizeIdentity();
		s_status.identity = s_character;
		s_status.saveName = s_saveName;
	}

	// -------------------------------------------------------------------------
	// Byte layer
	// -------------------------------------------------------------------------
	std::vector<std::uint8_t> Serialize()
	{
		std::uint32_t written = 0;
		return BuildBytes(s_live, s_tx, written);
	}

	bool Decode(const void* a_data, std::size_t a_len,
		FieldMap& a_out, std::uint64_t& a_tx, LoadReport& a_report)
	{
		a_out.clear();
		a_tx = 0;

		Codec::Reader r(a_data, a_len);
		const auto magic  = r.U32();
		const auto format = r.U16();
		r.U16();  // flags
		const auto tx     = r.U64();
		r.U64();  // stamp
		const auto count  = r.U32();
		if (!r.Ok()) {
			a_report.Add(Fault::kTruncated, "L3 状态头不完整，无法读取");
			return false;
		}
		if (magic != kMagic) {
			a_report.Add(Fault::kBadMagic, "L1 格式标识不匹配，已回退到磁盘快照");
			return false;
		}
		if (format > kFormat) {
			a_report.Add(Fault::kFutureVersion,
				"L2 状态由更新的版本写入（格式 " + std::to_string(format) +
				"），拒绝解释并原样保留文件");
			return false;
		}

		if (a_len < 4) {
			a_report.Add(Fault::kTruncated, "L3 缺少整体校验尾");
			return false;
		}
		const auto bodyLen = a_len - 4;
		const auto* tail = static_cast<const std::uint8_t*>(a_data) + bodyLen;
		const std::uint32_t storedTrailer =
			static_cast<std::uint32_t>(tail[0]) |
			(static_cast<std::uint32_t>(tail[1]) << 8) |
			(static_cast<std::uint32_t>(tail[2]) << 16) |
			(static_cast<std::uint32_t>(tail[3]) << 24);
		if (Codec::Crc32(a_data, bodyLen) != storedTrailer) {
			a_report.Add(Fault::kTruncated,
				"L3 整体校验失败，转入逐字段抢救（每条记录各有独立 CRC）");
		}

		// Header consumed 4+2+2+8+8+4 = 28 bytes; seek there explicitly so a
		// future header extension cannot desynchronise the record walk.
		r.Seek(28);
		bool torn = false;
		while (r.Ok() && r.Cursor() < bodyLen) {
			if (bodyLen - r.Cursor() < 8) { torn = true; break; }
			const auto recLen = r.U32();
			if (!r.Ok() || recLen < 3 || recLen > kMaxValueBytes ||
				recLen + 4 > bodyLen - r.Cursor()) {
				torn = true;
				break;
			}
			std::string payload(recLen, '\0');
			if (!r.Raw(payload.data(), recLen)) { torn = true; break; }
			const auto recCrc = r.U32();
			if (!r.Ok()) { torn = true; break; }

			if (Codec::Crc32(payload.data(), payload.size()) != recCrc) {
				++a_report.damagedFields;
				a_report.Add(Fault::kFieldDropped,
					"L4 一条记录的 CRC 失败，仅丢弃该字段（其余照常载入）");
				continue;
			}

			Codec::Reader pr(payload.data(), payload.size());
			const auto    key  = pr.Str();
			const auto    kind = static_cast<Kind>(pr.U8());
			Value         value;
			if (!pr.Ok() || key.empty() || !ReadValue(pr, kind, value)) {
				++a_report.damagedFields;
				a_report.Add(Fault::kFieldDropped, "L4 一条记录的值无法解析，仅丢弃该字段");
				continue;
			}

			const auto* spec = State::Find(key);
			if (!spec) {
				++a_report.discardedUnknown;
				a_report.events.push_back("按新分类舍弃未登记字段: " + key);
				continue;
			}
			if (spec->policy != Policy::kPersist || spec->storage != Storage::kSnapshot) {
				++a_report.discardedByPolicy;
				a_report.events.push_back(
					"按新分类舍弃 " + std::string{ State::PolicyName(spec->policy) } +
					" 字段: " + key + "（" + spec->rationale + "）");
				continue;
			}
			if (spec->kind != kind) {
				++a_report.discardedByPolicy;
				a_report.events.push_back("字段类型与分类表不符，已舍弃: " + key);
				continue;
			}

			Clamp(*spec, value, a_report, key);
			a_out.emplace(key, std::move(value));
			++a_report.persisted;
		}

		if (torn) {
			a_report.Add(Fault::kTruncated,
				"L3 状态尾部写入中断，已保留前面 " + std::to_string(a_report.persisted) +
				" 个完好字段");
		}
		if (count != a_report.persisted) {
			a_report.events.push_back("头部声明 " + std::to_string(count) +
				" 字段，实际采用 " + std::to_string(a_report.persisted) +
				" 个（差额为按分类舍弃或损坏隔离的字段）");
		}
		a_tx = tx;
		return a_report.persisted > 0 || a_report.discardedByPolicy > 0 ||
		       a_report.damagedFields > 0 || torn;
	}

	LoadReport Load(const void* a_bytes, std::size_t a_len, DeriveFn a_derive)
	{
		LoadReport report;
		FieldMap   incoming;
		std::uint64_t tx = 0;

		if (!a_bytes || a_len == 0) {
			report.Add(Fault::kMissing, "L0 co-save 没有状态块，改用磁盘快照");
			if (s_initialized) {
				return LoadFromDisk(a_derive);
			}
			return Create(s_status.identity, a_derive);
		}

		if (!Decode(a_bytes, a_len, incoming, tx, report)) {
			// L1/L2/L3-with-no-records: keep whatever disk already gave us.
			if (s_live.empty()) {
				auto fallback = LoadFromDisk(a_derive);
				return fallback;
			}
			RebuildViews(report, a_derive);
			return report;
		}

		if (incoming.empty()) {
			// Every record in this save was reclassified as derivable or
			// ephemeral.  That is a legitimate transition from an old build,
			// not a corrupt file - rebuild the canonical defaults and keep the
			// discard events so the report can name every dropped field.
			auto fresh = Create(s_status.identity, a_derive);
			for (auto& e : report.events) { fresh.events.push_back(std::move(e)); }
			for (const auto f : report.faults) { fresh.faults.push_back(f); }
			fresh.authoritative = true;
			return fresh;
		}

		// A co-save block is read from inside the .ess being loaded, so it is
		// by construction scoped to this save: "save as" copies the block
		// together with the world it describes.  The only thing that can be
		// wrong is the owner, and adopting another character's block would
		// attach their receipts to this character forever.
		if (!CheckOwner(incoming, report)) {
			report.authoritative = false;
			auto fallback = s_initialized ? LoadFromDisk(a_derive)
			                              : Create(s_status.identity, a_derive);
			// Keep the reason: a report that silently lost "this block belongs
			// to another character" would look like a clean load.
			for (auto& e : report.events) { fallback.events.push_back(std::move(e)); }
			for (const auto f : report.faults) { fallback.faults.push_back(f); }
			return fallback;
		}
		report.sameSave = true;

		s_live = std::move(incoming);
		s_tx = std::min<std::uint64_t>(tx, kMaxGeneration);
		s_live["tx/generation"] = Value::Int(static_cast<std::int64_t>(s_tx));
		s_live["schema/version"] = Value::Text(kPluginVersion);
		s_live["meta/version"] = Value::Text(kPluginVersion);
		report.tx = s_tx;
		report.authoritative = true;
		s_status.cosaveOk = true;
		s_status.cosaveTx = s_tx;

		RebuildViews(report, a_derive);
		ReplayWal(report);
		// The block may name the file it was first written into ("save as"
		// keeps the block, not the name).  The current save name wins.
		NormalizeIdentity();
		return report;
	}

	// Old Vault magic.  The source wrote the multi-character literal 'SVLT';
	// on disk it is the little-endian u32 0x53564C54, i.e. the bytes "TLVS".
	// Verified against a real slot-a.bin: taking the literal at face value
	// silently rejects every legacy save.  Kept only for the one-way import;
	// no new bytes are ever produced in this format.
	constexpr std::uint32_t kLegacyMagic = 0x53564C54u;

	bool DecodeLegacyVault(const void* a_data, std::size_t a_len,
		FieldMap& a_out, std::uint64_t& a_tx, LoadReport& a_report)
	{
		a_out.clear();
		a_tx = 0;
		Codec::Reader r(a_data, a_len);
		const auto magic  = r.U32();
		const auto format = r.U16();
		r.U16();
		const auto tx     = r.U64();
		r.F64();           // legacy gameTime
		const auto count  = r.U32();
		if (!r.Ok() || magic != kLegacyMagic || format > 2) {
			a_report.Add(Fault::kBadMagic, "旧格式状态无法识别，已跳过一次性导入");
			return false;
		}

		const auto bodyLen = a_len >= 4 ? a_len - 4 : 0;
		r.Seek(28);
		while (r.Ok() && r.Cursor() < bodyLen) {
			if (bodyLen - r.Cursor() < 8) { break; }
			const auto len = r.U32();
			if (!r.Ok() || len < 3 || len > kMaxValueBytes ||
				len + 4 > bodyLen - r.Cursor()) {
				break;
			}
			std::string payload(len, '\0');
			if (!r.Raw(payload.data(), len)) { break; }
			const auto crc = r.U32();
			if (!r.Ok()) { break; }
			if (Codec::Crc32(payload.data(), payload.size()) != crc) {
				++a_report.damagedFields;
				continue;
			}

			Codec::Reader pr(payload.data(), payload.size());
			const auto    key  = pr.Str();
			const auto    type = pr.U8();   // legacy Type enum
			if (!pr.Ok() || key.empty()) { continue; }

			// THE new rules decide FIRST, before the old bytes are even
			// interpreted.  A field the schema now calls derivable or
			// ephemeral is discarded with its stated reason - including the
			// legacy kForm records whose raw FormIDs we no longer parse at
			// all.  Judging after value parsing mislabels a deliberate
			// discard as "damage", which hides the intent we are trying to
			// make visible.
			const auto* spec = State::Find(key);
			if (!spec) {
				++a_report.discardedUnknown;
				a_report.events.push_back("旧格式未登记字段已舍弃: " + key);
				continue;
			}
			if (spec->policy != Policy::kPersist || spec->storage != Storage::kSnapshot) {
				++a_report.discardedByPolicy;
				a_report.events.push_back("旧格式字段按新分类舍弃: " + key +
					"（" + spec->rationale + "）");
				continue;
			}

			std::int64_t i = 0;
			double       f = 0.0;
			std::string  s;
			bool         readable = false;
			switch (type) {
			case 0: i = static_cast<std::int64_t>(pr.U64()); readable = pr.Ok(); break;
			case 1: f = pr.F64();                            readable = pr.Ok(); break;
			case 2: i = pr.U8();                             readable = pr.Ok(); break;
			case 3:
			{
				const auto n = pr.U16();
				if (pr.Ok() && n <= kMaxValueBytes) {
					s.assign(n, '\0');
					readable = pr.Raw(s.data(), n);
				}
				break;
			}
			default:  // legacy kForm: deliberately never decoded again
				break;
			}
			if (!readable) {
				++a_report.discardedByPolicy;
				a_report.events.push_back("旧格式值类型不受支持，已按新分类舍弃: " + key);
				continue;
			}

			Value v;
			v.kind = spec->kind;
			switch (spec->kind) {
			case Kind::kInt:    v.i = i; break;
			case Kind::kFloat:  v.f = f; break;
			case Kind::kBool:   v.b = i != 0; break;
			case Kind::kString:
			case Kind::kBlob:   v.s = std::move(s); break;
			}
			if (type != 3 && (spec->kind == Kind::kString || spec->kind == Kind::kBlob)) {
				continue;  // numeric legacy value cannot fill a text field
			}
			Clamp(*spec, v, a_report, key);
			a_out.emplace(key, std::move(v));
			++a_report.persisted;
		}

		if (count > 0 && a_report.persisted == 0) {
			a_report.Add(Fault::kTruncated, "旧格式状态里没有可被新分类接纳的字段");
			return false;
		}
		a_tx = std::min<std::uint64_t>(tx, kMaxGeneration);
		return a_report.persisted > 0;
	}

	// Reads the retired Vault slots once, if the new snapshot does not exist
	// yet.  Returns true when anything was adopted.
	bool ImportLegacyOnce(LoadReport& a_report)
	{
		const auto legacyDir = s_dataDir.parent_path() / "Vault" / s_status.identity;
		std::error_code ec;
		if (!std::filesystem::is_directory(legacyDir, ec)) {
			return false;
		}

		FieldMap     best;
		std::uint64_t bestTx = 0;
		for (const char* slot : { "slot-b.bin", "slot-a.bin" }) {
			const auto file = legacyDir / slot;
			std::ifstream in(file, std::ios::binary);
			if (!in.is_open()) { continue; }
			std::vector<std::uint8_t> bytes(
				(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			FieldMap     fields;
			std::uint64_t tx = 0;
			LoadReport   probe;
			if (DecodeLegacyVault(bytes.data(), bytes.size(), fields, tx, probe) &&
				tx >= bestTx && !fields.empty()) {
				best = std::move(fields);
				bestTx = tx;
			}
		}
		if (best.empty()) {
			return false;
		}

		s_live = std::move(best);
		s_tx = bestTx;
		s_live["tx/generation"] = Value::Int(static_cast<std::int64_t>(s_tx));
		s_live["schema/version"] = Value::Text(kPluginVersion);
		s_live["meta/version"] = Value::Text(kPluginVersion);
		s_live["identity/character"] = Value::Text(s_status.identity);
		a_report.tx = s_tx;
		a_report.persisted = s_live.size();
		a_report.authoritative = true;
		a_report.Add(Fault::kNone,
			"从旧格式一次性导入 " + std::to_string(s_live.size()) +
			" 个字段；旧存留规则已废弃，此后只写新格式");
		return true;
	}

	LoadReport LoadFromDisk(DeriveFn a_derive)
	{
		LoadReport report;
		if (!s_initialized || s_dataDir.empty()) {
			return Create(s_status.identity, a_derive);
		}

		// Per-character store first, then the single file written by the
		// 0.9.0 build.  The old file is adopted through the same scope filter,
		// so it contributes character-scoped history but never a baseline
		// that belongs to some other save.
		std::vector<std::filesystem::path> candidates;
		if (!s_snapshot.empty()) {
			candidates.push_back(s_snapshot);
		}
		const auto legacy = s_dataDir / "state.bin";
		if (std::find(candidates.begin(), candidates.end(), legacy) == candidates.end()) {
			candidates.push_back(legacy);
		}

		std::filesystem::path used;
		std::vector<std::uint8_t> bytes;
		for (const auto& file : candidates) {
			std::ifstream in(file, std::ios::binary);
			if (!in.is_open()) {
				continue;
			}
			bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (!bytes.empty()) {
				used = file;
				break;
			}
		}

		if (bytes.empty()) {
			// Try the one-way upgrade BEFORE declaring a fresh character.
			// Creating first and importing over it would leave a misleading
			// "L0 no prior state, defaults created" event in the report even
			// though eight real fields were in fact recovered.
			LoadReport adopted;
			if (ImportLegacyOnce(adopted)) {
				RebuildViews(adopted, a_derive);
				ReplayWal(adopted);
				NormalizeIdentity();
				return adopted;
			}
			return Create(s_status.identity, a_derive);
		}

		FieldMap     incoming;
		std::uint64_t tx = 0;
		if (!Decode(bytes.data(), bytes.size(), incoming, tx, report)) {
			auto fresh = Create(s_status.identity, a_derive);
			for (auto& e : report.events) { fresh.events.push_back(std::move(e)); }
			for (const auto f : report.faults) { fresh.faults.push_back(f); }
			fresh.scopeDropped += report.scopeDropped;
			return fresh;
		}

		// Ownership before inheritance: another character's state is not a
		// stale copy, it is a different world.
		if (!CheckOwner(incoming, report)) {
			auto fresh = Create(s_status.identity, a_derive);
			for (auto& e : report.events) { fresh.events.push_back(std::move(e)); }
			for (const auto f : report.faults) { fresh.faults.push_back(f); }
			fresh.authoritative = true;
			return fresh;
		}

		const auto before = incoming.size();
		DropForeignSaveFields(incoming, report);
		if (incoming.size() != before) {
			report.events.push_back(
				"磁盘快照来自 " + used.string() + "，它不是本次读入存档的缓存；"
				"存档级字段已回落到默认值，需要保存一次才会重新建立基线");
		}
		if (used == legacy && !s_snapshot.empty() && used != s_snapshot) {
			report.events.push_back(
				"读取了 0.9.0 的单文件快照（State\\state.bin）；"
				"下次保存将写入按角色分目录的新位置");
		}

		s_live = std::move(incoming);
		s_tx = std::min<std::uint64_t>(tx, kMaxGeneration);
		s_live["tx/generation"] = Value::Int(static_cast<std::int64_t>(s_tx));
		report.tx = s_tx;
		report.authoritative = true;
		RebuildViews(report, a_derive);
		ReplayWal(report);
		NormalizeIdentity();
		return report;
	}

	bool Commit()
	{
		LoadReport report;
		if (!s_derive) {
			// A no-op provider is always valid; classification still applies.
			s_derive = [](const char*, Value&) { return false; };
		}

		// The snapshot is written under the character it belongs to, and it
		// records which save produced it.  Both facts are re-asserted here:
		// a commit is the last moment at which the scope can still be fixed.
		NormalizeIdentity();
		if (s_snapshot.empty()) {
			s_snapshot = SnapshotPathFor(s_character);
		}

		const auto nextTx = s_tx + 1;
		s_live["tx/generation"] = Value::Int(static_cast<std::int64_t>(nextTx));
		s_live["schema/version"] = Value::Text(kPluginVersion);
		s_live["meta/version"] = Value::Text(kPluginVersion);

		// ---- serialise ------------------------------------------------------
		std::uint32_t written = 0;
		const auto    bytes   = BuildBytes(s_live, nextTx, written);

		// ---- read-back BEFORE touching the authoritative snapshot ----------
		FieldMap     check;
		std::uint64_t checkTx = 0;
		LoadReport   decodeReport;
		if (!Decode(bytes.data(), bytes.size(), check, checkTx, decodeReport) ||
			checkTx != nextTx || check.size() != s_live.size()) {
			report.Add(Fault::kRoundTripFailed, "L6 序列化结果无法无损解回，已拒绝提交");
			s_report = std::move(report);
			s_status.roundTripOk = false;
			return false;
		}
		for (const auto& [key, value] : s_live) {
			auto it = check.find(key);
			if (it == check.end() || !it->second.SameAs(value)) {
				report.Add(Fault::kRoundTripFailed,
					"L6 回读字段不一致: " + key + "，已拒绝提交");
				s_report = std::move(report);
				s_status.roundTripOk = false;
				return false;
			}
		}

		// ---- atomic replace -------------------------------------------------
		if (s_initialized) {
			std::string note;
			if (!WriteAtomic(s_snapshot, bytes, note)) {
				report.Add(Fault::kRoundTripFailed, "L6 " + note);
				s_report = std::move(report);
				s_status.roundTripOk = false;
				return false;
			}
		}

		s_tx = nextTx;
		report.persisted = written;
		report.tx = s_tx;
		report.roundTripOk = true;
		report.authoritative = true;
		report.Add(Fault::kNone,
			"提交 tx " + std::to_string(s_tx) + "：回读逐字段比对通过（" +
			std::to_string(written) + " 个持久化字段）");
		s_report = std::move(report);
		s_status.tx = s_tx;
		s_status.entries = s_live.size();
		s_status.roundTripOk = true;
		s_status.note = s_report.Summary();
		return true;
	}

	// -------------------------------------------------------------------------
	// Schema-routed update
	// -------------------------------------------------------------------------
	bool Set(std::string_view a_key, const Value& a_value)
	{
		const auto* spec = State::Find(a_key);
		if (!spec) {
			++s_rejected;
			return false;
		}
		if (spec->kind != a_value.kind) {
			return false;
		}
		if (a_value.s.size() > kMaxValueBytes) {
			return false;
		}
		const std::string key{ a_key };

		switch (spec->policy) {
		case Policy::kPersist:
			if (spec->storage == Storage::kSnapshot) {
				s_live[key] = a_value;
			} else if (spec->storage == Storage::kWal) {
				s_wal[key] = a_value;
			}
			return true;
		case Policy::kDerive:
			s_derived[key] = a_value;   // visible now, never persisted
			return true;
		case Policy::kEphemeral:
			s_ephemeral[key] = a_value; // visible now, reset on next load
			return true;
		}
		return false;
	}

	bool Get(std::string_view a_key, Value& a_out)
	{
		const std::string key{ a_key };
		for (const auto* view : { &s_live, &s_wal, &s_derived, &s_ephemeral }) {
			if (auto it = view->find(key); it != view->end()) {
				a_out = it->second;
				return true;
			}
		}
		return false;
	}

	bool Has(std::string_view a_key)
	{
		Value v;
		return Get(a_key, v);
	}

	bool Erase(std::string_view a_key)
	{
		const std::string key{ a_key };
		bool found = false;
		for (auto* view : { &s_live, &s_wal, &s_derived, &s_ephemeral }) {
			found |= view->erase(key) > 0;
		}
		return found;
	}

	bool SetInt(std::string_view a_key, std::int64_t a_value)
	{ return Set(a_key, Value::Int(a_value)); }
	bool SetFloat(std::string_view a_key, double a_value)
	{ return Set(a_key, Value::Float(a_value)); }
	bool SetBool(std::string_view a_key, bool a_value)
	{ return Set(a_key, Value::Bool(a_value)); }
	bool SetString(std::string_view a_key, std::string_view a_value)
	{
		const auto* spec = State::Find(a_key);
		if (!spec) { ++s_rejected; return false; }
		return spec->kind == Kind::kBlob ? Set(a_key, Value::Blob(a_value))
		                                : Set(a_key, Value::Text(a_value));
	}
	bool GetInt(std::string_view a_key, std::int64_t& a_value)
	{
		Value v;
		if (!Get(a_key, v) || v.kind != Kind::kInt) { return false; }
		a_value = v.i;
		return true;
	}
	bool GetFloat(std::string_view a_key, double& a_value)
	{
		Value v;
		if (!Get(a_key, v) || v.kind != Kind::kFloat) { return false; }
		a_value = v.f;
		return true;
	}
	bool GetBool(std::string_view a_key, bool& a_value)
	{
		Value v;
		if (!Get(a_key, v) || v.kind != Kind::kBool) { return false; }
		a_value = v.b;
		return true;
	}
	bool GetString(std::string_view a_key, std::string& a_value)
	{
		Value v;
		if (!Get(a_key, v) ||
			(v.kind != Kind::kString && v.kind != Kind::kBlob)) {
			return false;
		}
		a_value = v.s;
		return true;
	}

	bool RecordWalFact(std::string_view a_key, std::string_view a_payload)
	{
		const auto* spec = State::Find(a_key);
		if (!spec || spec->storage != Storage::kWal) {
			++s_rejected;
			return false;
		}
		if (s_walFile.empty()) {
			return false;
		}
		Journal::Log log{ s_walFile };
		log.Open();
		std::string note;
		const auto payload = Journal::MakePayload({
			{ "key", a_key }, { "value", a_payload }, { "tx", std::to_string(s_tx) }
		});
		if (!log.Append(Journal::Kind::kCommit, payload, note)) {
			return false;
		}
		s_wal[std::string{ a_key }] = Value::Text(a_payload);
		return true;
	}

	// -------------------------------------------------------------------------
	// Compatibility surface
	// -------------------------------------------------------------------------
	std::uint32_t RecordType() { return kRecordType; }

	void OnLoadRecord(const void* a_data, std::size_t a_len)
	{
		auto report = Load(a_data, a_len, s_derive);
		s_report = std::move(report);
		ApplyStatus(s_report, s_report.authoritative ?
			Status::Source::kCosave : Status::Source::kRecovered);
		s_status.identity = s_status.identity.empty() ? "default" : s_status.identity;
		if (auto it = s_live.find("identity/character"); it != s_live.end()) {
			s_status.identity = it->second.s;
		}
		s_status.cosaveOk = s_report.authoritative;
		s_status.cosaveTx = s_report.tx;
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		Reset();
	}

	void Reconcile()
	{
		// The new classification deliberately stores no raw FormIDs: player
		// position is derived from the live engine.  There is therefore
		// nothing to reconcile; the counters stay honestly at zero.
		s_status.formRefs = 0;
		s_status.tombstoned = 0;
	}

	bool Deserialize(const void* a_data, std::size_t a_len, std::string& a_note)
	{
		FieldMap     map;
		std::uint64_t tx = 0;
		LoadReport   report;
		const bool ok = Decode(a_data, a_len, map, tx, report);
		a_note = report.Summary();
		if (ok) {
			s_live = std::move(map);
			s_tx = tx;
		}
		return ok;
	}

	// -------------------------------------------------------------------------
	// Observability
	// -------------------------------------------------------------------------
	void Describe(std::ostream& a_out)
	{
		a_out << "状态分类表 / SkySaveSystemUltra v" << kPluginVersion << "\n";
		a_out << "生成时间: " << NowStamp() << "\n";
		a_out << "规则: 只有「必须持久化」字段会写入存档；其余字段每次读档重算或重置。\n";
		a_out << "      旧存档里带有的可推导/临时字段会在读入时按本表直接舍弃。\n\n";

		static constexpr Policy kOrder[] = {
			Policy::kPersist, Policy::kDerive, Policy::kEphemeral
		};
		for (const auto policy : kOrder) {
			a_out << "[" << State::PolicyName(policy) << " · "
			      << State::CountBy(policy) << " 项]\n";
			for (const auto& spec : State::kSpecs) {
				if (spec.policy != policy) { continue; }
				a_out << "  " << std::left << std::setw(30) << spec.key
				      << State::KindName(spec.kind) << " / "
				      << State::StorageName(spec.storage) << " / "
				      << State::ScopeName(spec.scope) << "\n";
				a_out << "      理由: " << spec.rationale << "\n";
			}
			a_out << "\n";
		}

		a_out << "[本次载入结果]\n";
		a_out << "  来源            : ";
		switch (s_status.source) {
		case Status::Source::kCosave:    a_out << "co-save\n"; break;
		case Status::Source::kDisk:      a_out << "磁盘快照\n"; break;
		case Status::Source::kRecovered: a_out << "损坏后恢复\n"; break;
		default:                         a_out << "新建\n"; break;
		}
		a_out << "  事务代数        : " << s_status.tx << "\n";
		a_out << "  持久化字段      : " << s_status.entries << "\n";
		a_out << "  本次存档        : " << (s_status.saveName.empty() ? "未记录" : s_status.saveName) << "\n";
		a_out << "  存档作用域      : "
		      << (s_status.sameSave ? "一致（快照就属于这份存档）"
		                            : "不一致（存档级字段已舍弃，需保存一次重建）") << "\n";
		a_out << "  按作用域舍弃    : " << s_status.scopeDropped << " 项\n";
		a_out << "  归属拒绝        : " << (s_status.foreignOwner ? "是（状态属于另一角色）" : "否") << "\n";
		a_out << "  重算字段        : " << s_status.derivedFields << "\n";
		a_out << "  重置字段        : " << s_status.resetFields << "\n";
		a_out << "  按分类舍弃      : " << s_status.discardedFields << "\n";
		a_out << "  回读一致性      : " << (s_status.roundTripOk ? "通过" : "未验证") << "\n";
		a_out << "  未登记键拒绝数  : " << s_rejected << "\n";
		if (!s_report.events.empty()) {
			a_out << "\n[处置记录]\n";
			for (const auto& e : s_report.events) {
				a_out << "  - " << e << "\n";
			}
		}
	}

	bool WriteSchemaReport(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		std::filesystem::create_directories(a_path.parent_path(), ec);
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) {
			return false;
		}
		Describe(out);
		return static_cast<bool>(out);
	}
}
