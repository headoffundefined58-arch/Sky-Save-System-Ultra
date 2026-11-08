#include "PCH.h"

#include "SaveDoctor.h"
#include "SaveGuard.h"
#include "Ini.h"
#include "LoadOrder.h"

#include "ess.hpp"

#include <fstream>
#include <system_error>

// -----------------------------------------------------------------------------
// Windows API, declared by hand so this translation unit never includes
// <windows.h> or <shlobj.h>: CommonLibSSE hard-errors if the SDK headers land
// before its own.  kernel32 provides these imports.
// -----------------------------------------------------------------------------
extern "C" __declspec(dllimport) int __stdcall CreateProcessW(
	const wchar_t* a_app, wchar_t* a_cmd, void* a_procAttr, void* a_threadAttr,
	int a_inherit, unsigned long a_flags, void* a_env, const wchar_t* a_cwd,
	void* a_si, void* a_pi);

extern "C" __declspec(dllimport) unsigned long __stdcall GetLastError();

namespace S3U::SaveDoctor
{
	namespace
	{
		constexpr const wchar_t* kExeName = L"SSS-Ultra-SaveDoctor.exe";
		constexpr const char*    kFixFile   = "SSS-Ultra.fix";
		constexpr const char*    kStatusFile = "SSS-Ultra.fix.status";

		constexpr unsigned long kCreateNoWindow = 0x08000000;
		constexpr unsigned long kCreateNewProcessGroup = 0x00000200;

		struct StartupInfoW
		{
			unsigned long cb;
			wchar_t*      reserved;
			wchar_t*      desktop;
			wchar_t*      title;
			unsigned long x, y, xSize, ySize, xCountChars, yCountChars;
			unsigned long fillAttribute;
			unsigned long flags;
			unsigned short showWindow;
			unsigned short cbReserved2;
			unsigned char* lpReserved2;
			void*          hStdInput;
			void*          hStdOutput;
			void*          hStdError;
		};

		struct ProcessInfoW
		{
			void*         hProcess;
			void*         hThread;
			unsigned long dwProcessId;
			unsigned long dwThreadId;
		};

		// Current in-flight repair state.
		std::filesystem::path s_saveFile;
		std::filesystem::path s_saveDir;
		bool                  s_dispatched = false;
		bool                  s_reloaded = false;

		std::string ToLower(std::string a_s)
		{
			for (auto& c : a_s) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return a_s;
		}

		// The engine's real load order, lower-cased for comparison.
		std::vector<std::string> CurrentLoadOrder()
		{
			std::vector<std::string> out;
			for (const auto& mod : LoadOrder::Snapshot()) {
				out.push_back(mod.name);  // already lower-case in LoadOrder
			}
			return out;
		}

		// Find the exe.  It ships in the mod's own folder, which MO2 maps to
		// <game>\Data\SkySaveSystemUltra\SSS-Ultra-SaveDoctor.exe.  Fall back to the
		// plugin directory (<game>\Data\SKSE\Plugins\) so a manual install next
		// to the DLL also works.
		std::filesystem::path ExePath()
		{
			auto root = Paths::GameRoot();
			if (root.empty()) {
				return {};
			}
			const std::filesystem::path candidates[] = {
				root / "Data" / "SkySaveSystemUltra" / kExeName,
				Paths::PluginDir() / kExeName,
			};
			std::error_code ec;
			for (const auto& c : candidates) {
				if (std::filesystem::exists(c, ec)) {
					return c;
				}
			}
			return candidates[0];
		}

