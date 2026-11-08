#pragma once
// -----------------------------------------------------------------------------
// ess.hpp - Skyrim SE/AE .ess reader + in-place plugin-table substitution.
//
// Layout (verified against UESP "Skyrim Mod:Save File Format" and real saves,
// and cross-checked against ReSaver's FileLocationTable semantics):
//
//   char[13]   "TESV_SAVEGAME"
//   uint32     headerSize          (bytes from offset 17 to end of header)
//   uint32     version             (12 on SE/AE)
//   uint32     saveNumber
//   ...        player name/level/time/FILETIME ...
//   uint32     shotWidth           (at headerEnd - 10)
//   uint32     shotHeight          (at headerEnd - 6)
//   uint16     compressionType     (0 none, 1 zlib, 2 LZ4 block)
//   uint8[4*shotWidth*shotHeight]  screenshot (BGRA)
//   uint32     uncompressedLen
//   uint32     compressedLen
//   uint8[compressedLen]           compressed body
//
// Decompressed body (offsets below are FILE-ABSOLUTE, per ReSaver: they must
// be reduced by `bodyOffset` to index into the decompressed buffer):
//
//   uint8      formVersion
//   uint32     pluginInfoSize
//   uint8      fullPluginCount
//   wstring[full]     (uint16 len + ascii)
//   uint16     lightPluginCount
//   wstring[light]
//   FileLocationTable   10 uint32 + 15 unused
//   globalDataTable1 / 2 / changeForms / globalDataTable3 / formIDArray ...
//
// The repair (in-place substitution) keeps the plugin-table byte length
// EXACTLY constant, so every FileLocationTable offset and every byte after
// the tables stays valid.  Only the names of plugins that no longer resolve
// are replaced with same-length names that DO resolve; the engine then drops
// the orphaned changeforms automatically on load.
//
// No engine, no SKSE, no third-party packages.
// -----------------------------------------------------------------------------
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "lz4.hpp"

namespace doctor
{
	inline constexpr std::size_t kMagicLen = 13;

	inline std::uint32_t ReadU32(const std::uint8_t* a_p)
	{
		std::uint32_t v = 0;
		std::memcpy(&v, a_p, sizeof(v));
		return v;
	}

	struct EssFile
	{
		std::string path;
		std::vector<std::uint8_t> raw;
		std::size_t headerEnd{ 0 };
		std::uint32_t shotW{ 0 };
		std::uint32_t shotH{ 0 };
		std::uint16_t compression{ 0 };
		std::size_t bodyOffset{ 0 };       // file-absolute offset of uncompressedLen
		std::uint32_t uncompressed{ 0 };
		std::uint32_t compressed{ 0 };
		std::vector<std::uint8_t> body;    // decompressed body

		std::uint8_t formVersion{ 0 };
		std::uint32_t pluginInfoSize{ 0 };
		std::uint8_t fullCount{ 0 };
		std::uint16_t lightCount{ 0 };
		std::size_t fullStart{ 0 };        // offset of first full-plugin wstring length
		std::size_t lightStart{ 0 };       // offset of first light-plugin wstring length
		std::size_t tableEnd{ 0 };         // offset of FileLocationTable
		std::vector<std::string> fullPlugins;
		std::vector<std::string> lightPlugins;
		std::vector<std::uint32_t> flt;    // 25 uint32

		std::vector<std::string> Plugins() const
		{
			auto v = fullPlugins;
			v.insert(v.end(), lightPlugins.begin(), lightPlugins.end());
			return v;
		}
	};

