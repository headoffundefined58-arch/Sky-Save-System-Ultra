#pragma once

// -----------------------------------------------------------------------------
// StateStore - complete save/load reconstruction, built on StateSchema.
//
// This is a replacement, not an extension of the old Vault.  The old module
// accepted any key from any caller and persisted it forever; this one will
// not emit a byte for a field the schema has not classified kPersist, and
// will DISCARD such a field if an old save still contains it.
//
// The four operations are explicit and closed:
//
//   Create  - every field gets its declared default; derive fields are
//             recomputed, ephemeral fields are reset.
//   Load    - pick an authoritative source, decode with per-field CRC,
//             apply persist fields, recompute derive fields, reset
//             ephemeral fields, then verify the result is consistent.
//   Update  - routed through the schema.  Writing a derivable/ephemeral
//             field updates the in-memory view but can never reach disk.
//   Commit  - serialise, write atomically, READ IT BACK, and compare every
//             persisted field.  A commit that does not round-trip is a
//             failed commit; the previous state stays authoritative.
//
// Faults are graded L0..L6 instead of collapsing into "corrupt, give up":
// a single damaged field costs one field, a torn tail is repaired in place,
// a missing file creates a fresh state, and an unreadable future version is
// preserved untouched for a newer build to read.
//
// No engine, no SKSE, no spdlog.  tools/state_test.cpp links this file and
// proves the whole lifecycle without launching Skyrim.
// -----------------------------------------------------------------------------

#include "StateSchema.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace SKSE
{
	class SerializationInterface;
}

namespace S3U::StateStore
{
	inline constexpr const char*    kPluginVersion = "2.0.0";
	inline constexpr std::uint32_t  kMagic         = 0x33535453u;  // "STS3"
	inline constexpr std::uint16_t  kFormat        = 1;
	inline constexpr std::uint32_t  kRecordType    = 0x54535453u;  // "STST"
	inline constexpr std::size_t    kMaxValueBytes = 1u << 20;
	inline constexpr std::uint64_t  kMaxGeneration = 1ull << 62;

	struct Value
	{
		State::Kind kind{ State::Kind::kInt };
		std::int64_t i{ 0 };
		double       f{ 0.0 };
		bool         b{ false };
		std::string  s;   // text and blob both live here

		static Value Int(std::int64_t a_v);
		static Value Float(double a_v);
		static Value Bool(bool a_v);
		static Value Text(std::string_view a_v);
		static Value Blob(std::string_view a_v);

		[[nodiscard]] std::string Describe() const;
		[[nodiscard]] bool        SameAs(const Value& a_other) const noexcept;
	};

	// Graded faults.  Higher levels are strictly more severe; Load() reports
	// every one it encountered rather than stopping at the first.
	enum class Fault : std::uint8_t
	{
		kNone = 0,
		kMissing,          // L0 no state file at all -> Create
		kBadMagic,         // L1 not our format -> fall back to an older snapshot
		kFutureVersion,    // L2 written by a newer build -> refuse, keep the file
		kTruncated,        // L3 torn write -> repair the tail / use the other slot
		kFieldDropped,     // L4 one record failed CRC -> that field only
		kClamped,          // L5 value out of range -> clamp to a safe bound
		kRoundTripFailed,  // L6 read-back disagreed -> commit rejected
		kForeignOwner,     // the state belongs to a different character
		kForeignSave,      // save-scoped fields belong to a different .ess
	};

	inline constexpr const char* FaultName(Fault a_fault) noexcept
	{
		switch (a_fault) {
		case Fault::kNone:             return "无";
		case Fault::kMissing:          return "L0 状态缺失，已新建";
		case Fault::kBadMagic:         return "L1 格式标识错误";
		case Fault::kFutureVersion:    return "L2 未来版本，拒绝并保留原文件";
		case Fault::kTruncated:        return "L3 写入中断，已按完好记录恢复";
		case Fault::kFieldDropped:     return "L4 单字段校验失败，已隔离";
		case Fault::kClamped:          return "L5 值越界，已钳制";
		case Fault::kRoundTripFailed:  return "L6 回读不一致，提交已拒绝";
		case Fault::kForeignOwner:     return "S1 状态属于另一角色，拒绝继承";
		case Fault::kForeignSave:      return "S2 存档级字段属于另一份存档，已舍弃";
		default:                       return "未知";
		}
	}

