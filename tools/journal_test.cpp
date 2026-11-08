// Exercises the append-only transaction log without Skyrim or SKSE.
// The important behaviours are: chain verification, torn-tail repair, and
// the write-ahead init receipt that must survive "load -> init -> no save".

#include "Journal.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		std::printf("  [%s] %s\n", a_ok ? "PASS" : "FAIL", a_what);
		if (!a_ok) {
			++g_failures;
		}
	}

	std::filesystem::path g_dir;

	void TestCodec()
	{
		using namespace S3U::Journal;
		std::printf("[record codec]\n");

		Record r;
		r.seq     = 7;
		r.stamp   = 1700000000;
		r.kind    = Kind::kInitApplied;
		r.prev    = 0x1122334455667788ull;
		r.payload = "plugin=test.esp\nquests=3\n";
		const auto bytes = Encode(r);

		Record     out;
		std::size_t used = 0;
		Check(DecodeOne(bytes.data(), bytes.size(), out, used), "round trip decodes");
		Check(used == bytes.size(), "consumed exactly one record");
		Check(out.seq == 7 && out.stamp == 1700000000, "scalar fields survive");
		Check(out.kind == Kind::kInitApplied && out.prev == r.prev, "kind and chain link survive");
		Check(out.payload == r.payload, "payload survives");
		Check(Field(out.payload, "quests") == "3", "field lookup works");
		Check(Field(out.payload, "absent").empty(), "missing field is empty");

		auto broken = bytes;
		broken[20] ^= 0x40;
		Record dummy;
		std::size_t dummyUsed = 0;
		Check(!DecodeOne(broken.data(), broken.size(), dummy, dummyUsed),
			"a flipped byte is rejected");

		bool allRejected = true;
		for (std::size_t n = 0; n < bytes.size(); ++n) {
			if (DecodeOne(bytes.data(), n, dummy, dummyUsed)) {
				allRejected = false;
			}
		}
		Check(allRejected, "every truncated prefix is rejected");
	}

	void TestChainAndRepair()
	{
		using namespace S3U::Journal;
		std::printf("[hash chain + torn tail]\n");

		const auto file = g_dir / "journal.log";
		{
			Log log(file);
			log.Open();
			std::string note;
			Check(log.Append(Kind::kInitPending, "plugin=a.esp\n", note), "append pending");
			Check(log.Append(Kind::kInitApplied, "plugin=a.esp\nquests=2\n", note), "append applied");
			Check(log.Append(Kind::kSchema, "from=1\nto=2\n", note), "append schema");
			Check(log.State().lastSeq == 3, "sequence reached 3");

			const auto receipts = log.InitStates();
			Check(receipts.at("a.esp") == InitState::kApplied, "a.esp is applied");
		}

		{
			Log reopened(file);
			const auto& s = reopened.Open();
			Check(s.records.size() == 3 && !s.repaired, "reopen replays all three intact");
			Check(s.records[1].prev == s.records[0].digest, "link points at predecessor digest");
			Check(s.records[2].prev == s.records[1].digest, "chain continues");
		}

		// Simulate a crash halfway through the fourth record.
		{
			const auto size = std::filesystem::file_size(file);
			Record     r;
			r.seq     = 4;
			r.stamp   = 1700000001;
			r.kind    = Kind::kInitPending;
			r.prev    = 0;  // deliberately wrong too
			r.payload = "plugin=b.esp\n";
			const auto bytes = Encode(r);
			std::ofstream out(file, std::ios::binary | std::ios::app);
			out.write(reinterpret_cast<const char*>(bytes.data()), 19);
			out.close();
			Check(std::filesystem::file_size(file) == size + 19, "torn record physically present");
		}

		{
			Log repaired(file);
			const auto& s = repaired.Open();
			Check(s.repaired && s.records.size() == 3, "torn tail detected, three good records kept");
			Check(std::filesystem::file_size(file) == s.goodBytes, "file truncated to last good byte");
			std::string note;
			Check(repaired.Append(Kind::kInitPending, "plugin=b.esp\n", note),
				"appending works again after repair");
			Check(repaired.State().lastSeq == 4, "sequence resumes at 4");
			Check(repaired.InitStates().at("b.esp") == InitState::kPending,
				"pending receipt is durable without any game save");
		}
	}

	void TestReceiptSemantics()
	{
		using namespace S3U::Journal;
		std::printf("[init receipt state machine]\n");

		std::vector<Record> records;
		auto add = [&records](Kind a_kind, const char* a_plugin) {
			Record r;
			r.kind    = a_kind;
			r.payload = std::string{ "plugin=" } + a_plugin + "\n";
			records.push_back(r);
		};

		add(Kind::kInitPending, "a.esp");
		Check(DeriveInitStates(records).at("a.esp") == InitState::kPending,
			"pending alone stays pending, never blindly retried");

		add(Kind::kInitApplied, "a.esp");
		Check(DeriveInitStates(records).at("a.esp") == InitState::kApplied,
			"applied supersedes pending");

		add(Kind::kInitPending, "b.esp");
		add(Kind::kInitFailed, "b.esp");
		Check(DeriveInitStates(records).at("b.esp") == InitState::kFailed,
			"failed supersedes pending");

		add(Kind::kRollback, "a.esp");
		Check(DeriveInitStates(records).at("a.esp") == InitState::kNone,
			"explicit rollback clears a receipt");

		Record noise;
		noise.kind    = Kind::kCommit;
		noise.payload = "tx=9\n";
		records.push_back(noise);
		Check(DeriveInitStates(records).size() == 2,
			"non-init records and payloads without a plugin are ignored");
	}

	// A receipt is a statement about ONE .ess.  The whole point of the save
	// binding is that an older save of the same character cannot inherit
	// "already initialised" from a newer one and then never run at all.
	void TestSaveBinding()
	{
		using namespace S3U::Journal;
		std::printf("[per-save receipt binding]\n");

		std::vector<Record> records;
		auto add = [&records](Kind a_kind, const char* a_plugin, const char* a_save) {
			Record r;
			r.kind    = a_kind;
			r.payload = std::string{ "plugin=" } + a_plugin +
			            "\nsave=" + a_save + "\n";
			records.push_back(r);
		};

		add(Kind::kInitApplied, "new.esp", "Save5_X.ess");
		add(Kind::kInitPending, "crash.esp", "Save5_X.ess");

		auto viewSave5 = DeriveInitStatesForSave(records, "Save5_X.ess");
		Check(viewSave5.states.at("new.esp") == InitState::kApplied,
			"receipt applies to the save that produced it");
		Check(viewSave5.states.at("crash.esp") == InitState::kPending,
			"pending stays pending for its own save");
		Check(viewSave5.bound == 2, "both records counted as bound");

		auto viewSave1 = DeriveInitStatesForSave(records, "Save1_X.ess");
		Check(viewSave1.states.count("new.esp") == 0,
			"another save of the same character inherits NOTHING");
		Check(viewSave1.foreign == 2, "both records counted as foreign");

		// Explicit rollback must clear the pending blocker: otherwise a
		// half-finished transaction could never be retried.
		add(Kind::kRollback, "crash.esp", "Save5_X.ess");
		Check(DeriveInitStatesForSave(records, "Save5_X.ess").states.at("crash.esp")
				== InitState::kNone,
			"rollback makes the plugin retryable again");

		// Records written by 0.9.0 carry no save binding.  A pending must
		// still block (a crash is a crash), an applied must not be trusted.
		std::vector<Record> legacy;
		Record              l;
		l.kind    = Kind::kInitPending;
		l.payload = "plugin=old-crash.esp\n";
		legacy.push_back(l);
		l.kind    = Kind::kInitApplied;
		l.payload = "plugin=old-done.esp\n";
		legacy.push_back(l);

		const auto legacyView = DeriveInitStatesForSave(legacy, "Anything.ess");
		Check(legacyView.states.at("old-crash.esp") == InitState::kPending,
			"unbound pending is still honoured (never retried)");
		Check(legacyView.states.count("old-done.esp") == 0,
			"unbound applied is not trusted any more");
		Check(legacyView.legacyIgnored == 1, "ignored legacy receipt is counted");
	}

	// Compaction must be bounded AND lossless for verdicts: the newest record
	// per (save, plugin) is exactly what a verdict is computed from, so a
	// pending can never be compacted away into a false "applied".
	void TestCompaction()
	{
		using namespace S3U::Journal;
		std::printf("[bounded log compaction]\n");

		const auto file = g_dir / "compact.log";
		Log        log(file);
		log.Open();
		std::string note;

		for (int i = 0; i < 40; ++i) {
			log.Append(Kind::kInitPending, "plugin=a.esp\nsave=S.ess\n", note);
			log.Append(Kind::kInitApplied, "plugin=a.esp\nsave=S.ess\n", note);
		}
		log.Append(Kind::kInitPending, "plugin=b.esp\nsave=S.ess\n", note);  // crash
		const auto before = log.State().records.size();
		const auto bytesBefore = std::filesystem::file_size(file);
		Check(before == 81, "80 verdict records plus one crash pending");

		Check(log.Compact(10, note), "compaction succeeds");
		Check(log.State().records.size() == 2, "one record per (save, plugin) kept");
		Check(std::filesystem::file_size(file) < bytesBefore, "file shrank");

		const auto view = DeriveInitStatesForSave(log.State().records, "S.ess");
		Check(view.states.at("a.esp") == InitState::kApplied, "applied verdict survives");
		Check(view.states.at("b.esp") == InitState::kPending, "crash pending survives as pending");

		// The rewritten chain must still verify end to end.
		Log reopened(file);
		const auto& s = reopened.Open();
		Check(s.records.size() == 2 && !s.repaired, "compacted chain re-opens cleanly");
		std::uint64_t prev = 0;
		bool          chained = true;
		for (const auto& rec : s.records) {
			if (rec.prev != prev) { chained = false; }
			prev = rec.digest;
		}
		Check(chained, "compacted records form a valid hash chain");

		Check(log.Compact(10, note), "compaction is idempotent below threshold");
	}
}

int main()
{
	std::printf("=== JournalTest ===\n");

	char name[64];
	std::snprintf(name, sizeof(name), "s3su-journal-test-%llu",
		static_cast<unsigned long long>(S3U::Journal::NowSeconds()));
	g_dir = std::filesystem::temp_directory_path() / name;
	std::error_code ec;
	std::filesystem::create_directories(g_dir, ec);

	TestCodec();
	TestChainAndRepair();
	TestReceiptSemantics();
	TestSaveBinding();
	TestCompaction();

	std::filesystem::remove_all(g_dir, ec);
	std::printf("=== %s (%d failures) ===\n",
		g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
	return g_failures == 0 ? 0 : 1;
}