		// Silently start the exe with the given argument vector (already
		// quoted).  Returns the process id, or 0 on failure.  CREATE_NO_WINDOW
		// + STARTF_USESHOWWINDOW/SW_HIDE keep it fully invisible.
		unsigned long LaunchExe(const std::vector<std::wstring>& a_args)
		{
			const auto exe = ExePath();
			if (exe.empty()) {
				spdlog::error("[SaveDoctor] cannot locate {}", "SSS-Ultra-SaveDoctor.exe");
				return 0;
			}

			std::wstring cmd = L"\"" + exe.wstring() + L"\"";
			for (const auto& a : a_args) {
				cmd += L" \"";
				cmd += a;
				cmd += L"\"";
			}

			StartupInfoW si{};
			si.cb = sizeof(si);
			si.showWindow = 0;  // SW_HIDE
			si.flags = 0x00000001;  // STARTF_USESHOWWINDOW

			ProcessInfoW pi{};

			std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
			cmdBuf.push_back(L'\0');

			const int ok = ::CreateProcessW(
				exe.wstring().c_str(), cmdBuf.data(),
				nullptr, nullptr, 0,
				kCreateNoWindow | kCreateNewProcessGroup,
				nullptr, nullptr, &si, &pi);

			if (!ok) {
				spdlog::error("[SaveDoctor] CreateProcessW failed (GLE={})", ::GetLastError());
				return 0;
			}
			return pi.dwProcessId;
		}
	}

	Probe Inspect(const std::filesystem::path& a_saveFile)
	{
		Probe p;
		try {
			auto save = doctor::Load(a_saveFile);
			p.parsed  = true;
			p.plugins = save.Plugins().size();

			const auto current = CurrentLoadOrder();
			for (const auto& name : save.Plugins()) {
				const auto ln = ToLower(name);
				if (std::find(current.begin(), current.end(), ln) == current.end()) {
					p.missing.push_back(name);
				}
			}
			if (p.missing.empty()) {
				p.note = "插件表与当前加载顺序一致";
			} else {
				p.note = "缺失 " + std::to_string(p.missing.size()) + " 个插件";
			}
		} catch (const std::exception& e) {
			p.note = std::string("解析失败: ") + e.what();
		} catch (...) {
			p.note = "解析失败（未知）";
		}
		return p;
	}

	bool OnPreLoadGame(const std::filesystem::path& a_saveFile,
		const std::filesystem::path& a_saveDir)
	{
		Reset();
		s_saveFile = a_saveFile;
		s_saveDir  = a_saveDir;

		const auto probe = Inspect(a_saveFile);
		if (!probe.parsed) {
			spdlog::warn("[SaveDoctor] cannot parse {}: {} - not dispatching repair",
				PathToUtf8(a_saveFile), probe.note);
			return false;
		}

		if (probe.missing.empty()) {
			// Healthy save: never touch it, never launch the exe.
			spdlog::info("[SaveDoctor] {} healthy ({} plugins), no repair",
				PathToUtf8(a_saveFile), probe.plugins);
			return false;
		}

		spdlog::warn("[SaveDoctor] BROKEN save detected: {} — {} missing plugin(s):",
			PathToUtf8(a_saveFile), probe.missing.size());
		for (const auto& m : probe.missing) {
			spdlog::warn("[SaveDoctor]   - {}", m);
		}

		// Write the contract file.
		const auto fixFile = a_saveDir / kFixFile;
		{
			std::ofstream out(fixFile, std::ios::binary | std::ios::trunc);
			if (!out.is_open()) {
				spdlog::error("[SaveDoctor] cannot write contract file {}", PathToUtf8(fixFile));
				return false;
			}
			out << PathToUtf8(a_saveFile) << "\n";
			for (const auto& mod : LoadOrder::Snapshot()) {
				out << mod.name << "\n";
			}
			out.flush();
		}

		// Remove any stale status file so a previous run's "OK" is not misread.
		std::error_code ec;
		const auto statusFile = a_saveDir / kStatusFile;
		std::filesystem::remove(statusFile, ec);

		// Silently launch the exe (auto mode: the fix file path is the sole arg).
		const auto pid = LaunchExe({ fixFile.wstring() });
		if (pid == 0) {
			return false;
		}

		spdlog::info("[SaveDoctor] dispatched SSS-Ultra-SaveDoctor.exe (pid {}) for {}",
			pid, PathToUtf8(a_saveFile));

		s_dispatched = true;
		return true;
	}

