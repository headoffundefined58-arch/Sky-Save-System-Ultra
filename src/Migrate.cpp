#include "PCH.h"

#include "Migrate.h"
#include "FormRef.h"
#include "Ini.h"
#include "LoadOrder.h"
#include "Vault.h"

#include <cstdlib>
#include <fstream>
#include <ostream>
#include <unordered_set>

namespace S3U::Migrate
{
	namespace
	{
		Result s_result;

		std::string Trim(std::string_view a_view)
		{
			std::size_t first = 0;
			while (first < a_view.size() && std::isspace(static_cast<unsigned char>(a_view[first]))) {
				++first;
			}
			std::size_t last = a_view.size();
			while (last > first && std::isspace(static_cast<unsigned char>(a_view[last - 1]))) {
				--last;
			}
			return std::string{ a_view.substr(first, last - first) };
		}

		std::string ToLower(std::string_view a_view)
		{
			std::string out{ a_view };
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		struct Command
		{
			std::string verb;
			std::string arg;
		};

		struct Manifest
		{
			std::string          name;
			std::int64_t         version{ 1 };
			std::vector<Command> commands;
		};

		bool ParseManifest(const std::filesystem::path& a_path, Manifest& a_out)
		{
			std::ifstream in(a_path);
			if (!in.is_open()) {
				return false;
			}

			a_out.name = a_path.stem().string();

			std::string line;
			while (std::getline(in, line)) {
				std::string_view view{ line };
				if (const auto hash = view.find_first_of(";#"); hash != std::string_view::npos) {
					view = view.substr(0, hash);
				}
				const auto text = Trim(view);
				if (text.empty() || text.front() == '[') {
					continue;
				}
				const auto eq = text.find('=');
				if (eq == std::string::npos) {
					continue;
				}

				const auto key   = ToLower(Trim(std::string_view{ text }.substr(0, eq)));
				const auto value = Trim(std::string_view{ text }.substr(eq + 1));
				if (value.empty()) {
					continue;
				}

				if (key == "name") {
					a_out.name = value;
				} else if (key == "version") {
					a_out.version = std::atoll(value.c_str());
					if (a_out.version < 1) {
						a_out.version = 1;
					}
				} else {
					a_out.commands.push_back(Command{ key, value });
				}
			}

			return !a_out.commands.empty();
		}

		// "skyrim.esm:0x0001A2B3" -> live form, or nullptr.  The plugin name is
		// lower-cased because that is how it was stored; the engine's lookup is
		// case-sensitive.
		RE::TESForm* ResolveSpec(std::string_view a_spec, std::string* a_rest = nullptr)
		{
			const auto colon = a_spec.rfind(':');
			if (colon == std::string_view::npos || colon == 0) {
				return nullptr;
			}

			const auto plugin = ToLower(a_spec.substr(0, colon));
			if (a_rest) {
				*a_rest = ToLower(a_spec.substr(colon + 1));
			}

			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return nullptr;
			}

			const auto spec = std::string{ a_spec.substr(colon + 1) };
			const auto id   = static_cast<RE::FormID>(std::strtoul(spec.c_str(), nullptr, 0));
			if (id == 0) {
				return nullptr;
			}

			return dh->LookupForm(id, plugin);
		}

		// "plugin:id:tail" -> tail after the second colon.
		bool SplitTail(std::string_view a_spec, std::string& a_head, std::string& a_tail)
		{
			const auto last = a_spec.rfind(':');
			if (last == std::string_view::npos || last == 0) {
				return false;
			}
			const auto prev = a_spec.rfind(':', last - 1);
			if (prev == std::string_view::npos) {
				return false;
			}
			a_head = std::string{ a_spec.substr(0, last) };
			a_tail = std::string{ a_spec.substr(last + 1) };
			return true;
		}

		bool Execute(const Command& a_command, std::string& a_note)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();

