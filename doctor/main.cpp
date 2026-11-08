// -----------------------------------------------------------------------------
// SSS-Ultra Save Doctor - standalone .exe repair tool.
//
// Launched SILENTLY by SkySaveSystemUltra.dll when it detects (at pre-load)
// that a save references plugins that are no longer installed.  The repair
// happens on the FILE, before the engine parses it, because the engine aborts
// inside BGSSaveLoadManager long before any post-load callback can run.
//
// Protocol with the DLL (files under the save directory):
//
//   <saveDir>/SSS-Ultra.fix         DLL writes this BEFORE launching us:
//                                     line 1: absolute path of the broken .ess
//                                     line 2+: one currently-loaded plugin name
//                                              per line (engine's real load
//                                              order, as TESDataHandler reports)
//   <saveDir>/SSS-Ultra.fix.status  We write "RUNNING" at start, then one of
//                                     "OK <newfile>" / "SKIP" / "FAIL <reason>".
//                                     The DLL polls this; on "OK" it reloads.
//
// The repair is in-place and atomic: we write to a temp file, then rename over
// the original.  The original .ess is NOT backed up (user chose direct replace),
// but the write is crash-safe: rename is atomic on NTFS, so a failed run can
// never leave a half-written save.
// -----------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ess.hpp"

namespace
{
	namespace fs = std::filesystem;

	std::string Trim(const std::string& a_s)
	{
		const auto b = a_s.find_first_not_of(" \t\r\n");
		const auto e = a_s.find_last_not_of(" \t\r\n");
		return (b == std::string::npos) ? std::string{} : a_s.substr(b, e - b + 1);
	}

	void WriteStatus(const fs::path& a_dir, const std::string& a_text)
	{
		const auto p = a_dir / "SSS-Ultra.fix.status";
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		if (out.is_open()) {
			out << a_text;
			out.flush();
		}
	}

	std::vector<std::string> ReadProfile(const fs::path& a_path)
	{
		std::vector<std::string> result;
		std::ifstream in(a_path);
		if (!in.is_open()) {
			return result;
		}
		std::string line;
		while (std::getline(in, line)) {
			line = Trim(line);
			if (line.empty() || line[0] == '#') {
				continue;
			}
			if (line[0] == '*') {
				line = Trim(line.substr(1));
			} else if (line[0] == '-') {
				continue;  // MO2 disabled entry
			}
			if (!line.empty()) {
				result.push_back(line);
			}
		}
		return result;
	}

	// The current load order is the union of loadorder.txt (authoritative
	// order incl. official masters/CC) with any plugin marked enabled in
	// plugins.txt that is not already listed.  This mirrors what the engine
	// actually loads.
	std::vector<std::string> BuildCurrentOrder(const std::vector<std::string>& a_loadOrder,
		const std::vector<std::string>& a_plugins)
	{
		std::vector<std::string> order = a_loadOrder;
		for (const auto& p : a_plugins) {
			const auto lp = doctor::ToLower(p);
			const bool present = std::any_of(order.begin(), order.end(),
				[&](const std::string& o) { return doctor::ToLower(o) == lp; });
			if (!present) {
				order.push_back(p);
			}
		}
		return order;
	}

	std::vector<std::string> FindMissing(const doctor::EssFile& a_save,
		const std::vector<std::string>& a_current)
	{
		std::vector<std::string> curLower;
		for (const auto& c : a_current) {
			curLower.push_back(doctor::ToLower(c));
		}
		std::vector<std::string> missing;
		for (const auto& p : a_save.Plugins()) {
			const auto lp = doctor::ToLower(p);
			if (std::find(curLower.begin(), curLower.end(), lp) == curLower.end()) {
				missing.push_back(p);
			}
		}
		return missing;
	}
}

