// RepairTest - proves the decision layer and the retention rule without
// launching Skyrim.  RepairPolicy.h and Records.h are engine-free on purpose:
// a rule that cannot be tested is a rule nobody can trust.

#include "Records.h"
#include "RepairPolicy.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int s_checks = 0;
	int s_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		++s_checks;
		if (!a_ok) {
			++s_failures;
			std::printf("FAIL: %s\n", a_what);
		}
	}

	namespace RP = S3U::RepairPolicy;
	namespace RC = S3U::Records;
	namespace fs = std::filesystem;

	void Write(const fs::path& a_path, std::string_view a_text)
	{
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out << a_text;
	}

	void TestClassify()
	{
		std::printf("[reference verdicts]\n");

		RP::RefFacts clean;
		clean.baseMissing = true;
		Check(RP::Classify(clean) == RP::RefAction::kClean,
			"a dangling ref with no live handles is cleanable");
		Check(RP::ShouldClean(RP::Classify(clean)), "kClean is actionable");

		auto f = clean; f.player = true;
		Check(RP::Classify(f) == RP::RefAction::kKeepPlayer,
			"player ref is never deleted");

		f = clean; f.runtimeForm = true;
		Check(RP::Classify(f) == RP::RefAction::kKeepRuntimeForm,
			"0xFF runtime object is kept");

		f = clean; f.questAlias = true;
		Check(RP::Classify(f) == RP::RefAction::kKeepQuestAlias,
			"quest alias target is kept");

		f = clean; f.scripted = true;
		Check(RP::Classify(f) == RP::RefAction::kKeepScripted,
			"scripted ref is kept (VM handle would dangle)");

		f = clean; f.deleted = true;
		Check(RP::Classify(f) == RP::RefAction::kKeepAlreadyDeleted,
			"already-deleted ref is not processed twice");

		RP::RefFacts healthy;
		Check(RP::Classify(healthy) == RP::RefAction::kKeepBasePresent,
			"a ref with a valid base object is never touched");

		// Precedence: deleted beats player, player beats runtime, and so on.
		RP::RefFacts multiple;
		multiple.baseMissing = true;
		multiple.deleted = true;
		multiple.player = true;
		multiple.questAlias = true;
		Check(RP::Classify(multiple) == RP::RefAction::kKeepAlreadyDeleted,
			"verdict precedence is deterministic");

		Check(std::string{ RP::Reason(RP::RefAction::kKeepQuestAlias) }.find("任务") != std::string::npos,
			"every skip has a human-readable reason");
	}

	void TestKeyMatching()
	{
		std::printf("[state receipt pruning]\n");
		const std::vector<std::string> missing{ "better vampires.esp", "dracula.esp" };

		Check(RP::KeyNamesMissingPlugin("init/applied/better vampires.esp", missing),
			"applied receipt of a removed plugin is identified");
		Check(RP::KeyNamesMissingPlugin("migrate/dracula.esp", missing),
			"migration receipt of a removed plugin is identified");
		Check(RP::KeyNamesMissingPlugin("INIT/APPLIED/BETTER VAMPIRES.ESP", missing),
			"matching is case-insensitive");
		Check(!RP::KeyNamesMissingPlugin("init/applied/skyrim.esm", missing),
			"receipt of an installed plugin is kept");
		Check(!RP::KeyNamesMissingPlugin("loadorder/list", missing),
			"the load-order baseline is never pruned as a receipt");
		Check(!RP::KeyNamesMissingPlugin("trend/history", missing),
			"unrelated keys are never pruned");
		Check(!RP::KeyNamesMissingPlugin("init/applied/better vampires.esp", {}),
			"an empty missing set prunes nothing");
		Check(!RP::KeyNamesMissingPlugin("init/applied/", missing),
			"an empty tail is not a plugin name");
	}

	void TestBaseline()
	{
		std::printf("[baseline verification]\n");
		const std::vector<std::string> missing{ "gone.esp" };
		const std::string dirty =
			"skyrim.esm\t0\t0\n"
			"gone.esp\t7\t0\n"
			"another.esl\t0\t1\n";
		const std::string clean =
			"skyrim.esm\t0\t0\n"
			"another.esl\t0\t1\n";
		Check(!RP::BaselineIsClean(dirty, missing),
			"baseline still naming a removed plugin is not clean");
		Check(RP::BaselineIsClean(clean, missing),
			"rewritten baseline without the plugin is clean");
		Check(RP::BaselineIsClean(dirty, {}),
			"no missing plugins means the baseline is trivially clean");
	}

	void TestOutcome()
	{
		std::printf("[success verdict and prompt]\n");
		RP::Outcome o;
		o.refsCleaned    = 3;
		o.keysPurged     = 2;
		o.missingBefore  = 2;
		o.baselineClean  = true;
		o.stateCommitted = true;
		o.verificationRan = true;
		Check(RP::Changed(o), "a pass that cleaned something counts as changed");
		Check(RP::Succeeded(o), "verified clean pass succeeds");

		const auto prompt = RP::Prompt(o);
		Check(prompt.find("保存当前存档") != std::string::npos,
			"prompt tells the player to save the current save");
		Check(prompt.find("重新载入") != std::string::npos,
			"prompt tells the player to reload");
		Check(prompt.find("3") != std::string::npos && prompt.find("2") != std::string::npos,
			"prompt names what was cleaned");
		Check(RP::ShortPrompt(o).find("保存") != std::string::npos,
			"HUD fallback still carries the instruction");

		auto unverified = o; unverified.verificationRan = false;
		Check(!RP::Succeeded(unverified), "unverified pass is not announced as success");
		auto dirty = o; dirty.baselineClean = false;
		Check(!RP::Succeeded(dirty), "a stale baseline fails the verdict");
		auto uncommitted = o; uncommitted.stateCommitted = false;
		Check(!RP::Succeeded(uncommitted), "a failed state commit fails the verdict");
		auto residual = o; residual.residualDangling = true;
		Check(!RP::Succeeded(residual), "residual dangling refs fail the verdict");
		RP::Outcome idle;
		Check(!RP::Changed(idle) && !RP::Succeeded(idle),
			"a load with nothing to fix does not prompt");
	}

	void TestRecordNames()
	{
		std::printf("[retention names]\n");
		Check(!RP::IsStaleRecord("report-latest.txt"), "the one txt is kept");
		Check(!RP::IsStaleRecord("audit-latest.log"), "the one log is kept");
		Check(RP::IsStaleRecord("audit-20261106-012346.log"),
			"timestamped audit log is history");
		Check(RP::IsStaleRecord("modguard-latest.txt"),
			"the second report txt is history");
		Check(RP::IsStaleRecord("state-schema.txt"),
			"schema txt is history");
		Check(RP::IsStaleRecord("backup-index.txt"), "backup index is history");
		Check(!RP::IsStaleRecord("state.bin"), "state snapshot is not a record");
		Check(!RP::IsStaleRecord("init.wal"), "WAL receipt is not a record");
		Check(RP::IsBackupArtefact("Save1_xxx.ess.bak"), ".ess.bak is a backup artefact");
		Check(RP::IsBackupArtefact("backup-index.txt"), "backup index is an artefact");
		Check(!RP::IsBackupArtefact("Save1_xxx.ess"), "a real save is never swept");
		Check(!RP::IsStaleRecord("_示例_Example.ini.txt"),
			"migration manifest template is configuration, not history");
		Check(!RP::IsStaleRecord("MyFix.ini"), "ini config is never swept");
	}

	void TestPruneRealDirectory()
	{
		std::printf("[prune a real directory]\n");
		const auto temp = fs::temp_directory_path() / "3su-repair-test";
		std::error_code ec;
		fs::remove_all(temp, ec);
		fs::create_directories(temp / "Journal" / "ABC12345", ec);
		fs::create_directories(temp / "State", ec);
		fs::create_directories(temp / "Vault" / "ABC12345" / "history", ec);
		fs::create_directories(temp / "Migrations", ec);

		Write(temp / "report-latest.txt", "current report\n");
		Write(temp / "audit-latest.log", "current log\n");
		Write(temp / "modguard-latest.txt", "old\n");
		Write(temp / "audit-20261104-215445.log", "old\n");
		Write(temp / "audit-20261104-224018.log", "old\n");
		Write(temp / "audit-20261106-012346.log", "old\n");
		Write(temp / "backup-index.txt", "old\n");
		Write(temp / "State" / "state-schema.txt", "old\n");
		Write(temp / "State" / "state.bin", "KEEP");
		Write(temp / "Journal" / "ABC12345" / "init.log", "LEGACY RECEIPT");
		Write(temp / "Vault" / "ABC12345" / "history" / "gen-3.bin", "old\n");
		Write(temp / "Save1.ess.bak", "old backup\n");
		Write(temp / "Migrations" / "_示例_Example.ini.txt", "config template\n");
		Write(temp / "Migrations" / "stale-note.txt", "an old migration note\n");

		const auto sweep = RC::Prune(temp);
		// 3 timestamped logs + modguard txt + backup index + schema txt +
		// .ess.bak + rotated history generation = 8 stale artefacts.
		Check(sweep.removed == 8, "all 8 historical records and backups removed");
		Check(fs::exists(temp / "report-latest.txt"), "the one txt survives");
		Check(fs::exists(temp / "audit-latest.log"), "the one log survives");
		Check(fs::exists(temp / "State" / "state.bin"), "functional state survives");
		Check(fs::exists(temp / "Migrations" / "_示例_Example.ini.txt"),
			"migration manifest template survives (Migrations/ is never swept)");
		Check(fs::exists(temp / "Migrations" / "stale-note.txt"),
			"Migrations/ subtree is skipped entirely");
		Check(!fs::exists(temp / "modguard-latest.txt"), "second txt is gone");
		Check(!fs::exists(temp / "audit-20261106-012346.log"), "timestamped log is gone");
		Check(!fs::exists(temp / "backup-index.txt"), "backup index is gone");
		Check(!fs::exists(temp / "State" / "state-schema.txt"), "schema txt is gone");

		// The legacy receipt must be RENAMED, never deleted.
		Check(sweep.migrated == 1, "legacy init.log was migrated");
		Check(fs::exists(temp / "Journal" / "ABC12345" / "init.wal"),
			"receipt survives under the new name");
		{
			std::ifstream in(temp / "Journal" / "ABC12345" / "init.wal");
			std::string body;
			std::getline(in, body);
			Check(body == "LEGACY RECEIPT", "receipt content is intact after migration");
		}
		Check(!fs::exists(temp / "Vault" / "ABC12345" / "history"),
			"history directory is gone");

		// Idempotent: a second sweep finds nothing to remove and does not
		// touch the two live files.
		const auto again = RC::Prune(temp);
		Check(again.removed == 0 && again.migrated == 0, "second sweep is a no-op");
		{
			std::ifstream in(temp / "report-latest.txt");
			std::string body;
			std::getline(in, body);
			Check(body == "current report", "live report is never overwritten by pruning");
		}

		// Legacy backup directory outside the data dir is swept too, but the
		// real .ess next to it is not.
		const auto saves = fs::temp_directory_path() / "3su-repair-test-saves";
		fs::remove_all(saves, ec);
		fs::create_directories(saves / "3SUBackup", ec);
		Write(saves / "3SUBackup" / "Save1.ess.bak", "old");
		Write(saves / "3SUBackup" / "backup-index.txt", "old");
		Write(saves / "3SUBackup" / "Save1.ess", "REAL SAVE");
		const auto outside = RC::Prune(temp, saves / "3SUBackup");
		Check(outside.removed == 2, "only the two backup artefacts are removed");
		Check(fs::exists(saves / "3SUBackup" / "Save1.ess"),
			"a real .ess is never swept from the backup folder");

		fs::remove_all(temp, ec);
		fs::remove_all(saves, ec);
	}
}

int main()
{
	std::printf("== RepairTest ==\n");
	TestClassify();
	TestKeyMatching();
	TestBaseline();
	TestOutcome();
	TestRecordNames();
	TestPruneRealDirectory();
	std::printf("%d checks, %d failures -> %s\n",
		s_checks, s_failures, s_failures == 0 ? "ALL PASS" : "FAILED");
	return s_failures == 0 ? 0 : 1;
}
