#include "PCH.h"

#include "SaveGuard.h"
#include "Ini.h"
#include "SavePathScan.h"
#include "SaveHeader.h"

#include <cstdio>
#include <cstring>
#include <system_error>

// Declared by hand at global scope so this translation unit never sees
// <windows.h> or <shlobj.h>: CommonLibSSE hard-errors if the Windows SDK
// headers land before its own.  shell32.lib provides the import.
extern "C" __declspec(dllimport) long __stdcall SHGetFolderPathW(
	void*         a_hwnd,
	int           a_csidl,
	void*         a_token,
	unsigned long a_flags,
	wchar_t*      a_path);

extern "C" __declspec(dllimport) unsigned long __stdcall GetLogicalDrives();

extern "C" __declspec(dllimport) int __stdcall WideCharToMultiByte(
	unsigned int  a_codePage,
	unsigned long a_flags,
	const wchar_t* a_wide,
	int           a_wideLen,
	char*         a_out,
	int           a_outLen,
	const char*   a_default,
	int*          a_usedDefault);

namespace S3U
{
	std::string PathToUtf8(const std::filesystem::path& a_path)
	{
		const auto wide = a_path.wstring();
		if (wide.empty()) {
			return {};
		}
		constexpr unsigned int kUtf8 = 65001;
		const int need = ::WideCharToMultiByte(kUtf8, 0, wide.c_str(),
			static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		if (need <= 0) {
			return {};
		}
		std::string out(static_cast<std::size_t>(need), '\0');
		if (::WideCharToMultiByte(kUtf8, 0, wide.c_str(), static_cast<int>(wide.size()),
				out.data(), need, nullptr, nullptr) != need) {
			return {};
		}
		return out;
	}
}

namespace S3U
{
	namespace
	{
		constexpr int kCSIDL_PERSONAL = 0x0005;

		// Size/format rules now live in SaveHeader.h so the plugin and
		// HeaderTest parse bytes with exactly the same code.
		Guard::Status s_status;
		bool          s_initialized = false;

		struct Candidate
		{
			std::filesystem::path dir;
			std::string           why;
		};

		std::vector<Candidate> s_candidates;

		std::filesystem::path DocumentsDir()
		{
			wchar_t buffer[260]{};
			const long hr = ::SHGetFolderPathW(nullptr, kCSIDL_PERSONAL, nullptr, 0, buffer);
			if (hr >= 0 && buffer[0] != L'\0') {
				return std::filesystem::path{ buffer };
			}
			spdlog::warn("[3SU] SHGetFolderPathW failed (0x{:08X})", static_cast<unsigned>(hr));
			return {};
		}

		void AddCandidate(const std::filesystem::path& a_dir, std::string a_why)
		{
			if (a_dir.empty()) {
				return;
			}
			std::error_code ec;
			const auto      normal = std::filesystem::weakly_canonical(a_dir, ec);
			const auto      target = ec ? a_dir : normal;

			for (const auto& existing : s_candidates) {
				std::error_code inner;
				if (std::filesystem::equivalent(existing.dir, target, inner)) {
					return;
				}
				if (existing.dir == target) {
					return;
				}
			}
			s_candidates.push_back({ target, std::move(a_why) });
		}

