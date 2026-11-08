#include "SavePathScan.h"

#include <system_error>
#include <utility>

namespace S3U::PathScan
{
	namespace
	{
		bool IsDirectory(const std::filesystem::path& a_path)
		{
			std::error_code ec;
			const auto      result = std::filesystem::is_directory(a_path, ec);
			return result && !ec;
		}

		bool IsFile(const std::filesystem::path& a_path)
		{
			std::error_code ec;
			const auto      result = std::filesystem::is_regular_file(a_path, ec);
			return result && !ec;
		}

		// Iterates one directory and hands every readable subdirectory to
		// a_visit.  A failure on a single entry is skipped; only a failure to
		// open the directory ends the walk.  That distinction is the whole
		// point of this file.
		template <class Fn>
		void ForEachSubdirectory(const std::filesystem::path& a_root, std::size_t a_cap, Fn&& a_visit)
		{
			std::error_code iter;
			std::filesystem::directory_iterator it(a_root, iter);
			if (iter) {
				return;
			}

			const auto end = std::filesystem::directory_iterator{};
			std::size_t seen = 0;
			for (; it != end; it.increment(iter)) {
				if (iter || ++seen > a_cap) {
					return;
				}
				std::error_code ec;
				if (!it->is_directory(ec) || ec) {
					continue;
				}
				a_visit(it->path());
			}
		}
	}

	void AddProfiles(const std::filesystem::path& a_root, Emit a_emit)
	{
		if (!a_emit) {
			return;
		}

		for (const auto* spelling : { "Profiles", "profiles" }) {
			const auto profiles = a_root / spelling;
			if (!IsDirectory(profiles)) {
				continue;
			}

			ForEachSubdirectory(profiles, 200, [&](const std::filesystem::path& a_profile) {
				const auto saves = a_profile / "saves";
				if (IsDirectory(saves)) {
					a_emit(saves, "profile", a_profile.filename());
				}
			});
		}
	}

	void ScanInstance(const std::filesystem::path& a_root, Emit a_emit)
	{
		if (!a_emit || !IsDirectory(a_root)) {
			return;
		}

		if (IsFile(a_root / "ModOrganizer.ini")) {
			AddProfiles(a_root, a_emit);
		}

		ForEachSubdirectory(a_root, 400, [&](const std::filesystem::path& a_child) {
			if (IsFile(a_child / "ModOrganizer.ini")) {
				AddProfiles(a_child, a_emit);
			}
		});
	}
}
