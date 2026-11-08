#include "PCH.h"

#include "Repair.h"

#include "Audit.h"
#include "Ini.h"
#include "InitEngine.h"
#include "Journal.h"
#include "LoadOrder.h"
#include "Notify.h"
#include "Records.h"
#include "RepairPolicy.h"
#include "SaveGuard.h"
#include "Vault.h"

#include "RE/M/Misc.h"
#include "SKSE/API.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <ostream>
#include <unordered_set>

namespace S3U::Repair
{
	namespace
	{
		Result     s_result;
		std::mutex s_mutex;
		std::size_t s_recordsRemoved{ 0 };  // lifetime counter, not per-load

		void Record(std::string a_line)
		{
			if (s_result.lines.size() < 60) {
				s_result.lines.push_back(std::move(a_line));
			}
		}

		// VM handles are uint64.  For a loaded, static reference the low 32
		// bits are the FormID; runtime objects use an encoded handle.  Keeping
		// just the low word makes the lookup work for both without having to
		// reproduce the engine's handle encoding here.
		std::unordered_set<std::uint32_t> ScriptedHandles()
		{
			std::unordered_set<std::uint32_t> out;
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) {
				return out;
			}
			// No lock: at this point (post-load, main-thread task) the VM is
			// quiescent.  Taking its spin lock from inside a task that may
			// itself be dispatched by a VM operation risks a deadlock.
			for (const auto& [handle, scripts] : vm->attachedScripts) {
				(void)scripts;
				out.insert(static_cast<std::uint32_t>(handle & 0xFFFFFFFFull));
			}
			return out;
		}

		struct RefHit
		{
			RE::TESObjectREFR* ptr{ nullptr };
			RepairPolicy::RefFacts facts;
			RepairPolicy::RefAction action{ RepairPolicy::RefAction::kKeepBasePresent };
		};

		// PASS 1 - observe only.  ForEachReference iterates the cell's own
		// container, so disabling/deleting a ref from inside the callback would
		// invalidate that iteration.  Every candidate is collected first.
		std::vector<RefHit> ScanDangling(const std::unordered_set<std::uint32_t>& a_scripted)
		{
			std::vector<RefHit> hits;
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return hits;
			}

			const auto inspect = [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->GetBaseObject() != nullptr) {
					return;
				}
				RepairPolicy::RefFacts facts;
				facts.baseMissing = true;
				facts.runtimeForm = a_ref->IsDynamicForm();
				facts.player      = a_ref->IsPlayerRef() || a_ref->IsPlayer();
				facts.questAlias  = a_ref->extraList.HasType<RE::ExtraAliasInstanceArray>();
				facts.scripted    = a_scripted.contains(a_ref->GetFormID());
				facts.deleted     = a_ref->IsDeleted() || a_ref->IsMarkedForDeletion();

