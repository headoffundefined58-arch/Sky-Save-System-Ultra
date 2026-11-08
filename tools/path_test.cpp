// Runs the real save-directory discovery against the real disk, without the
// game.  This is the part that silently failed in v0.4.1 (a shared
// std::error_code made the drive-root walk stop at $RECYCLE.BIN), so it is
// worth being able to run it on demand instead of inferring it from logs.
//
// Usage: PathTest.exe ["E:\\Skyrim Special Edition"]
#include "SavePathScan.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	struct Hit
	{
		std::filesystem::path dir;
		std::string           why;
		std::size_t           saves{ 0 };
	};

	std::size_t CountSaves(const std::filesystem::path& a_dir)
	{
		std::error_code iter;
		std::filesystem::directory_iterator it(a_dir, iter);
		if (iter) {
			return 0;
		}

		std::size_t count = 0;
		const auto  end   = std::filesystem::directory_iterator{};
		for (; it != end; it.increment(iter)) {
			if (iter) {
				break;
			}
			std::error_code ec;
			if (!it->is_regular_file(ec) || ec) {
				continue;
			}
			auto ext = it->path().extension().string();
			for (auto& c : ext) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (ext == ".ess") {
				++count;
			}
		}
		return count;
	}

	void Collect(const std::filesystem::path& a_root, const char* a_kind, std::vector<Hit>& a_out)
	{
		S3U::PathScan::AddProfiles(a_root, [&](const std::filesystem::path& a_dir,
													std::string_view           a_why,
													const std::filesystem::path& a_profile) {
			std::string why{ a_kind };
			why += "/";
			why += a_why;
			if (!a_profile.empty()) {
				why += "/";
				why += a_profile.string();
			}
			a_out.push_back({ a_dir, why, CountSaves(a_dir) });
		});
	}

	void CollectInstance(const std::filesystem::path& a_root, std::vector<Hit>& a_out)
	{
		S3U::PathScan::ScanInstance(a_root, [&](const std::filesystem::path& a_dir,
												 std::string_view           a_why,
												 const std::filesystem::path& a_profile) {
			std::string why{ "instance" };
			why += "/";
			why += a_why;
			if (!a_profile.empty()) {
				why += "/";
				why += a_profile.string();
			}
			a_out.push_back({ a_dir, why, CountSaves(a_dir) });
		});
	}
}

int main(int a_argc, char** a_argv)
{
	const std::filesystem::path gameRoot = a_argc > 1 ? a_argv[1] : std::filesystem::path{ "E:\\Skyrim Special Edition" };

	std::vector<Hit> hits;

	// Same order the plugin uses: working directory and its parents, then the
	// game's own drive, then every other drive root.
	std::error_code ec;
	auto            here = std::filesystem::current_path(ec);
	for (int depth = 0; depth < 4 && !here.empty(); ++depth) {
		Collect(here, "workdir", hits);
		const auto parent = here.parent_path();
		if (parent == here) {
			break;
		}
		here = parent;
	}

	auto root = gameRoot;
	if (!root.empty()) {
		Collect(root, "gameroot", hits);
		for (int up = 0; up < 3; ++up) {
			const auto parent = root.parent_path();
			if (parent.empty() || parent == root) {
				break;
			}
			root = parent;
			Collect(root, "gameroot-up", hits);
		}
		CollectInstance(root.root_path(), hits);
	}

	for (const wchar_t letter : { L'C', L'D', L'E' }) {
		wchar_t drive[4]{ letter, L':', L'\\', L'\0' };
		std::error_code probe;
		if (!std::filesystem::is_directory(std::filesystem::path{ drive }, probe) || probe) {
			continue;
		}
		CollectInstance(std::filesystem::path{ drive }, hits);
	}

	std::printf("candidates: %zu\n", hits.size());
	std::size_t withSaves = 0;
	for (const auto& hit : hits) {
		std::printf("  %-46s saves=%zu  %s\n", hit.why.c_str(), hit.saves,
			hit.dir.string().c_str());
		if (hit.saves > 0) {
			++withSaves;
		}
	}

	if (withSaves == 0) {
		std::printf("\nFAIL: no candidate directory contains a .ess file\n");
		return 1;
	}

	std::printf("\nPASS: %zu candidate(s) actually hold saves\n", withSaves);
	return 0;
}
