#pragma once

// -----------------------------------------------------------------------------
// Records - one report, one log, nothing else.
//
// The old build wrote a timestamped audit-YYYYMMDD-HHMMSS.log on every load,
// plus modguard-latest.txt, report-latest.txt, State/state-schema.txt,
// Vault/*/history/* and a rotated .ess.bak on every save.  After a few
// play sessions the data directory was mostly history, which is precisely
// what a save-repair tool must not accumulate: the player cannot tell which
// file describes the current save, and a stale "danger" report keeps being
// read as a live warning.
//
// This module owns retention as a hard rule:
//   * exactly one .txt  - report-latest.txt, overwritten in place
//   * exactly one .log  - audit-latest.log,  overwritten in place (no history)
//   * no rotated state snapshots, no backup index, no .bak
//
// It is plain std::filesystem with no engine and no spdlog, so the retention
// rule is proven by tools/repair_test.cpp against a real temporary directory.
// -----------------------------------------------------------------------------

#include "RepairPolicy.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace S3U::Records
{
	struct Sweep
	{
		std::size_t              removed{ 0 };
		std::size_t              migrated{ 0 };
		std::size_t              bytes{ 0 };
		std::vector<std::string> names;
		std::string              note;

		void Took(const std::filesystem::path& a_path, std::uintmax_t a_size)
		{
			++removed;
			bytes += static_cast<std::size_t>(a_size);
			if (names.size() < 40) {
				names.push_back(a_path.filename().string());
			}
		}
	};

	inline bool RemoveOne(const std::filesystem::path& a_path, Sweep& a_sweep)
	{
		std::error_code ec;
		const auto      size = std::filesystem::is_regular_file(a_path, ec) ?
		                           std::filesystem::file_size(a_path, ec) : 0;
		if (ec) {
			return false;
		}
		std::filesystem::remove(a_path, ec);
		if (ec) {
			return false;
		}
		a_sweep.Took(a_path, size);
		return true;
	}

	// Walk the data directory, but only ever delete files this plugin created:
	// .txt / .log records (except the two live names), .bak and backup indices.
	// State snapshots (state.bin), WAL receipts (*.wal) and migrations are
	// functional data, not records, and are never touched.
	inline Sweep Prune(const std::filesystem::path& a_dataDir,
		const std::filesystem::path& a_backupDir = {})
	{
		Sweep sweep;
		if (a_dataDir.empty() || !std::filesystem::is_directory(a_dataDir)) {
			sweep.note = "数据目录不存在，跳过清理";
			return sweep;
		}

		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(a_dataDir, ec), end;
		     !ec && it != end; it.increment(ec)) {
			// Migrations/ holds user-authored declarative manifests.  It is
			// configuration, never generated history.
			if (it->is_directory(ec) && !ec &&
			    RepairPolicy::Lower(it->path().filename().string()) == "migrations") {
				it.disable_recursion_pending();
				continue;
			}
			if (!it->is_regular_file(ec) || ec) {
				continue;
			}
			const auto& path = it->path();
			const auto  name = path.filename().string();

			// The pre-2.0.0 write-ahead journal was named init.log.  It is a
			// functional receipt, NOT a historical log: deleting it would make
			// load-time initialisation hand out starting items a second time.
			// Rename it to the new .wal name instead of sweeping it.
			if (RepairPolicy::Lower(name) == "init.log") {
				auto wal = std::filesystem::path{ path };
				wal.replace_extension(".wal");
				if (!std::filesystem::exists(wal, ec)) {
					std::filesystem::rename(path, wal, ec);
					if (!ec) {
						++sweep.migrated;
						continue;
					}
				}
				// A .wal already exists: the old .log is genuinely stale.
				ec.clear();
				RemoveOne(std::filesystem::path{ path }, sweep);
				continue;
			}

			if (RepairPolicy::IsBackupArtefact(name) ||
			    RepairPolicy::IsStaleRecord(name)) {
				RemoveOne(path, sweep);
			}
		}

		// A backup directory left by an older build may sit outside the data
		// directory (next to the .ess files).  Only plugin-generated artefacts
		// are removed there - never a real save.
		if (!a_backupDir.empty() && std::filesystem::is_directory(a_backupDir, ec)) {
			for (std::filesystem::directory_iterator it(a_backupDir, ec), end;
			     !ec && it != end; it.increment(ec)) {
				if (!it->is_regular_file(ec) || ec) {
					continue;
				}
				const auto name = it->path().filename().string();
				if (RepairPolicy::IsBackupArtefact(name)) {
					RemoveOne(it->path(), sweep);
				}
			}
		}

		// History directories are two levels down: Vault/<identity>/history,
		// State/<identity>/history.  A rotated snapshot is history regardless
		// of its extension, so the whole subtree goes.
		std::vector<std::filesystem::path> historyDirs;
		for (const char* sub : { "Vault", "State", "Journal" }) {
			const auto base = a_dataDir / sub;
			if (!std::filesystem::is_directory(base, ec)) {
				continue;
			}
			for (std::filesystem::directory_iterator group(base, ec), gend;
			     !ec && group != gend; group.increment(ec)) {
				if (!group->is_directory(ec) || ec) {
					continue;
				}
				const auto candidate = group->path() / "history";
				if (std::filesystem::is_directory(candidate, ec)) {
					historyDirs.push_back(candidate);
				}
			}
		}
		for (const auto& dir : historyDirs) {
			for (std::filesystem::recursive_directory_iterator h(dir, ec), hend;
			     !ec && h != hend; h.increment(ec)) {
				if (h->is_regular_file(ec) && !ec) {
					RemoveOne(h->path(), sweep);
				}
			}
			ec.clear();
			std::filesystem::remove_all(dir, ec);
		}

		if (sweep.removed == 0 && sweep.migrated == 0) {
			sweep.note = "无历史记录需要清理";
		} else {
			sweep.note = "已清理 " + std::to_string(sweep.removed) +
			             " 份历史记录/备份";
			if (sweep.migrated > 0) {
				sweep.note += "，迁移 " + std::to_string(sweep.migrated) + " 份收据";
			}
		}
		return sweep;
	}
}