			if (a_command.verb == "startquest") {
				auto* form = ResolveSpec(a_command.arg);
				auto* quest = form ? form->As<RE::TESQuest>() : nullptr;
				if (!quest) {
					a_note = "任务不存在";
					return false;
				}
				if (quest->IsRunning()) {
					a_note = "任务已在运行";
					return true;
				}
				const bool ok = quest->Start();
				a_note = ok ? "已启动任务" : "启动失败";
				return ok;
			}

			if (a_command.verb == "addspell") {
				auto* form = ResolveSpec(a_command.arg);
				auto* spell = form ? form->As<RE::SpellItem>() : nullptr;
				if (!spell || !player) {
					a_note = "法术不存在";
					return false;
				}
				player->AddSpell(spell);
				a_note = "已授予法术";
				return true;
			}

			if (a_command.verb == "addperk") {
				auto* form = ResolveSpec(a_command.arg);
				auto* perk = form ? form->As<RE::BGSPerk>() : nullptr;
				if (!perk || !player) {
					a_note = "perk 不存在";
					return false;
				}
				player->AddPerk(perk);
				a_note = "已授予 perk";
				return true;
			}

			if (a_command.verb == "additem") {
				std::string head, tail;
				if (!SplitTail(a_command.arg, head, tail)) {
					a_note = "格式应为 plugin:id:数量";
					return false;
				}
				auto* form = ResolveSpec(head);
				auto* base = form ? form->As<RE::TESBoundObject>() : nullptr;
				if (!base || !player) {
					a_note = "物品不存在";
					return false;
				}
				const auto count = static_cast<std::int32_t>(std::atoi(tail.c_str()));
				if (count <= 0) {
					a_note = "数量无效";
					return false;
				}
				player->AddObjectToContainer(base, nullptr, count, nullptr);
				a_note = "已发放 " + std::to_string(count) + " 件";
				return true;
			}

			if (a_command.verb == "setglobal") {
				std::string head, tail;
				if (!SplitTail(a_command.arg, head, tail)) {
					a_note = "格式应为 plugin:id:数值";
					return false;
				}
				auto* form = ResolveSpec(head);
				auto* global = form ? form->As<RE::TESGlobal>() : nullptr;
				if (!global) {
					a_note = "全局变量不存在";
					return false;
				}
				global->value = static_cast<float>(std::atof(tail.c_str()));
				a_note = "已设为 " + tail;
				return true;
			}

			a_note = "未知指令 " + a_command.verb;
			return false;
		}

		void Record(std::string a_line)
		{
			if (s_result.lines.size() < 60) {
				s_result.lines.push_back(std::move(a_line));
			}
		}

