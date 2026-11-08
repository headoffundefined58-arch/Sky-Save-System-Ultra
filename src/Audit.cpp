#include "PCH.h"

#include "Audit.h"
#include "Ini.h"
#include "SaveGuard.h"

namespace S3U
{
	namespace
	{
		Report s_report;

		struct TypeLabel
		{
			RE::FormType type;
			const char*  name;
		};

		// Only the record types that actually dominate a save's footprint, or
		// that hint at mod-uninstall residue, are listed.  Everything else is
		// folded into "合计".
		constexpr TypeLabel kInterestingTypes[] = {
			{ RE::FormType::Cell, "Cell 单元" },
			{ RE::FormType::WorldSpace, "WorldSpace 世界" },
			{ RE::FormType::Quest, "Quest 任务" },
			{ RE::FormType::Dialogue, "Dialogue 对话" },
			{ RE::FormType::Info, "Info 对话条目" },
			{ RE::FormType::Package, "Package AI 包" },
			{ RE::FormType::Scene, "Scene 场景" },
			{ RE::FormType::NPC, "NPC" },
			{ RE::FormType::LeveledNPC, "LeveledNPC" },
			{ RE::FormType::Container, "Container 容器" },
			{ RE::FormType::Weapon, "Weapon 武器" },
			{ RE::FormType::Armor, "Armor 护甲" },
			{ RE::FormType::Activator, "Activator 激活器" },
			{ RE::FormType::Static, "Static 静态物" },
			{ RE::FormType::Flora, "Flora 植物" },
			{ RE::FormType::Spell, "Spell 法术" },
			{ RE::FormType::MagicEffect, "MagicEffect 魔法效果" },
			{ RE::FormType::Keyword, "Keyword 关键词" },
			{ RE::FormType::FormList, "FormList 表单" },
			{ RE::FormType::Global, "Global 全局变量" },
		};

		std::uint64_t NowMs()
		{
			using namespace std::chrono;
			return static_cast<std::uint64_t>(
				duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		}

		void CollectPlugins(Report& a_report)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}

			for (auto* file : dh->files) {
				if (!file) {
					continue;
				}
				++a_report.pluginCount;
				if (file->IsLight()) {
					++a_report.lightPluginCount;
				} else {
					const auto name = file->GetFilename();
					if (name.size() >= 4 &&
						(name.substr(name.size() - 4) == ".esm" || name.substr(name.size() - 4) == ".ESM")) {
						++a_report.esmCount;
					}
				}
			}
		}