	PollState PollReload()
	{
		if (!s_dispatched || s_reloaded) {
			return PollState::kRunning;
		}

		const auto statusFile = s_saveDir / kStatusFile;
		std::ifstream in(statusFile);
		if (!in.is_open()) {
			return PollState::kRunning;  // exe not finished yet
		}

		std::string line;
		std::getline(in, line);

		if (line.rfind("OK", 0) == 0) {
			return PollState::kReady;
		}
		if (line.rfind("SKIP", 0) == 0) {
			return PollState::kDone;
		}
		if (line.rfind("FAIL", 0) == 0) {
			spdlog::error("[SaveDoctor] repair failed: {}", line);
			return PollState::kFailed;
		}
		if (line.rfind("RUNNING", 0) == 0) {
			return PollState::kRunning;
		}
		// Unknown content - treat as still running to avoid a premature reload.
		return PollState::kRunning;
	}

	void ReloadNow()
	{
		if (s_reloaded || s_saveFile.empty()) {
			return;
		}

		// Never issue a load while the engine is already loading (the broken
		// save attempt may still be in flight).  If it is, leave s_reloaded
		// false so the caller re-arms the poll for the next frame.
		if (auto* game = RE::BGSSaveLoadGame::GetSingleton()) {
			if (game->flags.any(RE::BGSSaveLoadGame::Flags::kLoading)) {
				return;
			}
		}

		s_reloaded = true;

		// The engine takes the save *file name* (no directory).  Reload the
		// now-repaired save in place.
		const auto name = s_saveFile.filename().string();
		spdlog::info("[SaveDoctor] reloading repaired save: {}", name);

		if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
			mgr->Load(name.c_str());
		} else {
			spdlog::error("[SaveDoctor] BGSSaveLoadManager unavailable; cannot reload");
		}
	}

	void Reset()
	{
		s_saveFile.clear();
		s_saveDir.clear();
		s_dispatched = false;
		s_reloaded  = false;
	}

	void Preflight()
	{
		// Build the current load order (lower-cased plugin names).
		std::vector<std::string> order;
		for (const auto& mod : LoadOrder::Snapshot()) {
			order.push_back(mod.name);
		}
		if (order.empty()) {
			spdlog::warn("[SaveDoctor] preflight: empty load order, skipping");
			return;
		}

		// The exe's preflight mode reads the load order from a file (one name
		// per line).  Write it once into the plugin data directory.
		const auto dataDir = Paths::EnsureDataDir();
		if (dataDir.empty()) {
			spdlog::error("[SaveDoctor] preflight: no data directory, skipping");
			return;
		}
		const auto pluginsFile = dataDir / "preflight-loadorder.txt";
		{
			std::ofstream out(pluginsFile, std::ios::binary | std::ios::trunc);
			if (!out.is_open()) {
				spdlog::error("[SaveDoctor] preflight: cannot write {}", PathToUtf8(pluginsFile));
				return;
			}
			for (const auto& n : order) {
				out << n << "\n";
			}
			out.flush();
		}

		// Launch one exe per real, existing save directory.  Each runs its
		// preflight scan, repairs every broken save, then exits.  Distinct
		// directories only: the candidate list can repeat a path via
		// different discovery routes.
		std::vector<std::filesystem::path> seen;
		std::error_code ec;
		for (const auto& dir : Guard::CandidateDirs()) {
			if (!std::filesystem::is_directory(dir, ec)) {
				continue;
			}
			bool dup = false;
			for (const auto& s : seen) {
				if (std::filesystem::equivalent(s, dir, ec)) {
					dup = true;
					break;
				}
			}
			if (dup) {
				continue;
			}
			seen.push_back(dir);

			const auto pid = LaunchExe({ L"preflight", dir.wstring(), pluginsFile.wstring() });
			if (pid != 0) {
				spdlog::info("[SaveDoctor] preflight dispatched (pid {}) for {}", pid, PathToUtf8(dir));
			}
		}
	}
}