		void RunManifests()
		{
			const auto dir = Paths::EnsureDataDir();
			if (dir.empty()) {
				return;
			}

			std::error_code ec;
			const auto      root = dir / "Migrations";
			if (!std::filesystem::is_directory(root, ec)) {
				return;
			}

			std::vector<std::filesystem::path> files;
			for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
				if (ec) {
					break;
				}
				if (!entry.is_regular_file(ec)) {
					continue;
				}
				auto ext = entry.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext == ".ini") {
					files.push_back(entry.path());
				}
			}
			std::sort(files.begin(), files.end());

			for (const auto& path : files) {
				Manifest manifest;
				if (!ParseManifest(path, manifest)) {
					Record("跳过 " + path.filename().string() + "（无有效指令）");
					continue;
				}
				++s_result.manifests;

				const auto key = "migrate/" + manifest.name + "/v";
				std::int64_t applied = 0;
				Vault::GetInt(key, applied);
				if (applied >= manifest.version) {
					++s_result.skipped;
					continue;
				}

				spdlog::info("[Migrate] applying {} v{} ({} commands)",
					manifest.name, manifest.version, manifest.commands.size());

				bool anyFailure = false;
				for (const auto& command : manifest.commands) {
					std::string note;
					bool        ok = false;
					try {
						ok = Execute(command, note);
					} catch (const std::exception& e) {
						note = std::string{ "异常: " } + e.what();
					} catch (...) {
						note = "未知异常";
					}

					if (!ok) {
						anyFailure = true;
						++s_result.failed;
						spdlog::warn("[Migrate] {} {} ({}) -> {}", manifest.name,
							command.verb, command.arg, note);
					} else {
						spdlog::info("[Migrate] {} {} -> {}", manifest.name,
							command.verb, note);
					}
					Record(manifest.name + " · " + command.verb + " · " + note);
				}

				// The version is recorded even when a command failed.  Retrying
				// blindly would re-run the commands that succeeded - and
				// additem is not idempotent.  A failure therefore requires the
				// author to bump `version` to run again, which is visible in the
				// report rather than silent.
				Vault::SetInt(key, manifest.version);
				if (anyFailure) {
					Vault::SetInt("migrate/" + manifest.name + "/failed", manifest.version);
				} else {
					++s_result.applied;
				}
			}
		}

		// A plugin that is loaded but was not loaded when the save was written
		// cannot have run anything.  Any Start-Game-Enabled quest it owns is
		// therefore provably not running - that is the whole detection.
		void ScanCandidates()
		{
			const auto& diff = LoadOrder::Last();
			if (!diff.haveBaseline || diff.added.empty()) {
				return;
			}

			std::unordered_set<std::string> added{ diff.added.begin(), diff.added.end() };

			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			(void)player;

			const auto questIdx = std::to_underlying(RE::FormType::Quest);
			for (auto* form : dh->formArrays[questIdx]) {
				auto* quest = form ? form->As<RE::TESQuest>() : nullptr;
				if (!quest) {
					continue;
				}
				if (!quest->data.flags.any(RE::QuestFlag::kStartsEnabled)) {
					continue;
				}
				if (quest->IsRunning()) {
					continue;
				}

				const auto owner = MakeFormRef(quest).plugin;
				if (owner.empty() || added.find(owner) == added.end()) {
					continue;
				}

				++s_result.candidates;
				const auto label = MakeFormRef(quest).ToString() + "  (" + owner + ")";

				if (s_result.candidates <= 30) {
					Record("候选启动任务 " + label);
				}

				// World mutation now belongs to InitEngine, which owns the
				// write-ahead receipt.  Starting the same quest from two places
				// would race that receipt and could replay an init twice, so
				// this scan is a read-only cross-check only.  It also resolves
				// ownership through FormRef, which does not name ESL plugins -
				// the authoritative counted list is InitEngine's report.
				continue;
			}
		}
	}

	const Result& Last()
	{
		return s_result;
	}

	void RunOnLoad()
	{
		s_result = Result{};

		if (!Ini::Get().migrateEnabled) {
			return;
		}

		try {
			RunManifests();
		} catch (const std::exception& e) {
			spdlog::error("[Migrate] manifest pass threw: {}", e.what());
		} catch (...) {
			spdlog::error("[Migrate] manifest pass threw (unknown)");
		}

		try {
			ScanCandidates();
		} catch (const std::exception& e) {
			spdlog::error("[Migrate] candidate scan threw: {}", e.what());
		} catch (...) {
			spdlog::error("[Migrate] candidate scan threw (unknown)");
		}

		spdlog::info("[Migrate] manifests={} applied={} skipped={} failed={} candidates={} autoStarted={}",
			s_result.manifests, s_result.applied, s_result.skipped,
			s_result.failed, s_result.candidates, s_result.autoStarted);
	}

	void WriteReportSection(std::ostream& a_out)
	{
		a_out << "[存档迁移 / Migration]\n";
		a_out << "  清单数   : " << s_result.manifests << "\n";
		a_out << "  已应用   : " << s_result.applied << "\n";
		a_out << "  已跳过   : " << s_result.skipped << "   （版本已应用过）\n";
		a_out << "  失败指令 : " << s_result.failed << "\n";
		a_out << "  候选任务 : " << s_result.candidates
			  << "   （只读交叉检查；此路径不命名 ESL，准确清单见上文）\n";
		a_out << "  说明     : 自动启动已由「载入期初始化」接管，见上文\n";

		if (!s_result.lines.empty()) {
			a_out << "\n";
			for (const auto& line : s_result.lines) {
				a_out << "    " << line << "\n";
			}
		}
		a_out << "\n";
	}
}
