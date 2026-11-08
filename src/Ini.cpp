#include "PCH.h"

#include "Ini.h"

#include <cctype>
#include <cstdlib>
#include <system_error>

namespace S3U
{
	namespace Paths
	{
		std::filesystem::path GameRoot()
		{
			// filePath() is the path of the running executable, e.g.
			// "E:\Skyrim Special Edition\SkyrimSE.exe".
			auto exe = REL::Module::get().filePath();
			std::filesystem::path p(exe);
			std::error_code       ec;
			auto                  parent = p.parent_path();
			if (!parent.empty()) {
				return parent;
			}
			auto cwd = std::filesystem::current_path(ec);
			return ec ? std::filesystem::path{} : cwd;
		}

		std::filesystem::path PluginDir()
		{
			auto root = GameRoot();
			if (root.empty()) {
				return {};
			}
			return root / "Data" / "SKSE" / "Plugins";
		}

		std::filesystem::path IniFile()
		{
			auto dir = PluginDir();
			if (dir.empty()) {
				return {};
			}
			return dir / "SkySaveSystemUltra.ini";
		}

		std::filesystem::path EnsureDataDir()
		{
			auto dir = PluginDir();
			if (dir.empty()) {
				return {};
			}
			auto out = dir / "SkySaveSystemUltra";
			std::error_code ec;
			std::filesystem::create_directories(out, ec);
			if (ec) {
				spdlog::warn("[3SU] cannot create {}: {}", out.string(), ec.message());
				return {};
			}
			return out;
		}
	}

	namespace Ini
	{
		namespace
		{
			Settings s_settings;

			std::string_view Trim(std::string_view a_view)
			{
				auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
				while (!a_view.empty() && isSpace(a_view.front())) {
					a_view.remove_prefix(1);
				}
				while (!a_view.empty() && isSpace(a_view.back())) {
					a_view.remove_suffix(1);
				}
				return a_view;
			}

			bool ToBool(std::string_view a_value, bool a_default)
			{
				if (a_value.empty()) {
					return a_default;
				}
				const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(a_value.front())));
				if (c == '1' || c == 't' || c == 'y') {
					return true;
				}
				if (c == '0' || c == 'f' || c == 'n') {
					return false;
				}
				return a_default;
			}

			// Never let std::sto* escape: an ini with a stray character must
			// degrade to the default, not take the process down.  (std::stoul
			// throwing out_of_range through an SKSE callback is a CTD.)
			std::int32_t ToInt(std::string_view a_value, std::int32_t a_default)
			{
				if (a_value.empty()) {
					return a_default;
				}
				try {
					return static_cast<std::int32_t>(std::stol(std::string{ a_value }));
				} catch (...) {
					return a_default;
				}
			}
		}

		const Settings& Get()
		{
			return s_settings;
		}

		void Load()
		{
			s_settings = Settings{};

			const auto path = Paths::IniFile();
			if (path.empty()) {
				return;
			}

			std::ifstream file(path);
			if (!file.is_open()) {
				spdlog::info("[3SU] no {} - using defaults", path.string());
				return;
			}

			std::string line;
			while (std::getline(file, line)) {
				std::string_view view{ line };
				if (const auto hash = view.find_first_of(";#"); hash != std::string_view::npos) {
					view = view.substr(0, hash);
				}
				view = Trim(view);
				if (view.empty() || view.front() == '[') {
					continue;
				}
				const auto eq = view.find('=');
				if (eq == std::string_view::npos) {
					continue;
				}
				const auto key   = Trim(view.substr(0, eq));
				const auto value = Trim(view.substr(eq + 1));

				if (key == "bAuditOnLoad") {
					s_settings.auditOnLoad = ToBool(value, s_settings.auditOnLoad);
				} else if (key == "bScanReferences") {
					s_settings.scanReferences = ToBool(value, s_settings.scanReferences);
				} else if (key == "bNotify") {
					s_settings.notify = ToBool(value, s_settings.notify);
				} else if (key == "iNotifyWidth") {
					s_settings.notifyWidth = ToInt(value, s_settings.notifyWidth);
				} else if (key == "iMaxCells") {
					s_settings.maxCells = std::max(0, ToInt(value, s_settings.maxCells));
				} else if (key == "iGrowthWarnPercent") {
					s_settings.growthWarnPercent = std::max(0, ToInt(value, s_settings.growthWarnPercent));
				} else if (key == "bVerbose") {
					s_settings.verbose = ToBool(value, s_settings.verbose);
				} else if (key == "bSaveGuard") {
					s_settings.guardEnabled = ToBool(value, s_settings.guardEnabled);
				} else if (key == "iBackupCount") {
					s_settings.backupCount = std::max(0, ToInt(value, s_settings.backupCount));
				} else if (key == "sSaveDir") {
					s_settings.saveDir = std::string{ value };
				} else if (key == "bVault") {
					s_settings.vaultEnabled = ToBool(value, s_settings.vaultEnabled);
				} else if (key == "iVaultHistory") {
					s_settings.vaultHistory = std::max(1, ToInt(value, s_settings.vaultHistory));
				} else if (key == "bVaultReconcile") {
					s_settings.vaultReconcile = ToBool(value, s_settings.vaultReconcile);
				} else if (key == "bMigrate") {
					s_settings.migrateEnabled = ToBool(value, s_settings.migrateEnabled);
			} else if (key == "bAutoStartNewModQuests") {
				s_settings.autoStartNewModQuests = ToBool(value, s_settings.autoStartNewModQuests);
			} else if (key == "bInitOnLoad") {
				s_settings.initEnabled = ToBool(value, s_settings.initEnabled);
			} else if (key == "bRepairOnLoad") {
				s_settings.repairEnabled = ToBool(value, s_settings.repairEnabled);
			} else if (key == "bRepairPrompt") {
				s_settings.repairPrompt = ToBool(value, s_settings.repairPrompt);
			}
		}

		spdlog::info("[3SU] ini: audit={} refs={} notify={} verify={} init={} repair={} prompt={} (no backups, no history)",
			s_settings.auditOnLoad, s_settings.scanReferences, s_settings.notify,
			s_settings.guardEnabled, s_settings.initEnabled,
			s_settings.repairEnabled, s_settings.repairPrompt);
		}
	}
}