	struct LoadReport
	{
		std::vector<Fault>      faults;
		std::vector<std::string> events;   // human-readable, written to the report
		std::size_t             persisted{ 0 };       // fields accepted from bytes
		std::size_t             derived{ 0 };         // fields recomputed
		std::size_t             reset{ 0 };           // ephemeral fields reset
		std::size_t             discardedByPolicy{ 0 };  // old-save fields dropped
		std::size_t             discardedUnknown{ 0 };   // undeclared keys dropped
		std::size_t             damagedFields{ 0 };
		std::size_t             clampedFields{ 0 };
		std::size_t             scopeDropped{ 0 };   // kSave fields from another .ess
		bool                    sameSave{ true };     // source describes this .ess
		bool                    roundTripOk{ false };
		bool                    authoritative{ false };
		std::uint64_t           tx{ 0 };

		void Add(Fault a_fault, std::string a_text);
		[[nodiscard]] bool Clean() const noexcept;
		[[nodiscard]] std::string Summary() const;
	};

	struct Status
	{
		std::string   identity;
		std::string   dir;
		std::uint64_t tx{ 0 };
		std::size_t   entries{ 0 };

		enum class Source
		{
			kNone,
			kCosave,
			kDisk,
			kRecovered
		};
		Source        source{ Source::kNone };
		bool          cosaveOk{ false };
		bool          diskOk{ false };
		std::uint64_t cosaveTx{ 0 };
		std::uint64_t diskTx{ 0 };
		std::string   note;

		std::size_t   formRefs{ 0 };
		std::size_t   tombstoned{ 0 };
		std::size_t   droppedRecords{ 0 };
		std::size_t   migrated{ 0 };
		double        elapsedMs{ 0.0 };

		// New in 0.9.0: the classification result of the last load.
		std::size_t   derivedFields{ 0 };
		std::size_t   resetFields{ 0 };
		std::size_t   discardedFields{ 0 };
		bool          roundTripOk{ false };

		// New in 1.0.0: scope adjudication.  saveName is the .ess this state
		// now belongs to; scopeDropped counts fields refused because they
		// described a different .ess; foreignOwner means the source state was
		// rejected outright.
		std::string   saveName;
		std::size_t   scopeDropped{ 0 };
		bool          foreignOwner{ false };
		bool          sameSave{ true };
	};

	// A derive field is recomputed by a provider injected by the caller.
	// The engine build supplies a provider that scans the live world; the
	// self-test supplies a deterministic one.  Returning false leaves the
	// field at its declared default and is recorded in the load report.
	using DeriveFn = std::function<bool(const char* a_key, Value& a_out)>;

	// Installed once by the engine build.  Used by Load/Create/OnLoadRecord;
	// the self-test installs its own deterministic provider.
	void SetDeriveProvider(DeriveFn a_fn);

	// ---- lifecycle ---------------------------------------------------------
	void            Initialize(const std::filesystem::path& a_dataDir);
	void            Reset();
	void            ObserveSaveName(const char* a_fileName);
	const Status&   Last() noexcept;
	const LoadReport& LastReport() noexcept;

	// The .ess this process is currently working on.  Initialisation receipts
	// are scoped by it, so they can never be inherited by a save that did not
	// produce them.
	const std::string& CurrentSaveName();
	const std::string& CurrentIdentity();

	// Every live key with the given prefix.  Used by the save path to carry
	// forward receipts the co-save already had instead of only this session's.
	std::vector<std::string> Keys(std::string_view a_prefix);

