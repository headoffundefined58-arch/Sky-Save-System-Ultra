#include "PCH.h"

#include "Audit.h"
#include "Ini.h"
#include "InitEngine.h"
#include "LoadOrder.h"
#include "Migrate.h"
#include "Notify.h"
#include "Records.h"
#include "Repair.h"
#include "RepairPolicy.h"
#include "SaveGuard.h"
#include "SaveDoctor.h"
#include "Trend.h"
#include "Vault.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <cstdio>
#include <fstream>

namespace
{
	bool s_auditedThisLoad = false;

	void SetupLog()
	{
		// CommonLibSSE-NG only installs a console sink, and a game process has
		// no console: without a file sink the plugin would be completely mute.
		auto directory = SKSE::log::log_directory();
		if (!directory) {
			return;
		}

		*directory /= "SkySaveSystemUltra.log";

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(directory->string(), true);
		sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

		auto logger = std::make_shared<spdlog::logger>("3su", std::move(sink));
		logger->set_level(spdlog::level::debug);
		logger->flush_on(spdlog::level::debug);

		spdlog::set_default_logger(std::move(logger));
		spdlog::set_level(spdlog::level::debug);
	}

	std::string FormatCount(std::uint64_t a_value)
	{
		char buffer[64]{};
		if (a_value >= 10000) {
			std::snprintf(buffer, sizeof(buffer), "%.1f万", static_cast<double>(a_value) / 10000.0);
		} else {
			std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(a_value));
		}
		return buffer;
	}

	std::string Stamp()
	{
		std::time_t now = std::time(nullptr);
		std::tm     tm{};
		localtime_s(&tm, &now);
		char buffer[32]{};
		std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
		return buffer;
	}

	const char* SourceName(S3U::Vault::Status::Source a_source)
	{
		using S = S3U::Vault::Status::Source;
		switch (a_source) {
		case S::kCosave: return "co-save";
		case S::kDisk:   return "disk";
		default:         return "none";
		}
	}

	// -------------------------------------------------------------------------
	// The ONLY place where world observations enter the state system.
	//
	// In the old design this function persisted eleven unrelated values with
	// no record of why any of them mattered.  Now every write is arbitrated by
	// StateSchema: kPersist values are collected, kDerive values are allowed
	// into the in-memory view for this session's report but cannot reach a
	// snapshot, and kEphemeral values are never touched here at all.
	// -------------------------------------------------------------------------
	void CollectState()
	{
		using namespace S3U;

		// Historical fact: the sequence of past observations cannot be
		// recomputed once the worlds it describes are gone.
		StateStore::SetString("trend/history", Trend::EncodeHistoryBlob());

		// The load order AS IT WAS at save time.  The current order is
		// derivable; this baseline is not.
		LoadOrder::StoreToVault();

		// Deliberately still written, and deliberately still discarded on the
		// next load: these are kDerive.  Keeping the calls makes the intent
		// visible at the point of use rather than only in a table.
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player) {
			Vault::SetForm("player/ref", player);
			if (auto* cell = player->GetParentCell()) {
				Vault::SetForm("player/cell", cell);
				StateStore::SetString("player/lastCell", std::to_string(cell->GetFormID()));
			}
			if (auto* world = player->GetWorldspace()) {
				Vault::SetForm("player/world", world);
			}
		}
		if (auto* calendar = RE::Calendar::GetSingleton()) {
			Vault::SetFloat("meta/gameDays", static_cast<double>(calendar->GetDaysPassed()));
		}

		std::int64_t saves = 0;
		StateStore::GetInt("meta/saveCount", saves);
		StateStore::SetInt("meta/saveCount", saves + 1);
		StateStore::SetString("meta/version", StateStore::kPluginVersion);
	}

	// v2.0.0: ONE report file, ONE action log.  The timestamped audit logs,
	// the separate modguard-latest.txt, the schema txt and backup indices are
	// all gone.  Everything the player needs to judge this save is in here.
	void WriteReport()
	{
		using namespace S3U;

		const auto dir = Paths::EnsureDataDir();
		if (dir.empty()) {
			return;
		}

		std::ofstream out(dir / RepairPolicy::kReportTxt, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) {
			return;
		}

		const auto& vault = Vault::Last();
		const auto& diff  = LoadOrder::Last();

		out << "== 存档系统报告 / SkySaveSystemUltra v2.0.0 ==\n";
		out << "生成时间: " << Stamp() << "\n";
		out << "本次存档: " << (StateStore::CurrentSaveName().empty() ?
			"未记录" : StateStore::CurrentSaveName()) << "\n";
		out << "记录策略: 仅此一份 txt + 一份 log（覆盖式，不保留历史）；不提供备份。\n\n";

		// Repair first: it is the section that decides what the player must do.
		Repair::WriteReportSection(out);

		// Audit::WriteSection emits its own section header.
		Audit::WriteSection(out, Audit::Last());
		out << "\n";

		out << "[保险库 / Vault]\n";
		out << "  角色       : " << vault.identity << "\n";
		out << "  提交序号   : " << vault.tx << "\n";
		out << "  条目数     : " << vault.entries << "\n";
		out << "  状态来源   : " << SourceName(vault.source) << "\n";
		out << "  引用解析   : " << (vault.formRefs - vault.tombstoned) << "/" << vault.formRefs << "\n";
		if (vault.tombstoned > 0) {
			out << "  墓碑引用   : " << vault.tombstoned << "   <-- 指向已不存在的对象\n";
		}
		if (!vault.note.empty()) {
			out << "  备注       : " << vault.note << "\n";
		}
		out << "\n";

		// The classification result is part of the report, not just a code
		// comment: the player can see exactly how many fields were kept,
		// recomputed, reset, or discarded by the new rules on this very load.
		const auto& state = StateStore::Last();
		out << "[状态分类 / StateSchema]\n";
		out << "  规则       : 仅「必须持久化」字段随存档走；可推导字段每次重算，临时字段每次重置。\n";
		out << "  分类总数   : 持久化 " << State::CountBy(State::Policy::kPersist)
			<< " · 可推导 " << State::CountBy(State::Policy::kDerive)
			<< " · 临时 " << State::CountBy(State::Policy::kEphemeral) << "\n";
		out << "  本次重算   : " << state.derivedFields << " 项\n";
		out << "  本次重置   : " << state.resetFields << " 项\n";
		out << "  按规则舍弃 : " << state.discardedFields
			<< " 项（旧存档里的可推导/临时字段）\n";
		out << "  回读一致性 : " << (state.roundTripOk ? "通过" : "未验证/失败") << "\n";
		out << "  完整清单   : 见本报告末尾「状态分类表」段\n";
		out << "\n";

		// Scope is the second axis, and the one that decides whether a value
		// may be inherited at all.  Without it a save written before the mod
		// existed inherited a newer save's baseline and concluded that nothing
		// had been added - which is precisely how "still needs a new game"
		// survived an initialiser that works.
		out << "[存档作用域 / Scope]\n";
		out << "  规则       : 存档级字段只属于写它的那一份 .ess；角色级跨该角色的所有存档；全局级人人相同。\n";
		out << "  字段归属   : 存档级 " << State::CountBy(State::Scope::kSave)
			<< " · 角色级 " << State::CountBy(State::Scope::kCharacter)
			<< " · 全局 " << State::CountBy(State::Scope::kGlobal) << "\n";
		out << "  本次存档   : " << (state.saveName.empty() ? "未记录" : state.saveName) << "\n";
		out << "  快照归属   : " << (state.sameSave ? "一致" : "不一致（来自另一份存档）") << "\n";
		out << "  作用域舍弃 : " << state.scopeDropped << " 项\n";
		out << "  归属拒绝   : "
			<< (state.foreignOwner ? "是（状态属于另一角色，已整份拒绝）" : "否") << "\n";
		out << "\n";

		LoadOrder::WriteReportSection(out);
		InitEngine::WriteReportSection(out);
		Migrate::WriteReportSection(out);

		const auto& repair = Repair::Last();
		out << "[结论]\n";
		if (repair.success) {
			out << "  存档已修复并通过验证。\n";
			out << "  下一步（必要）: 保存当前存档 → 重新载入该存档。\n";
			out << "  保存会把清理结果写进 .ess，重载后缺失 MOD 残留不再被实例化。\n";
		} else if (repair.ran && !repair.note.empty()) {
			out << "  " << repair.note << "\n";
		} else if (!diff.haveBaseline) {
			out << "  本次读入的存档没有加载顺序基线；下一次保存后即可完整比对。\n";
		} else if (diff.Risky()) {
			out << "  检测到缺失插件，载入期自愈已介入，详见「载入期自愈」段。\n";
		} else {
			out << "  安全：加载顺序与存档一致，没有需要修复的残留。\n";
		}
		out << "  说明: 本版本不提供备份，不保留历史日志；每次载入只覆盖写这两份文件。\n"
		       "        初始化走引擎自身的任务启动路径补状态，不改写 .ess 字节；\n"
		       "        收据按存档绑定写盘，不依赖玩家保存；未通过校验的启动会被回滚。\n\n";

		// The schema table used to be a third txt.  It is part of this one now.
		StateStore::Describe(out);

		// The single action log is overwritten on every write, never appended.
		Repair::WriteActionLog(dir);
	}

	void NotifyStatus()
	{
		using namespace S3U;

		// v2.0.0: risky saves are no longer announced as "danger".  They are
		// loaded and repaired; the only blocking message is the explicit
		// save-then-reload prompt emitted by Repair on verified success.
		if (!Ini::Get().notify) {
			return;
		}

		const auto& guard = Guard::Last();
		if (guard.verified && !guard.ok && guard.located) {
			spdlog::warn("[3SU] structural check flagged {}, continuing into repair",
				guard.lastFile.filename().string());
		}

		const auto& repair = Repair::Last();
		if (repair.finished && !repair.success && repair.missingBefore > 0) {
			Notify::Push("存档残留未完全清理 · 详见 report-latest.txt");
		}
	}

	// -------------------------------------------------------------------------
	// Automatic file-level repair polling.  When SaveDoctor dispatched the exe
	// at kPreLoadGame, this re-schedules itself once per frame until the exe
	// reports "OK", then triggers an in-engine reload of the repaired save.
	// Non-blocking: it never sleeps, so the game's message pump keeps running
	// while the exe works in the background.
	// -------------------------------------------------------------------------
	void PollSaveDoctor()
	{
		using namespace S3U;

		const auto state = SaveDoctor::PollReload();
		if (state == SaveDoctor::PollState::kRunning) {
			// Re-arm for the next frame.
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([] { PollSaveDoctor(); });
			}
			return;
		}

		if (state == SaveDoctor::PollState::kReady) {
			SaveDoctor::ReloadNow();
			// ReloadNow may defer (engine still loading); re-arm regardless so
			// a deferred reload happens as soon as the load settles.
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([] { PollSaveDoctor(); });
			}
			return;
		}

		// kDone (nothing to repair) / kFailed: leave the engine to its own
		// behaviour.  A kFailed is already logged by PollReload.
	}

	// -------------------------------------------------------------------------
	// The whole read-side pass, once per load.
	// -------------------------------------------------------------------------
	void RunLoadPass()
	{
		using namespace S3U;

		if (s_auditedThisLoad) {
			return;
		}
		s_auditedThisLoad = true;

		if (Ini::Get().vaultReconcile) {
			Vault::Reconcile();
		}

		// A save written before this plugin existed carries no co-save block,
		// so nothing has compared the baseline yet.  Without this fallback the
		// baseline loaded from disk would sit unused and the report would say
		// "no baseline" while one was in fact present.
		if (!LoadOrder::Last().haveBaseline) {
			LoadOrder::CompareWithVault();
		}

		// Both passes queue world mutations on the main-thread task queue and
		// rewrite the single report when done.  Initialisation captured its
		// "added" list synchronously above, so the repair's baseline rewrite
		// cannot swallow a newly installed mod.
		InitEngine::PrepareOnLoad([] { WriteReport(); });
		Repair::PrepareOnLoad([] { WriteReport(); });

		// Migration runs before the audit so the report can state what changed
		// rather than what was about to change.  Its manifest quests execute
		// inline, before the deferred init, which then sees them already
		// running and skips them.
		Migrate::RunOnLoad();

		if (Ini::Get().auditOnLoad && Audit::Run()) {
			const auto& report = Audit::Last();
			Trend::CompareAndReport();

			if (Ini::Get().notify) {
				const auto dead    = report.deletedRefs + report.disabledRefs;
				const double ratio = report.refs == 0 ? 0.0 :
				                     100.0 * static_cast<double>(dead) / static_cast<double>(report.refs);

				char buffer[256]{};
				std::snprintf(buffer, sizeof(buffer),
					"存档体检 · form %s · 脚本 %s",
					FormatCount(report.totalForms).c_str(),
					FormatCount(report.scriptInstances).c_str());
				Notify::Push(buffer);

				std::snprintf(buffer, sizeof(buffer),
					"只读数据 · 残留 %.1f%% · 悬空 %llu",
					ratio,
					static_cast<unsigned long long>(report.nullBaseRefs),
					static_cast<unsigned long long>(report.deletedRefs + report.disabledRefs));
				Notify::Push(buffer);
			}
		}

		WriteReport();
	}

	// -------------------------------------------------------------------------
	// TESLoadGameEvent fires once the save has been fully deserialised and the
	// world is live: the only moment where starting a quest, walking cell
	// graphs and reading the Papyrus instance table are all both meaningful and
	// safe.  It is also exactly the window a console `startquest` would land in.
	// -------------------------------------------------------------------------
	class LoadGameSink final : public RE::BSTEventSink<RE::TESLoadGameEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESLoadGameEvent*,
			RE::BSTEventSource<RE::TESLoadGameEvent>*) override
		{
			// A C++ exception escaping an SKSE/engine callback terminates the
			// process.  Everything is swallowed here.
			try {
				RunLoadPass();
				NotifyStatus();
			} catch (const std::exception& e) {
				spdlog::error("[3SU] load handler threw: {}", e.what());
			} catch (...) {
				spdlog::error("[3SU] load handler threw (unknown)");
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	LoadGameSink s_loadSink;

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink(&s_loadSink);
				spdlog::info("[3SU] load-game event sink registered");
			} else {
				spdlog::error("[3SU] ScriptEventSourceHolder missing - load hooks disabled");
			}

			// Pre-flight repair: scan save directories and fix every save that
			// references removed plugins, BEFORE the player opens the load
			// menu.  This makes the engine's "missing content" confirmation
			// never appear for a save the repair can already fix.
			if (S3U::Ini::Get().repairEnabled) {
				try {
					S3U::Guard::Initialize();
					S3U::SaveDoctor::Preflight();
				} catch (const std::exception& e) {
					spdlog::error("[3SU] SaveDoctor preflight failed: {}", e.what());
				} catch (...) {
					spdlog::error("[3SU] SaveDoctor preflight failed (unknown)");
				}
			}
			break;

		case SKSE::MessagingInterface::kPreLoadGame:
			{
				s_auditedThisLoad = false;
				const char* name = a_msg->dataLen > 0 && a_msg->data ?
				                       static_cast<const char*>(a_msg->data) :
				                       nullptr;

			S3U::Vault::ObserveSaveName(name);

			if (S3U::Ini::Get().guardEnabled) {
				try {
					S3U::Guard::OnPreLoadGame(name);
				} catch (...) {
					spdlog::error("[3SU] read-only save check failed");
				}
			}
			try {
				S3U::Repair::SweepLegacyBackups();
			} catch (...) {
				spdlog::error("[3SU] legacy backup sweep failed");
			}

			// Automatic file-level repair.  Runs AFTER the read-only check so
			// it can reuse the located save path, and only dispatches the exe
			// when a save genuinely references removed plugins.
			if (S3U::Ini::Get().repairEnabled) {
				try {
					const auto& guard = S3U::Guard::Last();
					if (guard.located && !guard.lastFile.empty()) {
						if (S3U::SaveDoctor::OnPreLoadGame(
								guard.lastFile, guard.lastFile.parent_path())) {
							// Dispatched: begin polling for completion.
							if (auto* tasks = SKSE::GetTaskInterface()) {
								tasks->AddTask([] { PollSaveDoctor(); });
							}
						}
					}
				} catch (const std::exception& e) {
					spdlog::error("[3SU] SaveDoctor dispatch failed: {}", e.what());
				} catch (...) {
					spdlog::error("[3SU] SaveDoctor dispatch failed (unknown)");
				}
			}
		}
		break;

		case SKSE::MessagingInterface::kSaveGame:
			{
				const char* name = a_msg->dataLen > 0 && a_msg->data ?
				                       static_cast<const char*>(a_msg->data) :
				                       nullptr;

				S3U::Vault::ObserveSaveName(name);

				try {
					S3U::Trend::CaptureOnMainThread();
					S3U::InitEngine::OnSave();  // freeze init receipts into this snapshot
					CollectState();
					S3U::Vault::Commit();
				} catch (const std::exception& e) {
					spdlog::error("[3SU] vault commit threw: {}", e.what());
				} catch (...) {
					spdlog::error("[3SU] vault commit threw (unknown)");
				}

			}
			break;

		case SKSE::MessagingInterface::kNewGame:
			s_auditedThisLoad = false;
			S3U::Vault::Reset();
			break;

		default:
			break;
		}
	}

	void OnCoSave(SKSE::SerializationInterface* a_intfc)
	{
		// One state block per save.  The trend history and everything else the
		// schema classifies kPersist are already fields of StateStore; there is
		// no second, competing record to fall out of sync with it.
		try {
			S3U::StateStore::OnSave(a_intfc);
		} catch (const std::exception& e) {
			spdlog::error("[3SU] state write threw: {}", e.what());
		} catch (...) {
			spdlog::error("[3SU] state write failed");
		}
	}

	void OnCoLoad(SKSE::SerializationInterface* a_intfc)
	{
		// SKSE hands the load callback one stateful record iterator.  Reading
		// it from two places does not work: whichever module walks it first
		// consumes the whole stream, and every later reader sees nothing -
		// which then looks exactly like a damaged co-save.  So the stream is
		// walked once, here, and each block is routed to its owner as bytes.
		std::vector<std::uint8_t> vaultBlock;
		std::vector<std::uint8_t> trendBlock;
		bool                      vaultSeen = false;
		bool                      trendSeen = false;
		std::size_t               records = 0;
		std::size_t               foreign = 0;

		try {
			if (a_intfc) {
				std::uint32_t type = 0;
				std::uint32_t version = 0;
				std::uint32_t length = 0;

				while (a_intfc->GetNextRecordInfo(type, version, length)) {
					++records;

					// Refuse to allocate on a corrupt length.  The record has
					// to be read even when it is not ours, otherwise the
					// iterator cannot advance to the next one.
					if (length > 32u * 1024u * 1024u) {
						spdlog::warn("[3SU] co-save record 0x{:08X} claims {} bytes - stopping",
							type, length);
						break;
					}

					std::vector<std::uint8_t> block(length ? length : 1, 0);
					const auto got = a_intfc->ReadRecordData(block.data(), length);
					if (got != length) {
						spdlog::warn("[3SU] co-save record 0x{:08X} truncated ({} of {} bytes)",
							type, got, length);
						break;
					}

					if (type == S3U::Vault::RecordType()) {
						block.resize(length);
						vaultBlock = std::move(block);
						vaultSeen  = true;
					} else if (type == S3U::Trend::RecordType()) {
						block.resize(length);
						trendBlock = std::move(block);
						trendSeen  = true;
					} else {
						++foreign;
					}
				}
			}
		} catch (const std::exception& e) {
			spdlog::error("[3SU] co-save scan threw: {}", e.what());
		} catch (...) {
			spdlog::error("[3SU] co-save scan failed");
		}

		spdlog::info("[3SU] co-save: {} records ({} foreign), vault={} trend={}",
			records, foreign, vaultSeen ? vaultBlock.size() : 0, trendSeen ? trendBlock.size() : 0);

		try {
			S3U::Trend::OnLoadRecord(trendSeen ? trendBlock.data() : nullptr, trendBlock.size());
		} catch (...) {
			spdlog::error("[3SU] trend read failed");
		}

		try {
			S3U::Vault::OnLoadRecord(vaultSeen ? vaultBlock.data() : nullptr, vaultBlock.size());
			// The store now holds this save's baseline, so the comparison has
			// to happen here and not later: later, the baseline would already
			// have been overwritten by the next commit.
			S3U::LoadOrder::CompareWithVault();

			// trend/history is a classified kPersist field of the store.  It
			// takes precedence over the legacy TRND block read above, so a
			// save always has exactly one authoritative trend history.
			std::string trendBlob;
			if (S3U::StateStore::GetString("trend/history", trendBlob) &&
				!trendBlob.empty()) {
				S3U::Trend::RestoreHistoryBlob(trendBlob.data(), trendBlob.size());
			}
		} catch (const std::exception& e) {
			spdlog::error("[3SU] state read threw: {}", e.what());
		} catch (...) {
			spdlog::error("[3SU] state read failed");
		}
	}

	void OnCoRevert(SKSE::SerializationInterface* a_intfc)
	{
		try {
			S3U::Trend::OnRevert(a_intfc);
		} catch (...) {}
		try {
			S3U::Vault::OnRevert(a_intfc);
		} catch (...) {}
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 2, 0, 0, 0 },
	.Name = "SkySaveSystemUltra",
	.Author = "Icarus",
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary);

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	SetupLog();
	S3U::Ini::Load();
	S3U::Repair::PruneRecordsOnStartup();
	S3U::StateStore::InitializeEngine();

	if (auto* serialization = SKSE::GetSerializationInterface()) {
		// Co-save signature.  It changed with the rename, which means saves
		// written by the old 存档体检 build no longer hand us a block.  That is
		// harmless: the block is a cache of the disk vault, and the vault
		// still holds every committed generation.
		serialization->SetUniqueID('S3SU');
		serialization->SetSaveCallback(OnCoSave);
		serialization->SetLoadCallback(OnCoLoad);
		serialization->SetRevertCallback(OnCoRevert);
	} else {
		spdlog::error("[3SU] serialisation interface unavailable - no vault, no trend");
	}

	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnSKSEMessage);
	}

	spdlog::info("[3SU] SkySaveSystemUltra v2.0.0 loaded (load-time self-healing, no backups, one txt + one log)");

	return true;
}