	// -------------------------------------------------------------------------
	// Parse a full .ess file.
	// -------------------------------------------------------------------------
	inline EssFile Load(const std::filesystem::path& a_path)
	{
		EssFile f;
		f.path = a_path.string();

		{
			std::ifstream in(a_path, std::ios::binary);
			if (!in.is_open()) {
				throw std::runtime_error("cannot open " + f.path);
			}
			in.seekg(0, std::ios::end);
			const auto end = in.tellg();
			in.seekg(0, std::ios::beg);
			f.raw.resize(static_cast<std::size_t>(end));
			in.read(reinterpret_cast<char*>(f.raw.data()), static_cast<std::streamsize>(end));
		}

		if (f.raw.size() < 13 || std::memcmp(f.raw.data(), "TESV_SAVEGAME", 13) != 0) {
			throw std::runtime_error("not a Skyrim SE save (bad magic)");
		}

		const auto headerSize = ReadU32(f.raw.data() + 13);
		f.headerEnd = 17 + headerSize;
		if (f.headerEnd + 12 > f.raw.size()) {
			throw std::runtime_error("truncated header");
		}

		f.shotW = ReadU32(f.raw.data() + f.headerEnd - 10);
		f.shotH = ReadU32(f.raw.data() + f.headerEnd - 6);
		std::memcpy(&f.compression, f.raw.data() + f.headerEnd - 2, sizeof(f.compression));

		f.bodyOffset = f.headerEnd + static_cast<std::size_t>(4) * f.shotW * f.shotH;
		if (f.bodyOffset + 8 > f.raw.size()) {
			throw std::runtime_error("screenshot extends past end of file");
		}

		f.uncompressed = ReadU32(f.raw.data() + f.bodyOffset);
		f.compressed   = ReadU32(f.raw.data() + f.bodyOffset + 4);
		const auto packed = f.raw.data() + f.bodyOffset + 8;

		if (f.bodyOffset + 8 + f.compressed != f.raw.size()) {
			throw std::runtime_error("compressed block length does not match file size");
		}

		if (f.compression == 2) {
			f.body = lz4::Decompress(packed, f.compressed, f.uncompressed);
		} else if (f.compression == 0) {
			f.body.assign(packed, packed + f.compressed);
		} else {
			throw std::runtime_error("unsupported compressionType " + std::to_string(f.compression));
		}

		// --- parse the decompressed body ---
		const auto& b = f.body;
		std::size_t pos = 0;
		if (b.empty()) {
			throw std::runtime_error("empty decompressed body");
		}
		f.formVersion = b[pos++];
		if (pos + 4 > b.size()) {
			throw std::runtime_error("truncated pluginInfoSize");
		}
		f.pluginInfoSize = ReadU32(b.data() + pos);
		pos += 4;

		f.fullCount = b[pos++];
		f.fullStart = pos;
		f.fullPlugins.clear();
		for (std::uint8_t i = 0; i < f.fullCount; ++i) {
			if (pos + 2 > b.size()) {
				throw std::runtime_error("truncated full plugin table");
			}
			const auto ln = static_cast<std::size_t>(b[pos]) | (static_cast<std::size_t>(b[pos + 1]) << 8);
			pos += 2;
			if (ln > 512 || pos + ln > b.size()) {
				throw std::runtime_error("implausible full plugin name length");
			}
			f.fullPlugins.emplace_back(reinterpret_cast<const char*>(b.data() + pos), ln);
			pos += ln;
		}

		f.lightCount = static_cast<std::uint16_t>(b[pos] | (b[pos + 1] << 8));
		pos += 2;
		f.lightStart = pos;
		f.lightPlugins.clear();
		for (std::uint16_t i = 0; i < f.lightCount; ++i) {
			if (pos + 2 > b.size()) {
				throw std::runtime_error("truncated light plugin table");
			}
			const auto ln = static_cast<std::size_t>(b[pos]) | (static_cast<std::size_t>(b[pos + 1]) << 8);
			pos += 2;
			if (ln > 512 || pos + ln > b.size()) {
				throw std::runtime_error("implausible light plugin name length");
			}
			f.lightPlugins.emplace_back(reinterpret_cast<const char*>(b.data() + pos), ln);
			pos += ln;
		}

		f.tableEnd = pos;
		if (f.pluginInfoSize != f.tableEnd - 5) {
			throw std::runtime_error("pluginInfoSize mismatch");
		}

		f.flt.resize(25);
		for (auto& v : f.flt) {
			if (pos + 4 > b.size()) {
				throw std::runtime_error("truncated FileLocationTable");
			}
			v = ReadU32(b.data() + pos);
			pos += 4;
		}

		return f;
	}

	// -------------------------------------------------------------------------
	// Rebuild the two plugin tables with substitutes, keeping byte length EXACT.
	// Returns the new body.  Everything before fullStart-1 and after tableEnd
	// is copied verbatim.
	// -------------------------------------------------------------------------
	struct Substitution
	{
		std::string from;
		std::string to;
	};