	// Where the write-ahead log for the current character lives.  The
	// initialiser appends to this exact file and the loader replays it; 0.9.0
	// had two different paths for the two halves, so the replay always read an
	// empty file and the kWal fields were permanently blank.
	std::filesystem::path CurrentWalPath();

	// Create a state from nothing.  Also used as the L0 recovery endpoint.
	LoadReport      Create(std::string_view a_identity, DeriveFn a_derive);

	// Load from bytes (co-save) with an optional disk fallback path.
	LoadReport      Load(const void* a_bytes, std::size_t a_len, DeriveFn a_derive);

	// Load the on-disk snapshot; used when the co-save carries no block.
	LoadReport      LoadFromDisk(DeriveFn a_derive);

	// Persist the current state.  Returns false only when the read-back
	// disagreed OR the file could not be written; either way the previous
	// snapshot remains untouched and authoritative.
	bool            Commit();

	// ---- schema-routed update ----------------------------------------------
	// Returns false when the key is undeclared, the type does not match the
	// schema, or the value is out of range.  kDerive/kEphemeral keys are
	// accepted into the in-memory view but are structurally incapable of
	// reaching a snapshot; that is the point of the classification.
	bool Set(std::string_view a_key, const Value& a_value);
	bool Get(std::string_view a_key, Value& a_out);
	bool Has(std::string_view a_key);
	bool Erase(std::string_view a_key);

	bool SetInt(std::string_view a_key, std::int64_t a_value);
	bool SetFloat(std::string_view a_key, double a_value);
	bool SetBool(std::string_view a_key, bool a_value);
	bool SetString(std::string_view a_key, std::string_view a_value);
	bool GetInt(std::string_view a_key, std::int64_t& a_value);
	bool GetFloat(std::string_view a_key, double& a_value);
	bool GetBool(std::string_view a_key, bool& a_value);
	bool GetString(std::string_view a_key, std::string& a_value);

	// kWal facts are appended and fsynced immediately, never waiting for a save.
	bool RecordWalFact(std::string_view a_key, std::string_view a_payload);

	// ---- byte layer ---------------------------------------------------------
	[[nodiscard]] std::vector<std::uint8_t> Serialize();
	bool Deserialize(const void* a_data, std::size_t a_len, std::string& a_note);

	// Decode into an explicit field map with no side effects.  This is what
	// makes Commit()'s read-back check possible without disturbing live state.
	// Ordered on purpose: a given state always serialises to the same bytes,
	// so "saved state == loaded state" can be checked byte-for-byte as well as
	// field-for-field.
	using FieldMap = std::map<std::string, Value>;
	bool Decode(const void* a_data, std::size_t a_len,
		FieldMap& a_out, std::uint64_t& a_tx, LoadReport& a_report);

	// One-way upgrade path from the retired Vault format.  Old bytes are
	// treated purely as DATA, never as rules: every field is re-judged by
	// StateSchema and anything now classed kDerive/kEphemeral is dropped.
	// Once imported, the new snapshot becomes authoritative and the old
	// format is never written again.
	bool DecodeLegacyVault(const void* a_data, std::size_t a_len,
		FieldMap& a_out, std::uint64_t& a_tx, LoadReport& a_report);

	// ---- co-save / compatibility surface -----------------------------------
	std::uint32_t RecordType();
	void          OnSave(SKSE::SerializationInterface* a_intfc);
	void          OnRevert(SKSE::SerializationInterface* a_intfc);
	void          OnLoadRecord(const void* a_data, std::size_t a_len);
	void          Reconcile();

	// ---- observability ------------------------------------------------------
	// Writes the entire classification table, with the reasons, as plain text.
	// The player can open this and see exactly what is kept and what is dropped.
	void          Describe(std::ostream& a_out);
	bool          WriteSchemaReport(const std::filesystem::path& a_path);

	// Count of undeclared keys rejected since startup.  Non-zero means some
	// module is still trying to persist something the schema does not own.
	std::size_t   RejectedKeys() noexcept;
}