				RefHit hit;
				hit.ptr    = a_ref;
				hit.facts  = facts;
				hit.action = RepairPolicy::Classify(facts);
				hits.push_back(hit);
			};

			const auto walk = [&](RE::TESObjectCELL* a_cell) {
				if (!a_cell || !a_cell->IsAttached()) {
					return;
				}
				a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					inspect(a_ref);
					return RE::BSContainer::ForEachResult::kContinue;
				});
			};

			for (std::uint32_t i = 0; i < dh->interiorCells.size(); ++i) {
				walk(dh->interiorCells[i]);
			}
			if (auto* tes = RE::TES::GetSingleton(); tes && tes->gridCells) {
				auto*      grid = tes->gridCells;
				const auto len  = grid->length;
				for (std::uint32_t x = 0; x < len; ++x) {
					for (std::uint32_t y = 0; y < len; ++y) {
						walk(grid->GetCell(x, y));
					}
				}
			}
			return hits;
		}

		// PASS 2 - mutate.  SetDelete(true) sets the kDeleted flag and calls
		// AddChange, so the engine serialises this reference as a deleted
		// changeform on the next save and never instantiates it on the reload.
		//
		// Disable() is deliberately NOT called.  It walks the 3D / NiNode
		// graph, and on a reference whose base object no longer resolves that
		// is the single most likely place to take an access violation - which
		// a C++ try/catch cannot catch.  A ref with no base object has no 3D
		// to remove anyway, so Disable would buy nothing and risk a CTD.
		std::size_t ApplyClean(const std::vector<RefHit>& a_hits, Result& a_out)
		{
			// A single pass is bounded.  A save with tens of thousands of
			// dangling refs is a different problem (it needs an offline
			// rewriter) and must not stall the game loop here.
			constexpr std::size_t kMaxPerLoad = 512;
			std::size_t cleaned = 0;

			for (const auto& hit : a_hits) {
				if (!RepairPolicy::ShouldClean(hit.action)) {
					++a_out.refsSkipped;
					switch (hit.action) {
					case RepairPolicy::RefAction::kKeepQuestAlias:  ++a_out.skippedAlias; break;
					case RepairPolicy::RefAction::kKeepScripted:    ++a_out.skippedScripted; break;
					case RepairPolicy::RefAction::kKeepRuntimeForm: ++a_out.skippedRuntime; break;
					default: break;
					}
					continue;
				}
				if (cleaned >= kMaxPerLoad || !hit.ptr) {
					continue;
				}
				try {
					hit.ptr->SetDelete(true);
					++cleaned;
				} catch (const std::exception& e) {
					Record("跳过一个引用：引擎调用异常 " + std::string{ e.what() });
				} catch (...) {
					Record("跳过一个引用：引擎调用异常");
				}
			}
			return cleaned;
		}

		// State receipts are keyed by plugin.  A receipt for a plugin that no
		// longer exists is pure residue: it can never be honoured again, and it
		// would suppress initialisation if that plugin is ever reinstalled.
		std::size_t PurgeStateReceipts(const std::vector<std::string>& a_missing)
		{
			if (a_missing.empty()) {
				return 0;
			}
			std::size_t purged = 0;
			for (const char* prefix : { "init/", "migrate/" }) {
				for (const auto& key : StateStore::Keys(prefix)) {
					if (!RepairPolicy::KeyNamesMissingPlugin(key, a_missing)) {
						continue;
					}
					if (StateStore::Erase(key)) {
						++purged;
						Record("清除状态收据 " + key);
					}
				}
			}
			return purged;
		}

		// A rollback receipt makes a reinstalled plugin retryable, instead of
		// being permanently blocked by an APPLIED record from the old install.
		std::size_t ResetJournalReceipts(const std::vector<std::string>& a_missing)
		{
			if (a_missing.empty()) {
				return 0;
			}
			const auto path = StateStore::CurrentWalPath();
			if (path.empty()) {
				return 0;
			}
			Journal::Log log{ path };
			log.Open();
			const auto save      = std::string{ StateStore::CurrentSaveName() };
			const auto character = std::string{ StateStore::CurrentIdentity() };
			std::size_t written  = 0;
			for (const auto& plugin : a_missing) {
				std::string note;
				const auto payload = Journal::MakePayload({
					{ "plugin", plugin },
					{ "save",   save },
					{ "char",   character },
					{ "reason", "plugin-removed" }
				});
				if (log.Append(Journal::Kind::kRollback, payload, note)) {
					++written;
				}
			}
			return written;
		}

		void Announce(const RepairPolicy::Outcome& a_outcome)
		{
			const auto text = RepairPolicy::Prompt(a_outcome);
			// CreateMessage rather than DebugMessageBox: the latter dereferences
			// GameSettingCollection's "sOk" without a null check.  An ASCII
			// button label is safe under every localisation, while the body is
			// UTF-8 and renders through the game's own fonts.
			try {
				RE::CreateMessage(text.c_str(), nullptr, 0, 4, 10, "OK", nullptr);
			} catch (...) {
				spdlog::warn("[Repair] message box unavailable");
			}
			Notify::Push(RepairPolicy::ShortPrompt(a_outcome));
			Notify::Push("请保存当前存档，然后重新载入该存档，修复才会固化");
			spdlog::info("[Repair] prompt shown: refs={} keys={} missing={}",
				a_outcome.refsCleaned, a_outcome.keysPurged, a_outcome.missingBefore);
		}
	}

	std::size_t PruneRecordsOnStartup()
	{
		const auto dir = Paths::EnsureDataDir();
		if (dir.empty()) {
			return 0;
		}
		// Only the data directory is swept at startup.  The legacy 3SUBackup
		// folder lives next to the .ess files and is not known until Guard has
		// resolved the save directory at pre-load time; SweepLegacyBackups()
		// handles that once the path is trustworthy.
		const auto sweep = Records::Prune(dir);
		s_recordsRemoved += sweep.removed;
		spdlog::info("[Repair] startup sweep: {}", sweep.note);
		return sweep.removed;
	}

	void SweepLegacyBackups()
	{
		const auto dir = Paths::EnsureDataDir();
		if (dir.empty()) {
			return;
		}
		const auto& guard = Guard::Last();
		const auto legacy = guard.saveDir.empty() ?
			std::filesystem::path{} : guard.saveDir / "3SUBackup";
		const auto sweep = Records::Prune(dir, legacy);
		s_recordsRemoved += sweep.removed;
		if (sweep.removed > 0 || sweep.migrated > 0) {
			spdlog::info("[Repair] {}", sweep.note);
		}
	}

	void PrepareOnLoad(std::function<void()> a_onFinished)
	{
		{
			std::lock_guard lock(s_mutex);
			s_result = Result{};
			s_result.enabled = Ini::Get().repairEnabled;
		}
		if (!Ini::Get().repairEnabled) {
			s_result.note = "载入期自愈已在 ini 中关闭";
			s_result.finished = true;
			return;
		}

		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			spdlog::warn("[Repair] no task interface; repair skipped");
			s_result.note = "SKSE 任务接口不可用，本次未执行修复";
			s_result.finished = true;
			return;
		}

		// Capture the risk picture NOW, before any baseline is rewritten.
		const std::vector<std::string> missing = LoadOrder::Last().missing;

		tasks->AddTask([missing, cb = std::move(a_onFinished)]() mutable {
			Result local;
			local.ran      = true;
			local.enabled  = true;
			local.recordsRemoved = s_recordsRemoved;
			local.missingBefore = missing.size();
			local.missingNames  = missing;

			try {
				const auto scripted = ScriptedHandles();
				auto hits = ScanDangling(scripted);
				local.danglingFound = hits.size();
				local.refsCleaned   = ApplyClean(hits, local);

				local.keysPurged     = PurgeStateReceipts(missing);
				local.receiptsReset  = ResetJournalReceipts(missing);

				// Safe initialisation: adopt the CURRENT load order as the new
				// baseline.  From the next load on, this save no longer claims
				// to depend on plugins that are gone.  InitEngine has already
				// captured its own "added" list synchronously, so rewriting the
				// baseline here cannot swallow a newly installed mod's startup.
				if (!missing.empty() || local.refsCleaned > 0) {
					LoadOrder::StoreToVault();
					local.baselineRewritten = true;
					Record("已把当前加载顺序写为新基线，缺失插件归零");
				}

				// ---- verification -------------------------------------------
				std::string blob;
				StateStore::GetString("loadorder/list", blob);
				local.baselineClean = RepairPolicy::BaselineIsClean(blob, missing);
				local.stateCommitted = StateStore::Commit();

				const auto after = ScanDangling(scripted);
				local.residualDangling = static_cast<std::size_t>(
					std::count_if(after.begin(), after.end(), [](const RefHit& h) {
						return RepairPolicy::ShouldClean(h.action);
					}));
				local.verified = true;

				RepairPolicy::Outcome outcome;
				outcome.refsCleaned      = local.refsCleaned;
				outcome.keysPurged       = local.keysPurged;
				outcome.missingBefore    = local.missingBefore;
				outcome.baselineClean    = local.baselineClean;
				outcome.stateCommitted   = local.stateCommitted;
				outcome.verificationRan  = local.verified;
				outcome.residualDangling = local.residualDangling > 0;

				local.success = RepairPolicy::Succeeded(outcome);
				if (local.success && Ini::Get().repairPrompt) {
					Announce(outcome);
					local.promptShown = true;
				}

				if (!missing.empty()) {
					Record("缺失插件 " + std::to_string(missing.size()) + " 个：" +
						(missing.size() > 6 ?
							missing.front() + " 等" :
							[&] { std::string s; for (const auto& m : missing) { s += m + " "; } return s; }()));
				}
				if (local.danglingFound == 0) {
					Record("已加载单元里没有悬空引用");
				} else {
					Record("悬空引用 " + std::to_string(local.danglingFound) +
						" 个，清理 " + std::to_string(local.refsCleaned) +
						" 个，按安全规则保留 " + std::to_string(local.refsSkipped) + " 个");
				}

				if (local.success) {
					local.note = "修复完成并通过验证；请保存当前存档后重新载入";
				} else if (!RepairPolicy::Changed(outcome)) {
					local.note = "本次没有需要修复的残留数据，存档可直接继续";
				} else {
					local.note = "修复未通过完整验证，详见下方明细；本次不提示保存重载";
				}

				spdlog::info("[Repair] done: missing={} dangling={} cleaned={} skipped={} "
					"keys={} rollback={} baseline={} commit={} residual={} success={}",
					local.missingBefore, local.danglingFound, local.refsCleaned,
					local.refsSkipped, local.keysPurged, local.receiptsReset,
					local.baselineClean, local.stateCommitted,
					local.residualDangling, local.success);
			} catch (const std::exception& e) {
				local.note = std::string{ "修复任务异常: " } + e.what();
				spdlog::error("[Repair] deferred pass threw: {}", e.what());
			} catch (...) {
				local.note = "修复任务异常（未知）";
				spdlog::error("[Repair] deferred pass threw (unknown)");
			}

			local.finished = true;
			{
				std::lock_guard lock(s_mutex);
				s_result = std::move(local);
			}
			if (cb) {
				try { cb(); } catch (...) {}
			}
		});
	}

	const Result& Last()
	{
		return s_result;
	}

	void WriteReportSection(std::ostream& a_out)
	{
		Result r;
		{
			std::lock_guard lock(s_mutex);
			r = s_result;
		}

		a_out << "[载入期自愈 / Load-time repair]\n";
		if (!r.ran) {
			a_out << "  状态       : " << (r.note.empty() ? "未执行" : r.note) << "\n\n";
			return;
		}
		a_out << "  规则       : 缺失 MOD 的悬空引用即时标记为删除；状态收据与初始化收据同步清除；\n"
		         "               然后把当前加载顺序写为新基线，完成安全初始化。\n";
		a_out << "  缺失 MOD   : " << r.missingBefore << " 个\n";
		if (!r.missingNames.empty()) {
			const auto limit = std::min<std::size_t>(r.missingNames.size(), 12);
			for (std::size_t i = 0; i < limit; ++i) {
				a_out << "    - " << r.missingNames[i] << "\n";
			}
			if (r.missingNames.size() > limit) {
				a_out << "    ... 另有 " << (r.missingNames.size() - limit) << " 个\n";
			}
		}
		a_out << "  悬空引用   : " << r.danglingFound << " 个（当前已加载单元）\n";
		a_out << "  已清理     : " << r.refsCleaned << " 个\n";
		a_out << "  安全跳过   : " << r.refsSkipped << " 个"
		      << "（任务别名 " << r.skippedAlias
		      << " · 带脚本 " << r.skippedScripted
		      << " · 运行时 " << r.skippedRuntime << "）\n";
		a_out << "  状态收据   : 清除 " << r.keysPurged << " 条 · 作废初始化收据 "
		      << r.receiptsReset << " 条\n";
		a_out << "  基线重写   : " << (r.baselineRewritten ? "是" : "无需") << "\n";
		a_out << "  验证       : 基线干净 " << (r.baselineClean ? "通过" : "失败")
		      << " · 状态回读 " << (r.stateCommitted ? "通过" : "失败")
		      << " · 剩余可清理悬空 " << r.residualDangling << "\n";
		a_out << "  提示       : " << (r.promptShown ? "已弹出，引导保存并重载" : "未弹出")
		      << "\n";
		a_out << "  记录清理   : 累计删除 " << r.recordsRemoved
		      << " 份历史记录/备份；目录内仅保留 report-latest.txt 与 audit-latest.log\n";
		a_out << "  结论       : " << (r.note.empty() ? "—" : r.note) << "\n";
		if (!r.lines.empty()) {
			a_out << "  明细:\n";
			for (const auto& line : r.lines) {
				a_out << "    - " << line << "\n";
			}
		}
		a_out << "\n  说明: 清理作用于当前已加载单元；标记删除的引用会在下一次保存时\n"
		         "        写入 .ess 成为 deleted changeform，重新载入后不再被实例化。\n"
		         "        本插件不改写 .ess 字节，因此「保存 → 重载」是修复生效的必要步骤。\n\n";
	}

	void WriteActionLog(const std::filesystem::path& a_dir)
	{
		if (a_dir.empty()) {
			return;
		}
		std::ofstream out(a_dir / RepairPolicy::kAuditLog,
			std::ios::binary | std::ios::trunc);
		if (!out.is_open()) {
			return;
		}
		Result r;
		{
			std::lock_guard lock(s_mutex);
			r = s_result;
		}
		out << "SkySaveSystemUltra 载入期自愈记录（最新一次，覆盖式，不保留历史）\n";
		out << "结果: " << (r.success ? "修复成功" : (r.ran ? "无需修复或未通过验证" : "未执行"))
		    << "\n";
		out << "缺失MOD=" << r.missingBefore
		    << " 悬空引用=" << r.danglingFound
		    << " 已清理=" << r.refsCleaned
		    << " 安全跳过=" << r.refsSkipped
		    << " 状态收据=" << r.keysPurged
		    << " 作废收据=" << r.receiptsReset
		    << " 基线重写=" << (r.baselineRewritten ? 1 : 0)
		    << " 验证=" << (r.verified ? 1 : 0)
		    << " 提示=" << (r.promptShown ? 1 : 0) << "\n";
		for (const auto& name : r.missingNames) {
			out << "missing-plugin: " << name << "\n";
		}
		for (const auto& line : r.lines) {
			out << "action: " << line << "\n";
		}
		if (!r.note.empty()) {
			out << "note: " << r.note << "\n";
		}
	}
}
