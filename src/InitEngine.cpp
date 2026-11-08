#include "PCH.h"

#include "InitEngine.h"

#include "Ini.h"
#include "Journal.h"
#include "LoadOrder.h"
#include "Vault.h"

#include "SKSE/API.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace S3U::InitEngine
{
	namespace
	{
		Result                        s_result;
		std::shared_ptr<Journal::Log> s_log;
		std::vector<std::string>      s_sessionApplied;
		std::mutex                    s_mutex;

		std::string ToLower(std::string_view a_view)
		{
			std::string out{ a_view };
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		// Resolve a form to the plugin that owns it in O(1).  FormRef's
		// MakeFormRef deliberately walks only the full plugin array, which is
		// fine for stable references but cannot name an ESL - and 163 of the
		// 190 plugins in this load order are ESLs.  An ESL startup quest would
		// therefore be invisible to initialisation.
		std::string OwnerName(RE::FormID a_formID)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh || a_formID == 0) {
				return {};
			}

			const auto high = a_formID >> 24;
			if (high == 0xFE) {
				const auto light = (a_formID >> 12) & 0xFFFu;
				auto**     mods  = dh->GetLoadedLightMods();
				if (mods && light < dh->GetLoadedLightModCount() && mods[light]) {
					return ToLower(mods[light]->GetFilename());
				}
				return {};
			}

			auto** mods = dh->GetLoadedMods();
			if (mods && high < dh->GetLoadedModCount() && mods[high]) {
				return ToLower(mods[high]->GetFilename());
			}
			return {};
		}

		// Receipts are per character on disk (one journal per character) but
		// per save in meaning, so the path stays character-scoped while every
		// record inside carries its own save binding.
		//
		// The path comes from StateStore, which is also the module that
		// replays this log into the kWal fields.  Two modules each computing
		// their own path is how 0.9.0 ended up appending to one file and
		// reading another, leaving init/receipts/* permanently empty.
		std::filesystem::path JournalPath()
		{
			if (auto path = StateStore::CurrentWalPath(); !path.empty()) {
				return path;
			}
			const auto dir = Paths::EnsureDataDir();
			if (dir.empty()) {
				return {};
			}
			auto id = StateStore::CurrentIdentity();
			if (id.empty()) {
				id = "default";
			}
			return dir / "Journal" / id / "init.wal";
		}

		struct Unit
		{
			std::string               plugin;
			std::vector<RE::TESQuest*> quests;
		};

		void Record(std::string a_line)
		{
			if (s_result.lines.size() < 60) {
				s_result.lines.push_back(std::move(a_line));
			}
		}

		std::string JoinIds(const std::vector<RE::FormID>& a_ids)
		{
			std::string out;
			char        buffer[16]{};
			for (const auto id : a_ids) {
				std::snprintf(buffer, sizeof(buffer), "%08X", id);
				if (!out.empty()) {
					out.push_back(',');
				}
				out += buffer;
			}
			return out;
		}

		// Find the startup quests of the plugins that were not present when
		// this save was written.  "Start Game Enabled" is the engine's own
		// New-Game hook, so starting those quests reproduces exactly the path
		// the engine would have taken on a fresh character.  Quests that are
		// already running are left alone.
		std::vector<Unit> Discover(const std::vector<std::string>& a_plugins)
		{
			std::vector<Unit> out;
			auto*             dh = RE::TESDataHandler::GetSingleton();
			if (!dh || a_plugins.empty()) {
				return out;
			}

			std::unordered_map<std::string, std::size_t> index;
			for (const auto& name : a_plugins) {
				index.emplace(name, out.size());
				out.push_back(Unit{ name, {} });
			}

			const auto questIdx = std::to_underlying(RE::FormType::Quest);
			for (auto* form : dh->formArrays[questIdx]) {
				auto* quest = form ? form->As<RE::TESQuest>() : nullptr;
				if (!quest || quest->IsDynamicForm()) {
					continue;
				}

				const auto owner = OwnerName(quest->GetFormID());
				const auto it    = index.find(owner);
				if (owner.empty() || it == index.end()) {
					continue;
				}
				if (quest->IsRunning() || quest->IsStarting()) {
					continue;
				}
				if (!quest->StartsEnabled()) {
					continue;
				}
				out[it->second].quests.push_back(quest);
			}

			// Drop plugins with no startup quest: they need no init and should
			// not create a receipt that would suppress a future real attempt.
			out.erase(std::remove_if(out.begin(), out.end(),
				[](const Unit& u) { return u.quests.empty(); }), out.end());
			return out;
		}

		// Compensating transaction.  Only the quests THIS pass started are
		// touched: a quest that was already running belongs to the save, not
		// to us, and stopping it would destroy the player's progress.
		std::uint32_t RollBack(const std::vector<RE::FormID>& a_started)
		{
			std::uint32_t reversed = 0;
			for (const auto id : a_started) {
				auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(id);
				if (!quest) {
					continue;
				}
				try {
					quest->Stop();
					quest->Reset();
					if (!quest->IsRunning()) {
						++reversed;
					}
				} catch (...) {
					spdlog::error("[Init] rollback of quest {:08X} threw", id);
				}
			}
			return reversed;
		}

		// The log is passed by shared_ptr rather than reached through the
		// global: a fast reload can replace s_log while this task is queued,
		// and a receipt must never land in another character's journal.
		void Execute(std::vector<Unit> a_units, std::shared_ptr<Journal::Log> a_log,
			std::string a_save, std::string a_character, std::function<void()> a_onFinished)
		{
			{
				std::lock_guard lock(s_mutex);

				for (auto& unit : a_units) {
					std::string note;

					// ---- 1. write-ahead -------------------------------------
					const auto pending = Journal::MakePayload({
						{ "plugin", unit.plugin },
						{ "save",   a_save },
						{ "char",   a_character },
						{ "quests", std::to_string(unit.quests.size()) }
					});
					if (!a_log->Append(Journal::Kind::kInitPending, pending, note)) {
						++s_result.failures;
						Record("收据写入失败 · " + unit.plugin + " · " + note);
						spdlog::error("[Init] cannot write pending receipt for {}: {}",
							unit.plugin, note);
						continue;
					}

					// ---- 2. execute -----------------------------------------
					std::vector<RE::FormID> startedIds;
					std::uint32_t           already = 0;
					std::uint32_t           failed  = 0;
					for (auto* quest : unit.quests) {
						if (!quest) {
							++failed;
							continue;
						}
						try {
							if (quest->IsRunning()) {
								++already;   // someone else started it; not ours
							} else if (quest->Start()) {
								startedIds.push_back(quest->GetFormID());
							} else {
								++failed;
							}
						} catch (...) {
							++failed;
						}
					}

					// ---- 3. verify ------------------------------------------
					// Start() returning true only means the call was accepted.
					// The quest has to actually be running, otherwise the
					// "applied" receipt would be a lie the next load inherits.
					std::uint32_t verified = 0;
					for (const auto id : startedIds) {
						auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(id);
						if (quest && quest->IsRunning()) {
							++verified;
						}
					}
					const bool ok = failed == 0 && verified == startedIds.size();

					s_result.questsStarted += static_cast<std::uint32_t>(startedIds.size());
					s_result.questsVerified += verified;
					s_result.failures += failed;
					s_result.questsFound += static_cast<std::uint32_t>(unit.quests.size());

					if (ok) {
						// ---- 4a. commit: applied ----------------------------
						const auto outcome = Journal::MakePayload({
							{ "plugin",   unit.plugin },
							{ "save",     a_save },
							{ "char",     a_character },
							{ "quests",   std::to_string(unit.quests.size()) },
							{ "started",  std::to_string(startedIds.size()) },
							{ "verified", std::to_string(verified) },
							{ "version",  StateStore::kPluginVersion }
						});
						if (!a_log->Append(Journal::Kind::kInitApplied, outcome, note)) {
							// Without a durable receipt the next load would run
							// this again.  Compensate instead of pretending.
							spdlog::error("[Init] cannot write applied receipt for {}: {}",
								unit.plugin, note);
							const auto undone = RollBack(startedIds);
							++s_result.rolledBack;
							s_result.questsStarted -= static_cast<std::uint32_t>(startedIds.size());
							s_result.questsStarted += undone;
							Record("收据落盘失败，已回滚 " + unit.plugin +
								" · " + std::to_string(undone) + "/" +
								std::to_string(startedIds.size()) + " 个任务");
							continue;
						}
						s_sessionApplied.push_back(unit.plugin);
						++s_result.generation;
						Record(unit.plugin + " · 已启动并校验 " + std::to_string(verified) +
							" 个启动任务");
						spdlog::info("[Init] {} initialised: {}/{} started, {} verified",
							unit.plugin, startedIds.size(), unit.quests.size(), verified);
					} else {
						// ---- 4b. abort: roll back ---------------------------
						const auto undone = RollBack(startedIds);
						++s_result.rolledBack;
						const auto outcome = Journal::MakePayload({
							{ "plugin",   unit.plugin },
							{ "save",     a_save },
							{ "char",     a_character },
							{ "quests",   std::to_string(unit.quests.size()) },
							{ "started",  std::to_string(startedIds.size()) },
							{ "verified", std::to_string(verified) },
							{ "failed",   std::to_string(failed) },
							{ "reverted", std::to_string(undone) },
							{ "version",  StateStore::kPluginVersion }
						});
						if (!a_log->Append(Journal::Kind::kRollback, outcome, note)) {
							// A missing rollback record leaves the preceding
							// PENDING in force, which blocks retries - the safe
							// direction, since a retry could double-apply.
							spdlog::error("[Init] cannot write rollback receipt for {}: {}",
								unit.plugin, note);
						}
						Record(unit.plugin + " · 启动 " + std::to_string(startedIds.size()) +
							" 个、校验 " + std::to_string(verified) +
							" 个、失败 " + std::to_string(failed) +
							" 个 → 已回滚 " + std::to_string(undone) +
							" 个，下次载入可重试");
						spdlog::warn("[Init] {} aborted and rolled back: {}/{} verified, {} failed, {} reverted",
							unit.plugin, verified, startedIds.size(), failed, undone);
					}
				}

				s_result.finished = true;

				spdlog::info("[Init] pass done: units={} quests={} started={} verified={} failed={} rolledBack={}",
					s_result.units, s_result.questsFound, s_result.questsStarted,
					s_result.questsVerified, s_result.failures, s_result.rolledBack);
			}   // release s_mutex before the callback: it writes the report,
			    // and WriteReportSection takes the same lock.

			if (a_onFinished) {
				a_onFinished();
			}
		}

		// Every plugin this save can prove it has already initialised: the
		// receipts carried by the .ess itself, the ones in the write-ahead log
		// bound to this save, and the ones this session just performed.
		std::vector<std::string> CollectApplied(const std::shared_ptr<Journal::Log>& a_log,
			const std::string& a_save)
		{
			std::set<std::string> names;
			constexpr std::string_view kPrefix{ "init/applied/" };
			for (const auto& key : StateStore::Keys(kPrefix)) {
				names.insert(key.substr(kPrefix.size()));
			}
			if (a_log) {
				const auto view = Journal::DeriveInitStatesForSave(
					a_log->State().records, a_save);
				for (const auto& [plugin, state] : view.states) {
					if (state == Journal::InitState::kApplied) {
						names.insert(plugin);
					}
				}
			}
			for (const auto& plugin : s_sessionApplied) {
				names.insert(plugin);
			}
			return { names.begin(), names.end() };
		}
	}

	const Result& Last()
	{
		return s_result;
	}

	void PrepareOnLoad(std::function<void()> a_onFinished)
	{
		const auto save      = StateStore::CurrentSaveName();
		const auto character = StateStore::CurrentIdentity();

		{
			std::lock_guard lock(s_mutex);
			s_result = Result{};
			s_sessionApplied.clear();
			s_result.saveName = save;
		}

		if (!Ini::Get().initEnabled) {
			s_result.note = "载入期初始化已在 ini 中关闭";
			return;
		}

		const auto& diff = LoadOrder::Last();
		if (!diff.haveBaseline) {
			s_result.note = "这份存档里没有加载顺序基线，本次无法判定新增插件；保存一次后生效";
			return;
		}

		const auto path = JournalPath();
		if (path.empty()) {
			s_result.note = "无法确定数据目录，初始化收据不可用";
			return;
		}

		auto log = std::make_shared<Journal::Log>(path);
		s_log = log;
		const auto& state = s_log->Open();
		s_result.journalReady = true;
		if (state.repaired) {
			s_result.note = state.note;
			spdlog::warn("[Init] journal repaired: {}", state.note);
		}

		// Only receipts bound to THIS save may decide anything.  The journal
		// is per character, so the binding is what keeps an older save from
		// inheriting "already done" from a newer one.
		const auto view = Journal::DeriveInitStatesForSave(state.records, save);
		s_result.receiptsBound   = view.bound;
		s_result.receiptsForeign = view.foreign;
		s_result.receiptsUnbound = view.legacy;
		if (view.foreign > 0) {
			Record("收据日志里有 " + std::to_string(view.foreign) +
				" 条属于其他存档，已忽略（不参与本次判定）");
		}
		if (view.legacyIgnored > 0) {
			Record("旧版收据 " + std::to_string(view.legacyIgnored) +
				" 条无存档绑定，已改为由存档内收据裁决");
		}

		// "Generation" means: how many plugins this save can prove it has
		// already initialised.  Counted once, from the receipts, so a reload
		// that does no work still reports the true total.
		for (const auto& [plugin, st] : view.states) {
			if (st == Journal::InitState::kApplied) {
				++s_result.generation;
			}
		}

		std::vector<std::string> todo;
		for (const auto& plugin : diff.added) {
			++s_result.newPlugins;
			const auto it = view.states.find(plugin);
			const auto st = it == view.states.end() ? Journal::InitState::kNone : it->second;

			switch (st) {
			case Journal::InitState::kApplied:
				++s_result.alreadyDone;
				break;
			case Journal::InitState::kPending:
				++s_result.indeterminate;
				Record(plugin + " · 上次初始化中断，收据为 pending，已跳过以免重复发放");
				break;
			case Journal::InitState::kFailed:
				++s_result.previouslyFailed;
				Record(plugin + " · 上次初始化失败，等待手动处理");
				break;
			default:
				todo.push_back(plugin);
				break;
			}
		}

		if (todo.empty()) {
			s_result.finished = true;
			if (s_result.note.empty()) {
				s_result.note = "没有需要初始化的新增插件";
			}
			spdlog::info("[Init] nothing to do: new={} done={} pending={} failed={} (save={})",
				s_result.newPlugins, s_result.alreadyDone,
				s_result.indeterminate, s_result.previouslyFailed, save);
			return;
		}

		auto units = Discover(todo);
		s_result.units = static_cast<std::uint32_t>(units.size());
		for (const auto& unit : units) {
			s_result.questsFound += static_cast<std::uint32_t>(unit.quests.size());
			Record(unit.plugin + " · 发现 " + std::to_string(unit.quests.size()) + " 个启动任务");
		}

		// A plugin with no startup quest needs no init at all.  Record an
		// APPLIED receipt so we stop re-scanning it on every load.  If that
		// receipt cannot be written the plugin must stay in the todo set -
		// silently claiming success is how a broken plugin hides.
		for (const auto& plugin : todo) {
			const bool hasUnit = std::any_of(units.begin(), units.end(),
				[&](const Unit& u) { return u.plugin == plugin; });
			if (!hasUnit) {
				std::string note;
				const auto  payload = Journal::MakePayload({
					{ "plugin", plugin },
					{ "save",   save },
					{ "char",   character },
					{ "quests", "0" }
				});
				if (s_log->Append(Journal::Kind::kInitApplied, payload, note)) {
					++s_result.alreadyDone;
					s_sessionApplied.push_back(plugin);
				} else {
					++s_result.failures;
					Record(plugin + " · 无启动任务，但收据写入失败，下次仍会重试");
					spdlog::error("[Init] cannot record empty receipt for {}: {}",
						plugin, note);
				}
			}
		}

		if (units.empty()) {
			s_result.finished = true;
			s_result.note = "新增插件都没有 Start Game Enabled 任务，无需初始化";
			return;
		}

		// The load event is not a safe place to start quests: Papyrus and the
		// alias tables are still settling.  SKSE's task queue runs on the main
		// thread on a later frame, which is the same window console commands
		// land in.
		auto* tasks = SKSE::GetTaskInterface();
		if (tasks) {
			tasks->AddTask([units = std::move(units), log, save, character,
				               cb = std::move(a_onFinished)]() mutable {
				try {
					Execute(std::move(units), log, save, character, std::move(cb));
				} catch (const std::exception& e) {
					spdlog::error("[Init] deferred pass threw: {}", e.what());
					s_result.note = std::string{ "初始化任务异常: " } + e.what();
					s_result.finished = true;
				} catch (...) {
					spdlog::error("[Init] deferred pass threw (unknown)");
					s_result.finished = true;
				}
			});
			spdlog::info("[Init] scheduled {} plugin(s), {} startup quest(s) on the main-thread queue (save={})",
				s_result.units, s_result.questsFound, save);
		} else {
			// Old SKSE without a task interface: still do the work, still
			// behind the write-ahead receipt, just without the extra frame.
			spdlog::warn("[Init] no task interface; running initialisation inline");
			Execute(std::move(units), log, save, character, std::move(a_onFinished));
		}
	}

	void OnSave()
	{
		std::lock_guard lock(s_mutex);

		const auto save      = StateStore::CurrentSaveName();
		const auto character = StateStore::CurrentIdentity();
		const auto applied   = CollectApplied(s_log, save);

		// Carry the whole receipt set into this snapshot, not just what this
		// session happened to do.  A save that simply re-loads and re-saves
		// must not lose the receipts it inherited through its co-save - that
		// would make the next load re-run the initialisation.
		for (const auto& plugin : applied) {
			StateStore::SetString("init/applied/" + plugin, StateStore::kPluginVersion);
		}
		StateStore::SetInt("init/generation", static_cast<std::int64_t>(s_result.generation));

		if (s_log) {
			std::string note;
			std::string payload = "generation=" + std::to_string(s_result.generation) + "\n";
			payload += "save=" + save + "\n";
			payload += "char=" + character + "\n";
			for (const auto& plugin : applied) {
				payload += "plugin=" + plugin + "\n";
			}
			s_log->Append(Journal::Kind::kCommit, payload, note);

			// Bounded growth: every load appends two records per plugin.  Only
			// compact once the deferred pass has finished, so a queued
			// transaction can never be rewritten underneath itself.
			if (s_result.finished && s_log->Compact(800, note)) {
				if (!note.empty() && note.find("compacted") != std::string::npos) {
					spdlog::info("[Init] {}", note);
				}
			} else {
				spdlog::warn("[Init] journal compaction failed: {}", note);
			}
		}
		s_sessionApplied.clear();
	}

	void WriteReportSection(std::ostream& a_out)
	{
		Result r;
		{
			std::lock_guard lock(s_mutex);
			r = s_result;
		}

		a_out << "[载入期初始化 / Load-time init]\n";
		a_out << "  本次存档     : " << (r.saveName.empty() ? "未记录" : r.saveName) << "\n";
		a_out << "  新增插件     : " << r.newPlugins << "\n";
		a_out << "  本次排程     : " << r.units << " 个插件 / " << r.questsFound << " 个启动任务\n";
		a_out << "  已启动任务   : " << r.questsStarted << "   （校验通过 " << r.questsVerified << "）\n";
		a_out << "  已有收据     : " << r.alreadyDone << "   （本存档内已初始化过，跳过）\n";
		a_out << "  中断待定     : " << r.indeterminate << "   （pending，不自动重试）\n";
		a_out << "  历史失败     : " << r.previouslyFailed << "\n";
		a_out << "  失败         : " << r.failures << "\n";
		a_out << "  已回滚       : " << r.rolledBack << "   （Stop+Reset，下次可重试）\n";
		a_out << "  事务代号     : " << r.generation << "\n";
		a_out << "  收据归属     : 本存档 " << r.receiptsBound
			<< " 条 · 其他存档 " << r.receiptsForeign
			<< " 条（忽略）· 无绑定 " << r.receiptsUnbound << " 条\n";
		a_out << "  收据日志     : " << (r.journalReady ? "可用" : "不可用") << "\n";
		a_out << "  执行状态     : " << (r.finished ? "已完成" : "已排程，等待下一帧") << "\n";
		if (!r.note.empty()) {
			a_out << "  备注         : " << r.note << "\n";
		}

		if (!r.lines.empty()) {
			a_out << "\n";
			for (const auto& line : r.lines) {
				a_out << "    " << line << "\n";
			}
		}
		a_out << "\n";
		a_out << "  说明: 收据按存档绑定并在读档当场写盘，不依赖玩家保存；\n"
		         "        启动后会逐个复核任务是否真的在运行，未通过则回滚本次启动的任务，\n"
		         "        崩溃留下的 pending 不会自动重试，避免重复发放初始物品。\n\n";
	}
}
