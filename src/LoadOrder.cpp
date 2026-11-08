#include "PCH.h"

#include "LoadOrder.h"
#include "Vault.h"

#include <cstdlib>
#include <ostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace S3U::LoadOrder
{
	namespace
	{
		// One string, one record, one checksum.  A list of 200 plugins costs
		// about 5 KB and cannot half-update: it is stored and compared as a
		// single value.
		constexpr const char* kKey = "loadorder/list";

		Diff s_diff;

		std::string ToLower(std::string_view a_view)
		{
			std::string out{ a_view };
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		struct SavedMod
		{
			std::uint8_t index{ 0 };
			bool         light{ false };
		};

		void Parse(const std::string& a_blob, std::unordered_map<std::string, SavedMod>& a_out)
		{
			std::size_t start = 0;
			while (start < a_blob.size()) {
				auto end = a_blob.find('\n', start);
				if (end == std::string::npos) {
					end = a_blob.size();
				}
				const std::string_view line{ a_blob.data() + start, end - start };
				start = end + 1;

				const auto tab1 = line.find('\t');
				if (tab1 == std::string_view::npos) {
					continue;
				}
				const auto tab2 = line.find('\t', tab1 + 1);
				if (tab2 == std::string_view::npos) {
					continue;
				}

				SavedMod mod;
				mod.index = static_cast<std::uint8_t>(std::atoi(std::string{ line.substr(tab1 + 1, tab2 - tab1 - 1) }.c_str()));
				mod.light = line.substr(tab2 + 1) == "1";
				a_out.emplace(std::string{ line.substr(0, tab1) }, mod);
			}
		}
	}

	const Diff& Last()
	{
		return s_diff;
	}

	std::vector<Mod> Snapshot()
	{
		std::vector<Mod> out;

		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return out;
		}

		const auto add = [&out](RE::TESFile* a_file) {
			if (!a_file) {
				return;
			}
			Mod mod;
			mod.name  = ToLower(a_file->GetFilename());
			mod.index = a_file->GetCompileIndex();
			mod.light = a_file->IsLight();
			out.push_back(std::move(mod));
		};

		if (auto** mods = dh->GetLoadedMods()) {
			const auto count = dh->GetLoadedModCount();
			for (std::uint32_t i = 0; i < count; ++i) {
				add(mods[i]);
			}
		}

		// Light plugins live in their own array and would otherwise be invisible
		// to the comparison - and this load order is mostly ESLs.
		if (auto** light = dh->GetLoadedLightMods()) {
			const auto count = dh->GetLoadedLightModCount();
			for (std::uint32_t i = 0; i < count; ++i) {
				add(light[i]);
			}
		}

		return out;
	}

	void StoreToVault()
	{
		const auto mods = Snapshot();
		if (mods.empty()) {
			return;
		}

		std::string blob;
		blob.reserve(mods.size() * 32);
		for (const auto& mod : mods) {
			blob += mod.name;
			blob += '\t';
			blob += std::to_string(mod.index);
			blob += '\t';
			blob += mod.light ? '1' : '0';
			blob += '\n';
		}

		Vault::SetString(kKey, blob);
		Vault::SetInt("loadorder/count", static_cast<std::int64_t>(mods.size()));
	}

	void CompareWithVault()
	{
		s_diff = Diff{};

		std::string blob;
		if (!Vault::GetString(kKey, blob) || blob.empty()) {
			s_diff.note = "这个存档里没有加载顺序基线（旧版本插件写的档，或当时关了保险库）";
			return;
		}

		std::unordered_map<std::string, SavedMod> saved;
		Parse(blob, saved);
		if (saved.empty()) {
			s_diff.note = "基线存在但解析为空";
			return;
		}

		s_diff.haveBaseline = true;

		const auto current = Snapshot();
		std::unordered_set<std::string> present;
		present.reserve(current.size());

		for (const auto& mod : current) {
			present.insert(mod.name);

			auto it = saved.find(mod.name);
			if (it == saved.end()) {
				s_diff.added.push_back(mod.name);
				continue;
			}
			// A record that moved is still a record, but every raw FormID that
			// pointed at it now points at whatever took its place.  This is the
			// silent half of "reordering mods corrupts saves".
			if (it->second.index != mod.index && !mod.light) {
				s_diff.shifted.push_back(mod.name);
			}
		}

		for (const auto& [name, mod] : saved) {
			(void)mod;
			if (present.find(name) == present.end()) {
				s_diff.missing.push_back(name);
			}
		}

		std::sort(s_diff.missing.begin(), s_diff.missing.end());
		std::sort(s_diff.added.begin(), s_diff.added.end());
		std::sort(s_diff.shifted.begin(), s_diff.shifted.end());
	}

	void WriteReportSection(std::ostream& a_out)
	{
		a_out << "[加载顺序 / Load order]\n";

		if (!s_diff.haveBaseline) {
			a_out << "  无基线：" << s_diff.note << "\n\n";
			return;
		}

		a_out << "  缺失插件 : " << s_diff.missing.size()
			  << "   <-- 载入瞬间的快照；处理结果见「载入期自愈」段\n";
		a_out << "  新增插件 : " << s_diff.added.size()
			  << "   <-- 从未运行过，可能需要初始化\n";
		a_out << "  序号变动 : " << s_diff.shifted.size()
			  << "   <-- 引擎自身会重映射；第三方裸 FormID 会指向别处\n";

		const auto list = [&a_out](const char* a_title, const std::vector<std::string>& a_items) {
			if (a_items.empty()) {
				return;
			}
			a_out << "  " << a_title << ":\n";
			const auto limit = std::min<std::size_t>(a_items.size(), 30);
			for (std::size_t i = 0; i < limit; ++i) {
				a_out << "    - " << a_items[i] << "\n";
			}
			if (a_items.size() > limit) {
				a_out << "    ... 另有 " << (a_items.size() - limit) << " 个\n";
			}
		};

		list("缺失", s_diff.missing);
		list("新增", s_diff.added);
		list("序号变动", s_diff.shifted);
		a_out << "\n";
	}
}