		void CollectFormCounts(Report& a_report)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}

			constexpr auto kMax = std::to_underlying(RE::FormType::Max);
			for (std::size_t i = 0; i < kMax; ++i) {
				a_report.totalForms += dh->formArrays[i].size();
			}

			for (const auto& entry : kInterestingTypes) {
				const auto idx = std::to_underlying(entry.type);
				if (idx >= kMax) {
					continue;
				}
				const auto count = static_cast<std::uint32_t>(dh->formArrays[idx].size());
				if (count != 0) {
					a_report.typeCounts.emplace_back(entry.name, count);
				}
			}

			const auto questIdx = std::to_underlying(RE::FormType::Quest);
			const auto npcIdx   = std::to_underlying(RE::FormType::NPC);
			if (questIdx < kMax) {
				a_report.questForms = dh->formArrays[questIdx].size();
			}
			if (npcIdx < kMax) {
				a_report.npcForms = dh->formArrays[npcIdx].size();
			}
		}

		void CollectScripts(Report& a_report)
		{
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) {
				return;
			}

			// NOTE: deliberately not taking attachedScriptsLock.  At this point
			// (right after a load completes, on the main thread) the VM is
			// quiescent; taking the engine's own spin lock from a callback that
			// may itself already run inside a VM operation risks a deadlock,
			// which is unrecoverable.  A torn read of a histogram is survivable.
			std::unordered_map<std::string, std::uint32_t> histogram;

			for (const auto& [handle, scripts] : vm->attachedScripts) {
				(void)handle;
				++a_report.scriptedObjects;
				for (const auto& attached : scripts) {
					auto* object = attached.get();
					++a_report.scriptInstances;
					if (!object) {
						continue;
					}
					if (!object->IsInitialized()) {
						++a_report.uninitializedScripts;
					}
					const auto* typeInfo = object->GetTypeInfo();
					if (!typeInfo) {
						continue;
					}
					const char* name = typeInfo->GetName();
					if (name && *name) {
						++histogram[name];
					}
				}
			}

			a_report.topScripts.reserve(histogram.size());
			for (auto& [name, count] : histogram) {
				a_report.topScripts.emplace_back(name, count);
			}
			std::sort(a_report.topScripts.begin(), a_report.topScripts.end(),
				[](const auto& a_lhs, const auto& a_rhs) {
					if (a_lhs.second != a_rhs.second) {
						return a_lhs.second > a_rhs.second;
					}
					return a_lhs.first < a_rhs.first;
				});
			if (a_report.topScripts.size() > 20) {
				a_report.topScripts.resize(20);
			}
		}

		void CollectReferences(Report& a_report)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}

			const auto maxCells = Ini::Get().maxCells;

			const auto walk = [&a_report](RE::TESObjectCELL* a_cell) {
				if (!a_cell || !a_cell->IsAttached()) {
					return;
				}
				++a_report.cellsAttached;
				a_cell->ForEachReference([&a_report](RE::TESObjectREFR* a_ref) {
					if (!a_ref) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					++a_report.refs;

					// A reference whose base object no longer resolves is the
					// fingerprint of a removed plugin: the changeform survived,
					// the thing it was an instance of did not.  Nothing in the
					// engine is allowed to dereference this; counting it is what
					// turns "random crash in a cell" into a number.
					if (a_ref->GetBaseObject() == nullptr) {
						++a_report.nullBaseRefs;
					}

					const bool deleted = a_ref->IsDeleted() || a_ref->IsMarkedForDeletion();
					if (deleted) {
						++a_report.deletedRefs;
					} else if (a_ref->IsDisabled()) {
						// Disabled but never deleted: the ChangeForm stays in the
						// save forever.  This is the single largest source of
						// bloat in a long playthrough.
						++a_report.disabledRefs;
					}

					if (a_ref->IsDynamicForm()) {
						++a_report.dynamicRefs;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			};

			// IMPORTANT: on Skyrim, cells are NOT in
			// TESDataHandler::formArrays[FormType::Cell] - that array stays
			// empty.  v0.1 walked exactly that array and therefore reported
			// "0 cells / 0 refs" on a save with 65k script instances.
			// Interior cells live in the data handler's own array; exterior
			// ones only exist in the loaded grid around the player.
			for (std::uint32_t i = 0; i < dh->interiorCells.size(); ++i) {
				if (maxCells > 0 && a_report.cellsScanned >= static_cast<std::uint32_t>(maxCells)) {
					spdlog::warn("[3SU] cell cap ({}) reached - reference numbers are a sample", maxCells);
					return;
				}
				++a_report.cellsScanned;
				walk(dh->interiorCells[i]);
			}

			if (auto* tes = RE::TES::GetSingleton(); tes && tes->gridCells) {
				auto*      grid = tes->gridCells;
				const auto len  = grid->length;
				for (std::uint32_t x = 0; x < len; ++x) {
					for (std::uint32_t y = 0; y < len; ++y) {
						++a_report.cellsScanned;
						walk(grid->GetCell(x, y));
					}
				}
			}
		}

		std::string Timestamp()
		{
			std::time_t now = std::time(nullptr);
			std::tm     tm{};
			localtime_s(&tm, &now);
			char buffer[32]{};
			std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
			return buffer;
		}

		void Dump(std::ostream& a_out, const Report& a_report)
		{
			a_out << "[存档体检 / 只读数据]\n";
			a_out << "扫描耗时: " << a_report.elapsedMs << " ms\n";

			a_out << "[插件]\n";
			a_out << "  总数        : " << a_report.pluginCount << "\n";
			a_out << "  轻量(ESL)   : " << a_report.lightPluginCount << "\n";
			a_out << "  完整(ESM)   : " << a_report.esmCount << "\n\n";

			a_out << "[记录]\n";
			a_out << "  form 总数   : " << a_report.totalForms << "\n";
			for (const auto& [name, count] : a_report.typeCounts) {
				a_out << "    " << name << " : " << count << "\n";
			}
			a_out << "\n";

			a_out << "[脚本]\n";
			a_out << "  带脚本的引用: " << a_report.scriptedObjects << "\n";
			a_out << "  脚本实例数  : " << a_report.scriptInstances << "\n";
			a_out << "  未初始化    : " << a_report.uninitializedScripts << "\n";
			if (Ini::Get().verbose && !a_report.topScripts.empty()) {
				a_out << "  实例最多的脚本:\n";
				for (const auto& [name, count] : a_report.topScripts) {
					a_out << "    " << count << "\t" << name << "\n";
				}
			}
			a_out << "\n";

			if (Ini::Get().scanReferences) {
				a_out << "[引用] (仅统计当前已加载的单元)\n";
				a_out << "  扫描单元    : " << a_report.cellsScanned
					  << " (已加载 " << a_report.cellsAttached << ")\n";
				a_out << "  引用总数    : " << a_report.refs << "\n";
				a_out << "  已删除壳    : " << a_report.deletedRefs << "\n";
				a_out << "  禁用未删除  : " << a_report.disabledRefs << "   <-- 永久残留的主要来源\n";
				a_out << "  基础对象缺失: " << a_report.nullBaseRefs << "   <-- 卸载 mod 后的悬空引用，最易闪退\n";
				a_out << "  运行时对象  : " << a_report.dynamicRefs << " (0xFF 前缀)\n";
				if (a_report.refs > 0) {
					const auto dead = a_report.deletedRefs + a_report.disabledRefs;
					a_out << "  残留占比    : "
						  << (100.0 * static_cast<double>(dead) / static_cast<double>(a_report.refs))
						  << " %\n";
				}
				a_out << "\n";
			} else {
				a_out << "[引用] 已按配置跳过 (bScanReferences=0)\n\n";
			}

			a_out << "[存档容器校验 / 只读]\n";
			const auto& guard = Guard::Last();
			if (!guard.dirFound) {
				a_out << "  未启用：找不到存档目录（可在 ini 用 sSaveDir 指定）\n";
			} else if (guard.verified && guard.located) {
				a_out << "  本次读入校验: " << (guard.ok ? "容器完整" : "容器异常（引擎可能拒绝载入）")
					  << " — " << guard.note << "\n";
				if (!guard.lastFile.empty()) {
					a_out << "  校验文件    : " << guard.lastFile.filename().string() << "\n";
				}
			} else {
				a_out << "  本次读入校验: 未定位（不是损坏）— 文件仍会正常载入\n";
			}
			a_out << "  备份功能    : 不提供（本版本不生成 .ess.bak，也不保留历史日志）\n\n";
			a_out << "重要: 容器完整 ≠ 存档可载入。引擎会按存档插件表逐名解析当前加载顺序；\n";
			a_out << "      插件被删/禁用导致名字缺失或索引漂移时，引擎在读 .ess 阶段就会\n";
			a_out << "      弹出「此存档已损坏，无法加载」，本插件的任何载入后逻辑都跑不到。\n";
			a_out << "      这类风险必须在启动游戏前用 3SU Save Doctor 修，不能靠载入期自愈。\n\n";

			a_out << "说明: 体检数字只读；载入期自愈只处理「已成功进入世界」后的残留状态。\n";
			a_out << "      引用数只覆盖当前已加载的单元（内景 + 玩家周围网格）；\n";
			a_out << "      未加载单元的持久引用不计入，因此这个数是下限而不是全量。\n";
			a_out << "      基础对象缺失的引用会被标记为删除，但任务别名、带脚本实例\n";
			a_out << "      和运行时对象会被跳过——删除它们会让仍指向它们的句柄悬空。\n";
		}
	}

	namespace Audit
	{
		bool Run()
		{
			const auto start = NowMs();

			Report report;
			try {
				CollectPlugins(report);
				CollectFormCounts(report);
				CollectScripts(report);
				if (Ini::Get().scanReferences) {
					CollectReferences(report);
				}
				report.valid = true;
			} catch (const std::exception& e) {
				spdlog::error("[3SU] audit aborted: {}", e.what());
				report.valid = false;
			} catch (...) {
				spdlog::error("[3SU] audit aborted: unknown exception");
				report.valid = false;
			}

			report.elapsedMs = NowMs() - start;
			s_report        = std::move(report);

			if (!s_report.valid) {
				return false;
			}

			spdlog::info("[3SU] plugins={} forms={} scriptedObjects={} instances={} refs={} deleted={} disabled={} in {} ms",
				s_report.pluginCount, s_report.totalForms, s_report.scriptedObjects,
				s_report.scriptInstances, s_report.refs, s_report.deletedRefs,
				s_report.disabledRefs, s_report.elapsedMs);

			return true;
		}

		const Report& Last()
		{
			return s_report;
		}

		bool HasResult()
		{
			return s_report.valid;
		}

		void WriteSection(std::ostream& a_out, const Report& a_report)
		{
			if (!a_report.valid) {
				a_out << "[存档体检 / 只读数据]\n  本次扫描未成功，略过。\n\n";
				return;
			}
			// v2.0.0: the caller owns the single report-latest.txt.  Writing
			// a timestamped audit-YYYYMMDD-HHMMSS.log here was the reason the
			// data directory filled up with history nobody could trust, so
			// this function now only contributes its section to that one file.
			Dump(a_out, a_report);
		}
	}
}