		// A mod manager with per-profile saves keeps them at
		// <instance root>\profiles\<name>\saves.  The instance root is not
		// recorded anywhere the game can see, but it is almost always the
		// working directory MO2 launched us from, or one of its parents.
		// Candidate discovery itself lives in SavePathScan.cpp: pure
		// std::filesystem, no engine, no ini - which is what lets
		// tools/path_test.cpp run it against the real disk.
		void BuildCandidates()
		{
			const auto emit = [](const std::filesystem::path& a_dir,
								  std::string_view           a_kind,
								  const std::filesystem::path& a_profile) {
				std::string why{ a_kind };
				if (!a_profile.empty()) {
					why += "/";
					why += PathToUtf8(a_profile);
				}
				AddCandidate(a_dir, std::move(why));
			};

			if (!Ini::Get().saveDir.empty()) {
				AddCandidate(std::filesystem::path{ Ini::Get().saveDir }, "ini");
			}

			const auto docs = DocumentsDir();
			if (!docs.empty()) {
				AddCandidate(docs / "My Games" / "Skyrim Special Edition" / "Saves", "documents");
			}

			std::error_code ec;
			auto            here = std::filesystem::current_path(ec);
			for (int depth = 0; depth < 4 && !here.empty(); ++depth) {
				PathScan::AddProfiles(here, emit);
				const auto parent = here.parent_path();
				if (parent == here) {
					break;
				}
				here = parent;
			}

			// The game's own install directory pins the drive the instance must
			// live on, and that is one fact we do not have to guess at.  It is
			// also the probe that still works when GetLogicalDrives comes back
			// empty, which happens when the launcher hands the game a
			// restricted token.
			auto gameRoot = Paths::GameRoot();
			if (!gameRoot.empty()) {
				PathScan::AddProfiles(gameRoot, emit);
				for (int up = 0; up < 3; ++up) {
					const auto parent = gameRoot.parent_path();
					if (parent.empty() || parent == gameRoot) {
						break;
					}
					gameRoot = parent;
					PathScan::AddProfiles(gameRoot, emit);
				}
				PathScan::ScanInstance(gameRoot.root_path(), emit);
			}

			const auto mask = ::GetLogicalDrives();
			spdlog::info("[SaveGuard] candidate scan: game root drive probed, logical drives 0x{:X}",
				static_cast<unsigned int>(mask));
			for (int i = 2; i < 26; ++i) {  // A:/B: are floppies on ancient boards
				if ((mask & (1u << i)) == 0) {
					continue;
				}
				wchar_t root[8]{};
				root[0] = static_cast<wchar_t>(L'A' + i);
				root[1] = L':';
				root[2] = L'\\';
				PathScan::ScanInstance(std::filesystem::path{ root }, emit);
			}

			spdlog::info("[SaveGuard] {} candidate save directories", s_candidates.size());
			for (const auto& candidate : s_candidates) {
				spdlog::debug("[SaveGuard]   [{}] {}", candidate.why, PathToUtf8(candidate.dir));
			}
		}

		void DescribeCandidates()
		{
			std::string list;
			std::error_code ec;
			std::size_t     n = 0;
			for (const auto& candidate : s_candidates) {
				if (!list.empty()) {
					list += " | ";
				}
				list += PathToUtf8(candidate.dir);
				list += "[";
				list += candidate.why;
				list += std::filesystem::is_directory(candidate.dir, ec) ? ",dir]" : ",nodir]";
				++n;
			}
			s_status.candidatesTried = n;
			s_status.candidatesList  = list;
		}

		// The engine's own file name, or nothing.  kPreLoadGame in particular
		// hands us a raw pointer we have never validated.
		std::string SanitizeName(const char* a_name)
		{
			if (!a_name) {
				return {};
			}
			std::string name{ a_name };
			if (name.size() < 4 || name.size() > 240) {
				return {};
			}
			for (unsigned char c : name) {
				if (c < 0x20 || c >= 0x7F) {
					return {};
				}
			}
			// Strip any directory component the engine may have included.
			std::filesystem::path p{ name };
			return p.filename().string();
		}

		enum class Probe
		{
			kMissing,  // could not be opened at all
			kCorrupt,  // opened, but the header or the size is wrong
			kOk        // opened and looks like a real save
		};

		// v2.0.0: the system no longer provides backups.  This only records
		// where the real saves live, so the read-only structural check can find
		// them and Records::Prune can sweep the LEGACY 3SUBackup folder left by
		// older builds.  No directory is ever created here.
		void AdoptSaveDir(const std::filesystem::path& a_dir)
		{
			if (a_dir.empty()) {
				return;
			}
			s_status.saveDir   = a_dir;
			s_status.backupDir = a_dir / "3SUBackup";  // legacy path, swept not written
		}

