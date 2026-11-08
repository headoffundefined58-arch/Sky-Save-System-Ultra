#pragma once

// -----------------------------------------------------------------------------
// RepairPolicy - the decision layer of load-time self-healing.
//
// This header is deliberately engine-free.  Every rule about WHAT may be
// removed and WHEN a repair may be called successful lives here, so the whole
// policy can be proven by tools/repair_test.cpp without launching Skyrim.
// Repair.cpp only carries out the verdicts; it never invents new ones.
//
// The distinction that matters is between a "dangling reference" and a
// "reference we are allowed to touch":
//
//   dangling  = its base object no longer resolves (the mod that owned it is
//               gone).  Nothing in the engine may dereference this safely.
//   removable = dangling AND not the player, not a runtime 0xFF form, not a
//               quest-alias target, not carrying Papyrus scripts, and not
//               already deleted.  Only these get SetDelete(true).
//
// Skipping the others is not timidity: deleting a reference that a live quest
// alias or a VM handle still points at is a direct CTD.  The residue that is
// safe to remove is exactly the residue a removed plugin leaves behind.
// -----------------------------------------------------------------------------

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace S3U::RepairPolicy
{
	inline constexpr const char* kReportTxt = "report-latest.txt";
	inline constexpr const char* kAuditLog  = "audit-latest.log";

	// -------------------------------------------------------------------------
	// Reference verdicts
	// -------------------------------------------------------------------------
	enum class RefAction : std::uint8_t
	{
		kClean = 0,
		kKeepBasePresent,
		kKeepRuntimeForm,
		kKeepPlayer,
		kKeepQuestAlias,
		kKeepScripted,
		kKeepAlreadyDeleted
	};

	struct RefFacts
	{
		bool baseMissing{ false };   // GetBaseObject() == nullptr
		bool runtimeForm{ false };   // 0xFF______, created at runtime
		bool player{ false };        // 0x14 / the player actor
		bool questAlias{ false };    // ExtraAliasInstanceArray present
		bool scripted{ false };      // VM attachedScripts has this handle
		bool deleted{ false };       // already IsDeleted / marked for deletion
	};

	[[nodiscard]] inline RefAction Classify(const RefFacts& a_f) noexcept
	{
		if (!a_f.baseMissing)  { return RefAction::kKeepBasePresent; }
		if (a_f.deleted)       { return RefAction::kKeepAlreadyDeleted; }
		if (a_f.player)        { return RefAction::kKeepPlayer; }
		if (a_f.runtimeForm)   { return RefAction::kKeepRuntimeForm; }
		if (a_f.questAlias)    { return RefAction::kKeepQuestAlias; }
		if (a_f.scripted)      { return RefAction::kKeepScripted; }
		return RefAction::kClean;
	}

	[[nodiscard]] inline bool ShouldClean(RefAction a_action) noexcept
	{
		return a_action == RefAction::kClean;
	}

	[[nodiscard]] inline const char* Reason(RefAction a_action) noexcept
	{
		switch (a_action) {
		case RefAction::kClean:              return "可清理";
		case RefAction::kKeepBasePresent:    return "基础对象正常，不是残差";
		case RefAction::kKeepAlreadyDeleted: return "已标记删除，无需重复处理";
		case RefAction::kKeepPlayer:         return "玩家引用，永不触碰";
		case RefAction::kKeepRuntimeForm:    return "运行时对象，可能有任务在用";
		case RefAction::kKeepQuestAlias:     return "任务别名仍指向它，删除会闪退";
		case RefAction::kKeepScripted:       return "挂有脚本实例，删除会破坏 VM 句柄";
		default:                             return "未知";
		}
	}

	// -------------------------------------------------------------------------
	// State keys owned by a plugin that is no longer installed.
	// Stored keys look like "init/applied/better vampires.esp" or
	// "migrate/<manifest>/applied".  Only the last path segment is a plugin
	// name, and the comparison is case-insensitive because load-order baselines
	// are lower-cased on write.
	// -------------------------------------------------------------------------
	inline std::string Lower(std::string_view a_view)
	{
		std::string out{ a_view };
		std::transform(out.begin(), out.end(), out.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	[[nodiscard]] inline bool KeyNamesMissingPlugin(
		std::string_view a_key, const std::vector<std::string>& a_missing)
	{
		if (a_missing.empty() || a_key.empty()) {
			return false;
		}
		// Only receipt namespaces may be pruned.  Deleting an arbitrary field
		// because its text happens to contain a plugin name would corrupt the
		// state; the schema owns those fields, not this pass.  The namespace
		// check is case-insensitive like the name check - a legacy key written
		// in upper case is still the same receipt.
		const auto lowerKey = Lower(a_key);
		const bool receipt =
			lowerKey.starts_with("init/") || lowerKey.starts_with("migrate/");
		if (!receipt) {
			return false;
		}

		const auto slash = lowerKey.find_last_of('/');
		const auto tail  = lowerKey.substr(slash == std::string::npos ? 0 : slash + 1);
		if (tail.empty()) {
			return false;
		}
		for (const auto& plugin : a_missing) {
			if (tail == Lower(plugin)) {
				return true;
			}
		}
		return false;
	}

	// A load-order baseline is "clean" once none of the missing plugin names
	// appear in it.  Lines are "name\tindex\tlight\n".
	[[nodiscard]] inline bool BaselineIsClean(
		std::string_view a_blob, const std::vector<std::string>& a_missing)
	{
		if (a_missing.empty()) {
			return true;
		}
		std::size_t start = 0;
		while (start < a_blob.size()) {
			auto end = a_blob.find('\n', start);
			if (end == std::string_view::npos) {
				end = a_blob.size();
			}
			const auto line = a_blob.substr(start, end - start);
			start = end + 1;
			const auto tab = line.find('\t');
			const auto name = Lower(line.substr(0, tab == std::string_view::npos ? line.size() : tab));
			for (const auto& plugin : a_missing) {
				if (name == Lower(plugin)) {
					return false;
				}
			}
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// Record retention: exactly one txt and one log, by name.  Anything else
	// with a .txt/.log extension at the data root is history and is removed.
	// Functional state (state.bin, *.wal) lives in subdirectories and is NOT a
	// record file, so it is never touched here.
	// -------------------------------------------------------------------------
	[[nodiscard]] inline bool IsStaleRecord(std::string_view a_fileName)
	{
		if (a_fileName == kReportTxt || a_fileName == kAuditLog) {
			return false;
		}
		// Migration manifests live under Migrations/ and are shipped as
		// *.ini.txt templates so Windows does not hide their extension.  They
		// are configuration, not records - deleting them would silently
		// disable the declarative migration feature.
		const auto lower = Lower(a_fileName);
		if (lower.ends_with(".ini.txt") || lower.ends_with(".ini")) {
			return false;
		}
		const auto dot = a_fileName.find_last_of('.');
		if (dot == std::string_view::npos) {
			return false;
		}
		const auto ext = Lower(a_fileName.substr(dot));
		return ext == ".txt" || ext == ".log";
	}

	[[nodiscard]] inline bool IsBackupArtefact(std::string_view a_fileName)
	{
		const auto lower = Lower(a_fileName);
		return lower == "backup-index.txt" || lower.ends_with(".bak") ||
		       lower.ends_with(".ess.bak");
	}

	// -------------------------------------------------------------------------
	// Success is a verdict, not a mood.  A repair is only announced when it
	// actually changed something AND the post-repair verification came back
	// clean.  Otherwise the player must not be told to save-and-reload.
	// -------------------------------------------------------------------------
	struct Outcome
	{
		std::size_t refsCleaned{ 0 };
		std::size_t keysPurged{ 0 };
		std::size_t missingBefore{ 0 };
		bool        baselineClean{ false };
		bool        stateCommitted{ false };
		bool        verificationRan{ false };
		bool        residualDangling{ false };  // still unhandled dangling refs
	};

	[[nodiscard]] inline bool Changed(const Outcome& a_o) noexcept
	{
		return a_o.refsCleaned > 0 || a_o.keysPurged > 0 || a_o.missingBefore > 0;
	}

	[[nodiscard]] inline bool Succeeded(const Outcome& a_o) noexcept
	{
		return Changed(a_o) && a_o.verificationRan && a_o.baselineClean &&
		       a_o.stateCommitted && !a_o.residualDangling;
	}

	[[nodiscard]] inline std::string Prompt(const Outcome& a_o)
	{
		// The instruction is not ceremony.  SetDelete changes the live
		// world; only a save serialises those changeforms into the .ess, and
		// only a reload proves the engine no longer resurrects them.
		std::string out = "存档修复完成 / SkySaveSystemUltra\n\n";
		out += "已清理 " + std::to_string(a_o.refsCleaned) + " 个失效引用、";
		out += std::to_string(a_o.keysPurged) + " 条残留记录；";
		out += "缺失 MOD " + std::to_string(a_o.missingBefore) + " 个已归零。\n\n";
		out += "请现在【保存当前存档】，然后【重新载入该存档】。\n";
		out += "保存会把清理结果写入 .ess，重新载入后修复才会彻底生效。\n";
		out += "完成这两步之前，请不要再卸载或新增 MOD。";
		return out;
	}

	[[nodiscard]] inline std::string ShortPrompt(const Outcome& a_o)
	{
		return "存档已修复 · 清理 " + std::to_string(a_o.refsCleaned) +
		       " 引用 / " + std::to_string(a_o.keysPurged) +
		       " 记录 · 请保存当前存档，再重新载入";
	}
}
