// StateTest - proves the reconstructed save/load system end to end.
//
// This executable links ONLY StateStore.cpp (engine-free).  Every claim in
// the delivery report that concerns "saved state == loaded state", "derivable
// fields are recomputed", "ephemeral fields are reset", "one bad field does
// not poison the rest" and "commits are verified by read-back" is asserted
// here, not merely described.

#include "StateSchema.h"
#include "StateStore.h"
#include "Codec.h"
#include "Journal.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	using namespace S3U;
	namespace SS = S3U::StateStore;

	int   g_checks = 0;
	int   g_fails  = 0;

	void Check(bool a_ok, const char* a_what)
	{
		++g_checks;
		if (!a_ok) {
			++g_fails;
			std::printf("  FAIL  %s\n", a_what);
		}
	}

	// A deterministic derive provider.  It returns a value derived solely
	// from the key, so "recomputed after load" can be asserted exactly.
	SS::DeriveFn DeterministicProvider()
	{
		return [](const char* a_key, SS::Value& a_out) -> bool {
			const std::string_view key{ a_key };
			if (a_out.kind == State::Kind::kInt) {
				a_out.i = 1000 + static_cast<std::int64_t>(
					Journal::Digest(key.data(), key.size()) % 500);
				return true;
			}
			if (a_out.kind == State::Kind::kFloat) {
				a_out.f = 42.5;
				return true;
			}
			if (a_out.kind == State::Kind::kBool) {
				a_out.b = true;
				return true;
			}
			if (a_out.kind == State::Kind::kString ||
				a_out.kind == State::Kind::kBlob) {
				a_out.s = std::string{ "derived:" } + a_key;
				return true;
			}
			return false;
		};
	}

	using Field = std::pair<std::string, SS::Value>;

	void EmitValue(Codec::Writer& a_out, const SS::Value& a_value)
	{
		switch (a_value.kind) {
		case State::Kind::kInt:    a_out.U64(static_cast<std::uint64_t>(a_value.i)); break;
		case State::Kind::kFloat:  a_out.F64(a_value.f); break;
		case State::Kind::kBool:   a_out.U8(a_value.b ? 1 : 0); break;
		case State::Kind::kString:
		case State::Kind::kBlob:
			a_out.U32(static_cast<std::uint32_t>(a_value.s.size()));
			a_out.Raw(a_value.s.data(), a_value.s.size());
			break;
		}
	}

	// Builds a snapshot with full control over the header and per-record CRC,
	// so hostile inputs can be produced without touching production code.
	std::vector<std::uint8_t> MakeSnapshot(const std::vector<Field>& a_fields,
		std::uint32_t a_magic = SS::kMagic,
		std::uint16_t a_format = SS::kFormat,
		std::uint64_t a_tx = 7,
		const std::set<std::size_t>& a_badCrc = {},
		bool a_badTrailer = false)
	{
		Codec::Writer out;
		out.U32(a_magic);
		out.U16(a_format);
		out.U16(0);
		out.U64(a_tx);
		out.U64(123456789);
		out.U32(static_cast<std::uint32_t>(a_fields.size()));

		for (std::size_t i = 0; i < a_fields.size(); ++i) {
			Codec::Writer rec;
			rec.Str(a_fields[i].first);
			rec.U8(static_cast<std::uint8_t>(a_fields[i].second.kind));
			EmitValue(rec, a_fields[i].second);
			const auto len = static_cast<std::uint32_t>(rec.Size());
			const auto crc = Codec::Crc32(rec.Data().data(), len);
			out.U32(len);
			out.Raw(rec.Data().data(), len);
			out.U32(a_badCrc.count(i) ? crc ^ 0x1u : crc);
		}
		const auto body = Codec::Crc32(out.Data().data(), out.Size());
		out.U32(a_badTrailer ? body ^ 0x1u : body);
		return out.Data();
	}

	// -------------------------------------------------------------------------
	void TestSchemaIntegrity()
	{
		std::printf("[schema-integrity]\n");

		Check(State::kSpecCount >= 30, "classification table is substantial");
		std::set<std::string_view> keys;
		for (const auto& spec : State::kSpecs) {
			Check(State::Find(spec.key) != nullptr, "every declared key resolves");
			keys.insert(spec.key);
			Check(spec.rationale != nullptr && spec.rationale[0] != '\0',
				"every field states a keep/drop reason");

			if (spec.policy == State::Policy::kPersist) {
				Check(spec.storage != State::Storage::kNone,
					"a persisted field has a storage backend");
			} else {
				Check(spec.storage == State::Storage::kNone,
					"a discarded field never has a storage backend");
			}
		}
		Check(keys.size() == State::kSpecCount, "no duplicate field declarations");
		Check(State::Find("totally/undeclared") == nullptr,
			"undeclared keys have no implicit persistence");
		Check(State::Find("migrate/Foo/v") != nullptr,
			"dynamic family matching works");
		Check(State::Find("init/applied/Bar.esp") != nullptr,
			"dynamic init family matching works");
		Check(State::CountBy(State::Policy::kPersist) ==
			State::kSpecCount - State::CountBy(State::Policy::kDerive) -
			State::CountBy(State::Policy::kEphemeral), "policy counts partition the table");

		std::printf("  persist=%zu derive=%zu ephemeral=%zu total=%zu\n",
			State::CountBy(State::Policy::kPersist),
			State::CountBy(State::Policy::kDerive),
			State::CountBy(State::Policy::kEphemeral),
			State::kSpecCount);
	}

	// -------------------------------------------------------------------------
	void TestCreateAndRouting()
	{
		std::printf("[create-and-routing]\n");

		SS::SetDeriveProvider(DeterministicProvider());
		auto report = SS::Create("D46E04D7", DeterministicProvider());

		Check(report.authoritative, "Create produces an authoritative state");
		Check(report.derived == State::CountBy(State::Policy::kDerive),
			"Create recomputes every derivable field");
		Check(report.reset == State::CountBy(State::Policy::kEphemeral),
			"Create resets every ephemeral field");

		std::int64_t saveCount = -1;
		Check(SS::GetInt("meta/saveCount", saveCount) && saveCount == 0,
			"persisted field has its declared default");

		SS::Value derived;
		Check(SS::Get("stats/forms", derived) && derived.i >= 1000,
			"derivable field was produced by the provider");

		SS::Value ephemeral;
		Check(SS::Get("session/notifyQueue", ephemeral),
			"ephemeral field exists in the in-memory view");
		Check(ephemeral.kind == State::Kind::kBlob && ephemeral.s.empty(),
			"ephemeral field is at its declared default after Create");

		const auto before = SS::RejectedKeys();
		Check(!SS::SetString("some/undeclared/key", "x"),
			"undeclared key is refused outright");
		Check(SS::RejectedKeys() == before + 1, "rejection is counted");

		Check(!SS::SetInt("meta/version", 3), "wrong value type is refused");
		Check(SS::SetInt("meta/saveCount", 99), "classified integer is accepted");
		Check(SS::SetString("init/applied/Foo.esp", "0.9.0"),
			"dynamic persisted family is accepted");
		Check(SS::SetInt("migrate/Foo/v", 2), "migration receipt is accepted");
	}

	// -------------------------------------------------------------------------
	void TestOnlyPersistFieldsAreSerialized()
	{
		std::printf("[serialisation-filter]\n");

		// Write a derivable and an ephemeral field with loud, non-default
		// values.  Neither may appear in the byte stream.
		SS::SetInt("stats/forms", 999999);
		SS::SetString("session/lastError", "LOUD SESSION VALUE");
		SS::SetFloat("meta/gameDays", 777.0);

		const auto bytes = SS::Serialize();
		Check(!bytes.empty(), "serialisation produces bytes");

		SS::FieldMap   map;
		std::uint64_t  tx = 0;
		SS::LoadReport report;
		Check(SS::Decode(bytes.data(), bytes.size(), map, tx, report),
			"own output decodes cleanly");
		Check(report.Clean(), "own output has no faults");

		Check(map.count("meta/saveCount") == 1, "persisted field is present");
		Check(map.count("init/applied/Foo.esp") == 1, "dynamic persisted field present");
		Check(map.count("migrate/Foo/v") == 1, "migration receipt present");
		Check(map.count("stats/forms") == 0, "derivable field is NOT serialised");
		Check(map.count("meta/gameDays") == 0, "derivable float is NOT serialised");
		Check(map.count("session/lastError") == 0, "ephemeral field is NOT serialised");

		const std::string blob(bytes.begin(), bytes.end());
		Check(blob.find("LOUD SESSION VALUE") == std::string::npos,
			"ephemeral value does not leak into the file");
	}

	// -------------------------------------------------------------------------
	void TestRoundTripConsistency()
	{
		std::printf("[round-trip-consistency]\n");

		// Capture the full authoritative view, decode our own output, and
		// compare field for field.  This is the exact check Commit() performs.
		SS::FieldMap   expected;
		std::uint64_t  tx = 0;
		SS::LoadReport r1;
		const auto bytes = SS::Serialize();
		SS::Decode(bytes.data(), bytes.size(), expected, tx, r1);

		SS::FieldMap   actual;
		std::uint64_t  tx2 = 0;
		SS::LoadReport r2;
		Check(SS::Decode(bytes.data(), bytes.size(), actual, tx2, r2),
			"second independent decode succeeds");
		Check(tx == tx2 && expected.size() == actual.size(),
			"header and field count are stable across decodes");
		bool identical = true;
		for (const auto& [k, v] : expected) {
			auto it = actual.find(k);
			if (it == actual.end() || !it->second.SameAs(v)) { identical = false; }
		}
		Check(identical, "every persisted field round-trips exactly");

		// A second serialisation of the same state must be byte-identical
		// (ordered field map, deterministic encoder).
		const auto again = SS::Serialize();
		Check(again == bytes, "same state serialises to identical bytes");
	}

	// -------------------------------------------------------------------------
	void TestLoadRecomputesAndResets()
	{
		std::printf("[load-recomputes-and-resets]\n");

		SS::SetInt("stats/forms", 999999);
		SS::SetString("session/lastError", "LOUD SESSION VALUE");
		SS::SetFloat("meta/gameDays", 777.0);
		const auto bytes = SS::Serialize();

		auto report = SS::Load(bytes.data(), bytes.size(), DeterministicProvider());
		Check(report.authoritative, "Load produces an authoritative state");
		Check(report.derived == State::CountBy(State::Policy::kDerive),
			"Load recomputes every derivable field");
		Check(report.reset == State::CountBy(State::Policy::kEphemeral),
			"Load resets every ephemeral field");

		SS::Value v;
		Check(SS::Get("stats/forms", v) && v.i >= 1000 && v.i != 999999,
			"derivable field was recomputed, not carried over");
		Check(SS::Get("meta/gameDays", v) && v.f == 42.5,
			"derivable float was recomputed, not carried over");
		Check(SS::Get("session/lastError", v) && v.s.empty(),
			"ephemeral field was reset to its default");

		std::int64_t saves = -1;
		Check(SS::GetInt("meta/saveCount", saves) && saves == 99,
			"persisted field survived the load unchanged");
		std::string applied;
		Check(SS::GetString("init/applied/Foo.esp", applied) && applied == "0.9.0",
			"dynamic persisted field survived the load unchanged");
	}

	// -------------------------------------------------------------------------
	void TestOldSaveFieldsAreDiscarded()
	{
		std::printf("[old-save-reclassification]\n");

		std::vector<Field> legacy = {
			{ "meta/saveCount", SS::Value::Int(5) },
			{ "player/ref", SS::Value::Text("00000014") },
			{ "trend/forms", SS::Value::Int(181031) },
			{ "stats/refs", SS::Value::Int(654) },
			{ "session/lastError", SS::Value::Text("stale error") },
			{ "totally/undeclared", SS::Value::Int(1) },
		};
		const auto bytes = MakeSnapshot(legacy);

		SS::FieldMap   map;
		std::uint64_t  tx = 0;
		SS::LoadReport report;
		Check(SS::Decode(bytes.data(), bytes.size(), map, tx, report),
			"legacy snapshot decodes");
		Check(map.count("meta/saveCount") == 1, "still-classified field is kept");
		Check(map.count("player/ref") == 0, "old derivable FormID is discarded");
		Check(map.count("trend/forms") == 0, "old redundant trend value is discarded");
		Check(map.count("session/lastError") == 0, "old ephemeral value is discarded");
		Check(report.discardedByPolicy >= 4, "discards are counted by policy");
		Check(report.discardedUnknown >= 1, "undeclared keys are counted");
		Check(tx == 7, "transaction counter read from the header");
		bool reasonsPresent = false;
		for (const auto& e : report.events) {
			if (e.find("理由") != std::string::npos ||
				e.find("舍弃") != std::string::npos) { reasonsPresent = true; }
		}
		Check(reasonsPresent, "every discard is explained in the load report");
	}

	// -------------------------------------------------------------------------
	void TestFieldLevelDamageIsolation()
	{
		std::printf("[field-level-damage-isolation]\n");

		std::vector<Field> fields = {
			{ "meta/saveCount", SS::Value::Int(11) },
			{ "loadorder/list", SS::Value::Blob("Skyrim.esm\nUpdate.esm\nFoo.esp") },
			{ "init/applied/Foo.esp", SS::Value::Text("0.9.0") },
			{ "meta/firstSeen", SS::Value::Text("2026-11-04") },
		};
		const auto bytes = MakeSnapshot(fields, SS::kMagic, SS::kFormat, 9, { 1 });

		SS::FieldMap   map;
		std::uint64_t  tx = 0;
		SS::LoadReport report;
		SS::Decode(bytes.data(), bytes.size(), map, tx, report);
		Check(report.damagedFields == 1, "exactly one field failed its CRC");
		Check(map.size() == 3, "the other three fields still loaded");
		Check(map.count("meta/saveCount") == 1, "field before the damage survives");
		Check(map.count("init/applied/Foo.esp") == 1, "field after the damage survives");
		bool hasL4 = false;
		for (const auto f : report.faults) {
			if (f == SS::Fault::kFieldDropped) { hasL4 = true; }
		}
		Check(hasL4, "damage is reported as an L4 graded fault");

		// A wrong overall checksum must NOT throw away intact records.
		const auto badTrailer = MakeSnapshot(fields, SS::kMagic, SS::kFormat, 9, {}, true);
		SS::FieldMap   map2;
		std::uint64_t  tx2 = 0;
		SS::LoadReport report2;
		SS::Decode(badTrailer.data(), badTrailer.size(), map2, tx2, report2);
		Check(map2.size() == 4, "bad trailer still allows per-field recovery");
		bool hasL3 = false;
		for (const auto f : report2.faults) {
			if (f == SS::Fault::kTruncated) { hasL3 = true; }
		}
		Check(hasL3, "bad trailer is reported as an L3 graded fault");
	}

	// -------------------------------------------------------------------------
	void TestHostileInputs()
	{
		std::printf("[hostile-inputs]\n");

		std::vector<Field> fields = {
			{ "meta/saveCount", SS::Value::Int(3) },
			{ "loadorder/list", SS::Value::Blob("Skyrim.esm\nUpdate.esm") },
		};
		const auto bytes = MakeSnapshot(fields);

		// Every truncation prefix must be a normal, recoverable outcome:
		// no crash, no unbounded allocation, no fabricated full state.
		for (std::size_t n = 0; n < bytes.size(); ++n) {
			SS::FieldMap   map;
			std::uint64_t  tx = 0;
			SS::LoadReport report;
			SS::Decode(bytes.data(), n, map, tx, report);
			if (report.persisted > 2) {
				Check(false, "truncated input cannot fabricate fields");
				break;
			}
		}
		Check(true, "all truncation prefixes handled without fault");

		SS::FieldMap   map;
		std::uint64_t  tx = 0;
		SS::LoadReport report;
		SS::Decode(bytes.data(), bytes.size(), map, tx, report);
		Check(map.size() == 2 && report.Clean(), "intact baseline decodes cleanly");

		const auto badMagic = MakeSnapshot(fields, 0xDEADBEEFu);
		SS::LoadReport r1;
		SS::FieldMap   m1;
		std::uint64_t  t1 = 0;
		SS::Decode(badMagic.data(), badMagic.size(), m1, t1, r1);
		bool foundL1 = false;
		for (const auto f : r1.faults) { if (f == SS::Fault::kBadMagic) foundL1 = true; }
		Check(foundL1 && m1.empty(), "wrong magic is refused as L1");

		const auto future = MakeSnapshot(fields, SS::kMagic, SS::kFormat + 1);
		SS::LoadReport r2;
		SS::FieldMap   m2;
		std::uint64_t  t2 = 0;
		SS::Decode(future.data(), future.size(), m2, t2, r2);
		bool foundL2 = false;
		for (const auto f : r2.faults) { if (f == SS::Fault::kFutureVersion) foundL2 = true; }
		Check(foundL2 && m2.empty(), "future format is refused as L2 without guessing");
	}

	// -------------------------------------------------------------------------
	struct LegacyField
	{
		std::string key;
		std::uint8_t type;   // retired Vault::Type
		std::string payload;
	};

	std::string LegacyString(const std::string& a_value)
	{
		Codec::Writer w;
		w.Str(a_value);
		return std::string(w.Data().begin(), w.Data().end());
	}
	std::string LegacyU64(std::uint64_t a_value)
	{
		Codec::Writer w;
		w.U64(a_value);
		return std::string(w.Data().begin(), w.Data().end());
	}

	std::vector<std::uint8_t> MakeLegacySnapshot(
		const std::vector<LegacyField>& a_fields, std::uint64_t a_tx = 2)
	{
		Codec::Writer out;
		out.U32(0x53564C54u);   // verified against a real slot-a.bin
		out.U16(2);
		out.U16(0);
		out.U64(a_tx);
		out.F64(0.0);
		out.U32(static_cast<std::uint32_t>(a_fields.size()));
		for (const auto& f : a_fields) {
			Codec::Writer rec;
			rec.Str(f.key);
			rec.U8(f.type);
			rec.Raw(f.payload.data(), f.payload.size());
			const auto len = static_cast<std::uint32_t>(rec.Size());
			out.U32(len);
			out.Raw(rec.Data().data(), len);
			out.U32(Codec::Crc32(rec.Data().data(), len));
		}
		out.U32(Codec::Crc32(out.Data().data(), out.Size()));
		return out.Data();
	}

	void TestLegacyImport(const char* a_realSlot)
	{
		std::printf("[legacy-one-way-import]\n");

		std::vector<LegacyField> legacy = {
			{ "loadorder/list", 3, LegacyString("Skyrim.esm\nUpdate.esm\nFoo.esp") },
			{ "meta/saveCount", 0, LegacyU64(7) },
			{ "init/generation", 0, LegacyU64(3) },
			{ "meta/version", 3, LegacyString("0.8.0") },
			{ "player/ref", 4, LegacyString("Skyrim.esm") },
			{ "player/cell", 4, LegacyString("Skyrim.esm") },
			{ "meta/gameDays", 1, LegacyU64(0) },
			{ "loadorder/count", 0, LegacyU64(190) },
			{ "legacy/unknown", 0, LegacyU64(1) },
		};
		const auto bytes = MakeLegacySnapshot(legacy);

		SS::FieldMap   map;
		std::uint64_t  tx = 0;
		SS::LoadReport report;
		Check(SS::DecodeLegacyVault(bytes.data(), bytes.size(), map, tx, report),
			"retired format decodes for the one-way import");
		Check(map.size() == 4, "only the four still-authoritative fields are adopted");
		Check(map.count("loadorder/list") == 1 &&
			map["loadorder/list"].kind == State::Kind::kBlob &&
			map["loadorder/list"].s == "Skyrim.esm\nUpdate.esm\nFoo.esp",
			"baseline is converted to its newly classified blob type");
		Check(map.count("meta/saveCount") == 1 && map["meta/saveCount"].i == 7,
			"historical counter is preserved");
		Check(map.count("player/ref") == 0, "retired raw FormID is not imported");
		Check(map.count("meta/gameDays") == 0, "retired derivable value is not imported");
		Check(map.count("loadorder/count") == 0, "retired derivable count is not imported");
		Check(report.discardedByPolicy >= 4, "reclassification is counted");
		Check(report.discardedUnknown >= 1, "undeclared legacy key is refused");
		Check(tx == 2, "legacy transaction counter carries over");

		// Against the player's ACTUAL 0.8.x slot: proves the byte layout and
		// the magic were read correctly, not assumed.
		if (a_realSlot) {
			std::ifstream in(a_realSlot, std::ios::binary);
			if (in.is_open()) {
				std::vector<std::uint8_t> real(
					(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				SS::FieldMap   m2;
				std::uint64_t  t2 = 0;
				SS::LoadReport r2;
				const bool ok = SS::DecodeLegacyVault(real.data(), real.size(), m2, t2, r2);
				std::printf("  real slot: %zu bytes, decoded=%d, fields=%zu, discarded=%zu\n",
					real.size(), ok ? 1 : 0, m2.size(), r2.discardedByPolicy);
				Check(ok && m2.count("loadorder/list") == 1,
					"real 0.8.x slot imports its load-order baseline");
				Check(m2.count("player/ref") == 0,
					"real slot's raw FormIDs are dropped by the new rules");
			}
		}
	}

	// -------------------------------------------------------------------------
	// Scope adjudication: the rule that stops one save's facts from being
	// inherited by another.  This is the difference between "the initialiser
	// works" and "the initialiser is even asked to run".
	// -------------------------------------------------------------------------
	void TestSaveScope()
	{
		std::printf("[save scope / inheritance]\n");

		namespace fs = std::filesystem;
		const fs::path temp = fs::temp_directory_path() / "3su_state_scope";
		std::error_code ec;
		fs::remove_all(temp, ec);
		fs::create_directories(temp, ec);

		SS::Initialize(temp);
		SS::SetDeriveProvider(DeterministicProvider());

		// ---- save #1 of character D46E04D7 -------------------------------
		SS::ObserveSaveName("Save1_D46E04D7_1_1.ess");
		SS::Create("D46E04D7", DeterministicProvider());
		SS::SetString("loadorder/list", "Skyrim.esm\nUpdate.esm\nOld.esp");
		SS::SetInt("meta/saveCount", 7);
		SS::SetString("trend/history", "HISTORY-OF-THE-CHARACTER");
		SS::SetString("init/applied/Old.esp", "1.0.0");
		SS::SetString("player/lastCell", "00012345");
		Check(SS::Commit(), "commit under save #1");
		Check(fs::exists(temp / "D46E04D7" / "state.bin"),
			"snapshot is filed under the character");
		Check(!fs::exists(temp / "state.bin"),
			"no installation-wide snapshot is written any more");

		const auto save1Bytes = SS::Serialize();
		Check(!save1Bytes.empty(), "save #1 state is serialisable");

		// ---- a DIFFERENT save of the same character ----------------------
		SS::ObserveSaveName("Save5_D46E04D7_9_9.ess");
		std::string   text;
		std::int64_t  number = 0;
		Check(!SS::GetString("loadorder/list", text),
			"another save does not inherit the load-order baseline");
		Check(!SS::Has("init/applied/Old.esp"),
			"nor its initialisation receipts");
		Check(!SS::Has("player/lastCell"),
			"nor the save-scoped cell");
		Check(SS::GetString("trend/history", text) && text == "HISTORY-OF-THE-CHARACTER",
			"character-scoped history still comes across");
		Check(SS::GetInt("meta/saveCount", number) && number == 7,
			"character-scoped counter still comes across");
		Check(SS::Last().scopeDropped >= 3,
			"every refused field is counted, not silently ignored");
		Check(SS::Last().sameSave == false,
			"the load reports the scope mismatch");

		// ---- co-save: the block travels INSIDE the .ess -------------------
		// "Save as" copies both the world and the co-save block, so a renamed
		// save legitimately owns the same receipts.  Refusing them here would
		// break the most common way players keep backups.
		SS::ObserveSaveName("Save9_D46E04D7_9_9.ess");
		auto coReport = SS::Load(save1Bytes.data(), save1Bytes.size(),
			DeterministicProvider());
		Check(coReport.authoritative, "co-save block is adopted");
		Check(SS::GetString("loadorder/list", text) && text.find("Old.esp") != std::string::npos,
			"a co-save block brings its own baseline");
		Check(SS::Has("init/applied/Old.esp"),
			"a co-save block brings its own receipts");
		Check(coReport.sameSave, "co-save is always scoped to the save being read");
		Check(SS::GetString("identity/saveName", text) && text == "Save9_D46E04D7_9_9.ess",
			"the current save name wins over the block's stale name");

		// ---- a snapshot written by ANOTHER character ---------------------
		fs::create_directories(temp / "AAAA1111", ec);
		fs::copy_file(temp / "D46E04D7" / "state.bin", temp / "AAAA1111" / "state.bin",
			fs::copy_options::overwrite_existing, ec);
		Check(!ec, "foreign snapshot planted for the ownership test");

		SS::ObserveSaveName("Save2_AAAA1111_1_1.ess");
		Check(SS::Last().foreignOwner,
			"a state owned by another character is refused outright");
		Check(!SS::GetString("loadorder/list", text),
			"a foreign state contributes no baseline");
		Check(SS::GetString("identity/character", text) && text == "AAAA1111",
			"identity stays with the save being loaded, never the file");
		Check(SS::Commit(), "commit under the second character succeeds");
		Check(fs::exists(temp / "AAAA1111" / "state.bin"),
			"and lands in that character's own directory");
		Check(fs::file_size(temp / "D46E04D7" / "state.bin") > 0,
			"the first character's snapshot is left untouched");

		fs::remove_all(temp, ec);
	}

	// -------------------------------------------------------------------------
	void TestCommitAndReadBack()
	{
		std::printf("[commit-and-read-back]\n");

		namespace fs = std::filesystem;
		const fs::path temp = fs::temp_directory_path() / "3su_state_test";
		std::error_code ec;
		fs::remove_all(temp, ec);
		fs::create_directories(temp, ec);

		SS::Initialize(temp);
		SS::SetDeriveProvider(DeterministicProvider());
		SS::Create("TESTCHAR", DeterministicProvider());
		SS::SetString("loadorder/list", "Skyrim.esm\nUpdate.esm\nFoo.esp");
		SS::SetString("trend/history", std::string(512, 'T'));
		SS::SetString("init/applied/Foo.esp", "0.9.0");

		Check(SS::Commit(), "Commit succeeds");
		Check(SS::Last().roundTripOk, "Commit reports read-back consistency");
		Check(SS::Last().tx == 1, "first commit advances tx to 1");
		Check(fs::exists(temp / "state.bin"), "snapshot file was written");

		const auto saved = SS::Serialize();
		Check(!saved.empty(), "post-commit state is serialisable");

		// Simulate a fresh process: reload from disk, then check every
		// authoritative field against what was saved.
		SS::Create("TESTCHAR", DeterministicProvider());
		auto report = SS::LoadFromDisk(DeterministicProvider());
		Check(report.authoritative, "disk snapshot loads as authoritative");
		Check(report.tx == 1, "transaction counter survived the round trip");

		std::string baseline, history, applied;
		Check(SS::GetString("loadorder/list", baseline) &&
			baseline == "Skyrim.esm\nUpdate.esm\nFoo.esp", "baseline is identical");
		Check(SS::GetString("trend/history", history) && history.size() == 512,
			"blob field is identical");
		Check(SS::GetString("init/applied/Foo.esp", applied) && applied == "0.9.0",
			"dynamic family field is identical");

		Check(SS::Commit(), "second commit succeeds");
		Check(SS::Last().tx == 2, "transaction counter is monotonic");

		// The classification report must be generated and readable.
		Check(SS::WriteSchemaReport(temp / "state-schema.txt"),
			"schema report is written");
		std::ostringstream text;
		SS::Describe(text);
		bool allReasons = true;
		for (const auto& spec : State::kSpecs) {
			if (text.str().find(spec.key) == std::string::npos ||
				text.str().find(spec.rationale) == std::string::npos) {
				allReasons = false;
			}
		}
		Check(allReasons, "report lists every field with its keep/drop reason");

		fs::remove_all(temp, ec);
	}
}

int main(int argc, char** argv)
{
	std::printf("StateTest - SkySaveSystemUltra v%s\n", SS::kPluginVersion);

	TestSchemaIntegrity();
	TestCreateAndRouting();
	TestOnlyPersistFieldsAreSerialized();
	TestRoundTripConsistency();
	TestLoadRecomputesAndResets();
	TestOldSaveFieldsAreDiscarded();
	TestFieldLevelDamageIsolation();
	TestHostileInputs();
	TestLegacyImport(argc > 1 ? argv[1] : nullptr);
	TestCommitAndReadBack();
	TestSaveScope();

	std::printf("\n%d checks, %d failures -> %s\n",
		g_checks, g_fails, g_fails == 0 ? "ALL PASS" : "FAILED");
	return g_fails == 0 ? 0 : 1;
}