int main(int a_argc, char** a_argv)
{
	// Usages:
	//   SSS-Ultra-SaveDoctor.exe            -> run the auto-repair from SSS-Ultra.fix
	//   SSS-Ultra-SaveDoctor.exe scan <dir> <plugins.txt> <loadorder.txt>   -> diagnose
	//   SSS-Ultra-SaveDoctor.exe repair <save.ess> <plugins.txt> <loadorder.txt> -> fix
	//   SSS-Ultra-SaveDoctor.exe preflight <saveDir> <pluginsFile> -> fix every broken save
	//   SSS-Ultra-SaveDoctor.exe selftest  -> internal round-trip test
	try {
		if (a_argc >= 2 && std::string(a_argv[1]) == "selftest") {
			// Round-trip the codec on a synthetic buffer with runs and repeats.
			std::vector<std::uint8_t> data;
			for (int i = 0; i < 200000; ++i) {
				data.push_back(static_cast<std::uint8_t>((i % 251) + (i / 251) % 3));
			}
			auto c = doctor::lz4::Compress(data.data(), data.size());
			auto d = doctor::lz4::Decompress(c.data(), c.size(), data.size());
			if (d != data) {
				std::fprintf(stderr, "SELFTEST FAIL: round-trip mismatch\n");
				return 1;
			}
			std::printf("SELFTEST OK: %zu -> %zu -> %zu bytes\n", data.size(), c.size(), d.size());
			return 0;
		}

		if (a_argc >= 2 && std::string(a_argv[1]) == "scan") {
			const fs::path dir(a_argv[2]);
			const auto current = BuildCurrentOrder(
				ReadProfile(a_argc >= 4 ? fs::path(a_argv[4]) : fs::path{}),
				ReadProfile(a_argc >= 3 ? fs::path(a_argv[3]) : fs::path{}));
			std::printf("=== SSS-Ultra Save Doctor / scan ===\ncurrent load order: %zu entries\n\n",
				current.size());
			for (const auto& e : fs::directory_iterator(dir)) {
				if (!e.is_regular_file() || e.path().extension() != ".ess") {
					continue;
				}
				try {
					auto save = doctor::Load(e.path());
					auto missing = FindMissing(save, current);
					std::printf("  %s\n    plugins=%zu missing=%zu\n",
						e.path().filename().string().c_str(), save.Plugins().size(), missing.size());
					for (const auto& m : missing) {
						std::printf("      - %s\n", m.c_str());
					}
				} catch (const std::exception& ex) {
					std::printf("  %s\n    BROKEN: %s\n", e.path().filename().string().c_str(), ex.what());
				}
			}
			return 0;
		}

		if (a_argc >= 2 && std::string(a_argv[1]) == "repair") {
			const fs::path save(a_argv[2]);
			const auto current = BuildCurrentOrder(
				ReadProfile(a_argc >= 4 ? fs::path(a_argv[4]) : fs::path{}),
				ReadProfile(a_argc >= 3 ? fs::path(a_argv[3]) : fs::path{}));
			auto s = doctor::Load(save);
			auto missing = FindMissing(s, current);
			std::printf("=== SSS-Ultra Save Doctor / repair ===\nfile: %s\nplugins=%zu missing=%zu\n",
				save.string().c_str(), s.Plugins().size(), missing.size());
			if (missing.empty()) {
				std::printf("nothing to repair\n");
				return 0;
			}
			std::vector<doctor::Substitution> log;
			auto newBody = doctor::Substitute(s, current, missing, log);
			std::printf("substitutions (%zu):\n", log.size());
			for (const auto& c : log) {
				std::printf("  %s -> %s\n", c.from.c_str(), c.to.c_str());
			}
			// Atomic replace: write temp, rename over original.
			const auto tmp = save.string() + ".SSS-Ultra.tmp";
			doctor::BuildWithBody(s, newBody, tmp);
			fs::rename(tmp, save);
			std::printf("repaired in place: %s\n", save.string().c_str());
			return 0;
		}

		if (a_argc >= 2 && std::string(a_argv[1]) == "preflight") {
			// Scan a whole save directory and repair every save that references
			// plugins no longer installed.  Run silently by the DLL at game
			// startup (kDataLoaded), BEFORE the player opens the load menu, so
			// the engine's "missing content" confirmation never appears.  The
			// load order arrives as a file: each line is one plugin name.
			//   SSS-Ultra-SaveDoctor.exe preflight <saveDir> <pluginsFile>
			const fs::path dir(a_argv[2]);
			const auto current = ReadProfile(a_argc >= 4 ? fs::path(a_argv[3]) : fs::path{});

			std::size_t scanned = 0, repaired = 0, skipped = 0, failed = 0;
			std::vector<std::string> fixed;

			std::error_code ec;
			if (!fs::is_directory(dir, ec)) {
				std::fprintf(stderr, "preflight: not a directory: %s\n", dir.string().c_str());
				return 2;
			}

			for (const auto& e : fs::directory_iterator(dir)) {
				if (!e.is_regular_file() || e.path().extension() != ".ess") {
					continue;
				}
				++scanned;
				try {
					auto save = doctor::Load(e.path());
					auto missing = FindMissing(save, current);
					if (missing.empty()) {
						++skipped;
						continue;
					}
					std::vector<doctor::Substitution> log;
					auto newBody = doctor::Substitute(save, current, missing, log);
					const auto tmp = e.path().string() + ".SSS-Ultra.tmp";
					doctor::BuildWithBody(save, newBody, tmp);
					fs::rename(tmp, e.path());

					// Drop the co-save so SKSE starts clean.
					auto cosavePath = e.path();
					cosavePath.replace_extension("skse");
					if (fs::exists(cosavePath, ec)) {
						fs::remove(cosavePath, ec);
					}
					++repaired;
					fixed.push_back(e.path().filename().string());
				} catch (const std::exception& ex) {
					++failed;
					std::fprintf(stderr, "preflight: %s: %s\n",
						e.path().filename().string().c_str(), ex.what());
				}
			}

			std::printf("preflight: scanned=%zu repaired=%zu skipped=%zu failed=%zu\n",
				scanned, repaired, skipped, failed);
			for (const auto& n : fixed) {
				std::printf("  fixed: %s\n", n.c_str());
			}
			return 0;
		}

		// --- auto mode (default): read SSS-Ultra.fix in cwd, or accept paths via args ---
		//
		// Contract file format (written by the DLL before launching us):
		//   line 1: absolute path of the broken .ess
		//   line 2+: one currently-loaded plugin name per line (engine's real
		//            load order, as reported by TESDataHandler)
		//
		// Status file <saveDir>/SSS-Ultra.fix.status:
		//   "RUNNING" -> "OK <filename>" | "SKIP" | "FAIL <reason>"
		fs::path fixFile;
		if (a_argc >= 2) {
			fixFile = fs::path(a_argv[1]);
		} else {
			fixFile = fs::path("SSS-Ultra.fix");
		}

		std::ifstream in(fixFile);
		if (!in.is_open()) {
			std::fprintf(stderr, "no SSS-Ultra.fix contract file\n");
			return 2;
		}

		std::string saveLine;
		if (!std::getline(in, saveLine)) {
			std::fprintf(stderr, "empty SSS-Ultra.fix contract\n");
			return 2;
		}
		saveLine = Trim(saveLine);

		std::vector<std::string> current;
		{
			std::string line;
			while (std::getline(in, line)) {
				line = Trim(line);
				if (!line.empty()) {
					current.push_back(line);
				}
			}
		}

		const fs::path savePath(saveLine);
		const fs::path dir = savePath.parent_path();
		WriteStatus(dir, "RUNNING");

		if (savePath.extension() != ".ess") {
			WriteStatus(dir, "FAIL not-an-ess");
			return 3;
		}

		auto save = doctor::Load(savePath);
		auto missing = FindMissing(save, current);

		if (missing.empty()) {
			// Nothing to fix - the DLL may have raced with a load-order change.
			WriteStatus(dir, "SKIP");
			return 0;
		}

		std::vector<doctor::Substitution> log;
		auto newBody = doctor::Substitute(save, current, missing, log);

		// Write temp + atomic rename over the original.
		const auto tmp = savePath.string() + ".SSS-Ultra.tmp";
		doctor::BuildWithBody(save, newBody, tmp);
		fs::rename(tmp, savePath);

		// Drop the co-save so SKSE starts clean (removes orphaned Papyrus state).
		std::error_code ec;
		auto cosavePath = savePath;
		cosavePath.replace_extension("skse");
		if (fs::exists(cosavePath, ec)) {
			fs::remove(cosavePath, ec);
		}

		WriteStatus(dir, "OK " + savePath.filename().string());
		return 0;

	} catch (const std::exception& ex) {
		std::fprintf(stderr, "SSS-Ultra-SaveDoctor FAIL: %s\n", ex.what());
		// Try to report into the status file if we know the dir.
		if (a_argc >= 2 && std::string(a_argv[1]) != "scan" &&
			std::string(a_argv[1]) != "repair" && std::string(a_argv[1]) != "selftest") {
			try {
				std::ifstream in(a_argv[1]);
				std::string l;
				std::getline(in, l);
				l = Trim(l);
				if (!l.empty()) {
					WriteStatus(fs::path(l).parent_path(), std::string("FAIL ") + ex.what());
				}
			} catch (...) {}
		}
		return 1;
	}
}