		// Reads the file instead of asking whether it exists.  Under a mod
		// manager's virtual filesystem the two answers differ: the directory
		// listing can be empty while the file opens perfectly well.
		Probe Inspect(const std::filesystem::path& a_file,
			std::uintmax_t&               a_size,
			std::uint32_t&                a_version,
			std::string&                  a_note)
		{
			a_size    = 0;
			a_version = 0;

			if (a_file.empty()) {
				a_note = "没有可校验的文件";
				return Probe::kMissing;
			}

			std::ifstream in(a_file, std::ios::binary);
			if (!in.is_open()) {
				a_note = "文件不存在或无法打开";
				return Probe::kMissing;
			}

			in.seekg(0, std::ios::end);
			const auto end = in.tellg();
			if (end <= 0) {
				a_note = "文件为空";
				return Probe::kCorrupt;
			}
			a_size = static_cast<std::uintmax_t>(end);

			in.seekg(0, std::ios::beg);
			char header[SaveHeader::kProbeBytes]{};
			in.read(header, sizeof(header));
			const auto verdict = SaveHeader::Parse(header,
				static_cast<std::size_t>(in.gcount()), a_size);

			a_note = verdict.note;
			if (!verdict.ok) {
				return Probe::kCorrupt;
			}
			a_version = verdict.version;

			// v3.0.0: a 32-byte header check is NOT a save check.  Verify the
			// screenshot + LZ4 block geometry against the real file size, and
			// never tell the player "结构正常" when all we proved is that the
			// container is intact.  Plugin-table / load-order compatibility can
			// only be judged before launch, by 3SU Save Doctor.
			if (verdict.headerSize > 0) {
				const auto headerEnd = std::uintmax_t{ 17 } + verdict.headerSize;
				char       tail[10]{};
				if (headerEnd >= 10 && headerEnd < a_size) {
					in.clear();
					in.seekg(static_cast<std::streamoff>(headerEnd - 10), std::ios::beg);
					in.read(tail, sizeof(tail));
				}
				if (in.gcount() == static_cast<std::streamsize>(sizeof(tail))) {
					const auto shotW = SaveHeader::ReadU32(tail, 0);
					const auto shotH = SaveHeader::ReadU32(tail, 4);
					std::uint16_t comp = 0;
					std::memcpy(&comp, tail + 8, sizeof(comp));

					const auto bodyOffset = headerEnd + 4ull * shotW * shotH;
					char       lens[8]{};
					if (bodyOffset + 8ull <= a_size) {
						in.clear();
						in.seekg(static_cast<std::streamoff>(bodyOffset), std::ios::beg);
						in.read(lens, sizeof(lens));
					}
					if (in.gcount() == static_cast<std::streamsize>(sizeof(lens))) {
						const auto unc = SaveHeader::ReadU32(lens, 0);
						const auto cmp = SaveHeader::ReadU32(lens, 4);
						const auto cont = SaveHeader::CheckContainer(a_size,
							verdict.headerSize, shotW, shotH, comp, unc, cmp);
						a_note = cont.note;
						if (!cont.ok) {
							return Probe::kCorrupt;
						}
					}
				}
			}
			return Probe::kOk;
		}

		struct Located
		{
			std::filesystem::path path;
			std::string           via;
			std::uintmax_t        size{ 0 };
			Probe                 probe{ Probe::kMissing };
			std::string           note;
		};

		Located Locate(const std::string& a_name)
		{
			Located best;

			for (const auto& candidate : s_candidates) {
				const auto file = candidate.dir / a_name;
				std::uintmax_t size = 0;
				std::uint32_t  version = 0;
				std::string    note;

				const auto probe = Inspect(file, size, version, note);
				if (probe == Probe::kOk) {
					best.path  = file;
					best.via   = candidate.why;
					best.size  = size;
					best.probe = probe;
					best.note  = note;
					return best;
				}
				if (probe == Probe::kCorrupt && best.probe == Probe::kMissing) {
					// Keep it: a corrupt file we can open is still a much
					// better answer than "not found".
					best.path  = file;
					best.via   = candidate.why;
					best.size  = size;
					best.probe = probe;
					best.note  = note;
				}
			}

			return best;
		}

		// v2.0.0: CopyStream / WriteIndex / PruneBackups used to live here.
		// They are gone with the backup subsystem; no code path in this build
		// writes a second copy of a save any more.
	}

	namespace Guard
	{
		const Status& Last()
		{
			return s_status;
		}

		std::vector<std::filesystem::path> CandidateDirs()
		{
			std::vector<std::filesystem::path> out;
			out.reserve(s_candidates.size());
			for (const auto& c : s_candidates) {
				out.push_back(c.dir);
			}
			return out;
		}