	inline std::string ToLower(std::string a_s)
	{
		for (auto& c : a_s) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}
		return a_s;
	}

	namespace
	{
		struct Entry
		{
			std::size_t start;  // includes u16 length
			std::size_t end;
			std::string name;
		};

		std::vector<Entry> Spans(const std::vector<std::uint8_t>& a_body,
			std::size_t a_start, std::size_t a_count)
		{
			std::vector<Entry> spans;
			std::size_t pos = a_start;
			for (std::size_t i = 0; i < a_count; ++i) {
				const auto ln = static_cast<std::size_t>(a_body[pos]) |
				                (static_cast<std::size_t>(a_body[pos + 1]) << 8);
				spans.push_back({ pos, pos + 2 + ln,
					std::string(reinterpret_cast<const char*>(a_body.data() + pos + 2), ln) });
				pos += 2 + ln;
			}
			return spans;
		}

		// DP: pick one substitute name per hole so total byte length == target.
		// Each hole has its own pool (keeps full/light classification intact).
		std::vector<std::string> PickNames(const std::vector<std::vector<std::string>>& a_pools,
			std::size_t a_target)
		{
			using Combo = std::vector<std::string>;
			std::map<std::size_t, Combo> dp;
			dp[0] = {};

			std::size_t idx = 0;
			for (const auto& pool : a_pools) {
				if (pool.empty()) {
					return {};
				}
				std::vector<std::string> rotated = pool;
				std::rotate(rotated.begin(), rotated.begin() + (idx % rotated.size()), rotated.end());

				std::map<std::size_t, Combo> next;
				for (const auto& [sum, combo] : dp) {
					for (const auto& name : rotated) {
						const auto t = sum + name.size();
						if (t <= a_target && next.find(t) == next.end()) {
							next[t] = combo;
							next[t].push_back(name);
						}
					}
				}
				dp = std::move(next);
				if (dp.empty()) {
					return {};
				}
				++idx;
			}
			auto it = dp.find(a_target);
			return it == dp.end() ? std::vector<std::string>{} : it->second;
		}
	}

	inline std::vector<std::uint8_t> Substitute(EssFile& a_save,
		const std::vector<std::string>& a_current,
		const std::vector<std::string>& a_missing,
		std::vector<Substitution>& a_outLog)
	{
		auto& b = a_save.body;

		std::vector<std::string> missingLower;
		for (const auto& m : a_missing) {
			missingLower.push_back(ToLower(m));
		}
		auto isMissing = [&](const std::string& n) {
			const auto ln = ToLower(n);
			for (const auto& m : missingLower) {
				if (ln == m) {
					return true;
				}
			}
			return false;
		};

		const auto fullSpans  = Spans(b, a_save.fullStart, a_save.fullCount);
		const auto lightSpans = Spans(b, a_save.lightStart, a_save.lightCount);

		std::vector<Entry> holes;
		for (const auto& s : fullSpans) {
			if (isMissing(s.name)) {
				holes.push_back(s);
			}
		}
		for (const auto& s : lightSpans) {
			if (isMissing(s.name)) {
				holes.push_back(s);
			}
		}
		if (holes.size() != a_missing.size()) {
			throw std::runtime_error("could not locate every missing plugin entry");
		}

		std::size_t oldTotal = 0;
		for (const auto& h : holes) {
			oldTotal += h.name.size();
		}

		// Build the current-name set and per-class pools.
		auto curSet = [&] {
			std::vector<std::string> v;
			for (const auto& c : a_current) {
				v.push_back(ToLower(c));
			}
			return v;
		}();
		auto fullSet = [&] {
			std::vector<std::string> v;
			for (const auto& c : a_save.fullPlugins) {
				v.push_back(ToLower(c));
			}
			return v;
		}();
		auto lightSet = [&] {
			std::vector<std::string> v;
			for (const auto& c : a_save.lightPlugins) {
				v.push_back(ToLower(c));
			}
			return v;
		}();

		std::vector<std::string> fullPool;
		for (const auto& c : a_current) {
			const auto lc = ToLower(c);
			if (std::find(curSet.begin(), curSet.end(), lc) != curSet.end() &&
				std::find(fullSet.begin(), fullSet.end(), lc) != fullSet.end()) {
				fullPool.push_back(c);
			}
		}
		std::vector<std::string> lightPool;
		for (const auto& c : a_current) {
			const auto lc = ToLower(c);
			if (std::find(curSet.begin(), curSet.end(), lc) != curSet.end() &&
				std::find(lightSet.begin(), lightSet.end(), lc) != lightSet.end()) {
				lightPool.push_back(c);
			}
		}

		std::vector<std::vector<std::string>> pools;
		for (const auto& h : holes) {
			const auto lh = ToLower(h.name);
			const bool inFull = std::find(fullSet.begin(), fullSet.end(), lh) != fullSet.end();
			pools.push_back(inFull ? fullPool : lightPool);
		}

		const auto replacements = PickNames(pools, oldTotal);
		if (replacements.empty()) {
			throw std::runtime_error("no substitute combination sums to the required length");
		}

		// Rebuild the table byte range in place.
		std::vector<std::uint8_t> table;
		table.push_back(b[a_save.fullStart - 1]);  // full plugin count
		std::size_t holeIdx = 0;
		a_outLog.clear();
		for (const auto& [s, e, name] : fullSpans) {
			if (isMissing(name)) {
				const auto& r = replacements[holeIdx++];
				table.push_back(static_cast<std::uint8_t>(r.size() & 0xFF));
				table.push_back(static_cast<std::uint8_t>((r.size() >> 8) & 0xFF));
				table.insert(table.end(), r.begin(), r.end());
				a_outLog.push_back({ name, r });
			} else {
				table.insert(table.end(), b.begin() + s, b.begin() + e);
			}
		}
		table.push_back(b[a_save.lightStart - 2]);
		table.push_back(b[a_save.lightStart - 1]);
		for (const auto& [s, e, name] : lightSpans) {
			if (isMissing(name)) {
				const auto& r = replacements[holeIdx++];
				table.push_back(static_cast<std::uint8_t>(r.size() & 0xFF));
				table.push_back(static_cast<std::uint8_t>((r.size() >> 8) & 0xFF));
				table.insert(table.end(), r.begin(), r.end());
				a_outLog.push_back({ name, r });
			} else {
				table.insert(table.end(), b.begin() + s, b.begin() + e);
			}
		}

		const std::size_t origLen = a_save.tableEnd - (a_save.fullStart - 1);
		if (table.size() != origLen) {
			throw std::runtime_error("table length drift");
		}

		std::vector<std::uint8_t> newBody;
		newBody.reserve(b.size());
		newBody.insert(newBody.end(), b.begin(), b.begin() + (a_save.fullStart - 1));
		newBody.insert(newBody.end(), table.begin(), table.end());
		newBody.insert(newBody.end(), b.begin() + a_save.tableEnd, b.end());

		if (newBody.size() != b.size()) {
			throw std::runtime_error("body length drift");
		}
		return newBody;
	}

	// -------------------------------------------------------------------------
	// Write a new .ess using the source header + screenshot verbatim and a
	// freshly packed body.  The body length must be unchanged (FLT stays valid).
	// -------------------------------------------------------------------------
	inline void BuildWithBody(const EssFile& a_src,
		const std::vector<std::uint8_t>& a_newBody,
		const std::filesystem::path& a_target)
	{
		if (a_newBody.size() != a_src.uncompressed) {
			throw std::runtime_error("body length changed; FLT offsets would be stale");
		}

		std::vector<std::uint8_t> packed;
		if (a_src.compression == 2) {
			packed = lz4::Compress(a_newBody.data(), a_newBody.size());
			// self round-trip check
			auto rt = lz4::Decompress(packed.data(), packed.size(), a_newBody.size());
			if (rt != a_newBody) {
				throw std::runtime_error("LZ4 encoder self round-trip failed");
			}
		} else {
			packed = a_newBody;
		}

		std::ofstream out(a_target, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) {
			throw std::runtime_error("cannot write " + a_target.string());
		}
		out.write(reinterpret_cast<const char*>(a_src.raw.data()),
			static_cast<std::streamsize>(a_src.bodyOffset));
		const std::uint32_t unc = static_cast<std::uint32_t>(a_newBody.size());
		const std::uint32_t cmp = static_cast<std::uint32_t>(packed.size());
		out.write(reinterpret_cast<const char*>(&unc), sizeof(unc));
		out.write(reinterpret_cast<const char*>(&cmp), sizeof(cmp));
		out.write(reinterpret_cast<const char*>(packed.data()),
			static_cast<std::streamsize>(packed.size()));
		out.flush();
	}
}