		bool Initialize()
		{
			if (s_initialized) {
				return s_status.dirFound;
			}
			s_initialized = true;

			BuildCandidates();
			DescribeCandidates();

			if (s_candidates.empty()) {
				spdlog::warn("[3SU] no save directory candidates at all; SaveGuard disabled");
				s_status.dirFound = false;
				return false;
			}

			std::error_code ec;
			for (const auto& candidate : s_candidates) {
				if (std::filesystem::is_directory(candidate.dir, ec)) {
					s_status.saveDir = candidate.dir;
					break;
				}
			}
			if (s_status.saveDir.empty()) {
				// Nothing on disk looked like a save directory, but a virtual
				// filesystem can still serve files out of one, so keep the
				// first candidate as the place to probe and to put backups.
				s_status.saveDir = s_candidates.front().dir;
			}

		s_status.dirFound = true;
		// No backup directory is created.  The path is only remembered as the
		// location a PREVIOUS build may have left .ess.bak files in; the record
		// sweeper removes those at startup.
		s_status.backupDir = s_status.saveDir / "3SUBackup";

		spdlog::info("[3SU] read-only save check active: {} candidates, probing={} (no backups)",
			s_status.candidatesTried, PathToUtf8(s_status.saveDir));
			if (s_status.candidatesTried > 1) {
				spdlog::debug("[3SU] candidates: {}", s_status.candidatesList);
			}

			return true;
		}

		bool VerifyFile(const std::filesystem::path& a_file, std::string& a_note, std::uintmax_t& a_size)
		{
			std::uint32_t version = 0;
			const auto    probe   = Inspect(a_file, a_size, version, a_note);
			return probe == Probe::kOk;
		}

		void OnPreLoadGame(const char* a_fileName)
		{
			if (!Initialize()) {
				return;
			}

			const auto name = SanitizeName(a_fileName);
			s_status.verified = false;
			s_status.located  = false;

			if (name.empty()) {
				spdlog::debug("[3SU] pre-load: engine gave no usable file name, skipping verification");
				return;
			}

			const auto found = Locate(name);

			s_status.verified = true;
			s_status.located  = found.probe != Probe::kMissing;
			s_status.ok       = found.probe == Probe::kOk;
			s_status.note     = found.note;
			s_status.lastFile = found.path;
			s_status.lastSize = found.size;
			s_status.locatedVia = found.via;

			if (found.probe == Probe::kOk) {
				AdoptSaveDir(found.path.parent_path());
				// v3.0.0: say what was actually proven. A healthy container
				// does not mean the plugin table matches the load order.
				spdlog::info("[3SU] container OK (not a loadability verdict): {} [{}] ({})",
					name, found.via, found.note);
				return;
			}

			if (found.probe == Probe::kCorrupt) {
				AdoptSaveDir(found.path.parent_path());
				// v3.0.0: DO NOT promise that load-time repair will fix this.
				// If the engine rejects the file, no post-load callback ever
				// runs. Point the player at the pre-launch file repair tool.
				spdlog::warn("[3SU] save container flagged: {} [{}] ({}) - the engine may "
					"refuse to load it; run 3SU Save Doctor before launching the game",
					name, found.via, found.note);
				return;
			}

			// Not found is NOT corruption.  Reporting it as corruption is how a
			// checker teaches its user to ignore it, so it stays a warning and
			// names every path that was tried.
			spdlog::warn("[3SU] save not located: {} — tried {} paths: {}",
				name, s_status.candidatesTried, s_status.candidatesList);
			spdlog::warn("[3SU] 这不是损坏，只是没找到落点（MOD 管理器可能用了独立存档目录）。"
						 "在 3SU.ini 里把 sSaveDir 设为存档所在目录即可。");
		}

		void OnSaveGame(const char* a_fileName)
		{
			// v2.0.0: backups were removed entirely.  This entry point is kept
			// so the message handler needs no conditional, but it writes nothing:
			// no .ess.bak, no rotation, no backup index.  A save is not a thing
			// this plugin copies any more.
			(void)a_fileName;
			s_status.lastBackupOk   = false;
			s_status.lastBackupNote = "本版本不提供备份功能";
			s_status.backupsKept    = 0;
		}
	}
}
