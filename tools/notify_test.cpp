// Proves the notification wrapper without Skyrim: it compiles Notify.cpp,
// which has no engine dependency on purpose, and prints what the HUD would
// receive.  The assertion that matters is the last column of every line:
// once it exceeds the configured width, the game clips the text and the user
// never sees the rest.
#include "Notify.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	int s_failures = 0;

	std::size_t Check(std::string_view a_title, std::string_view a_text,
		std::size_t a_width, std::size_t a_limit)
	{
		const auto lines = S3U::Notify::Wrap(a_text, a_width);

		std::printf("[%s] width=%zu -> %zu line(s)\n", std::string{ a_title }.c_str(),
			a_width, lines.size());
		std::size_t widest = 0;
		for (const auto& line : lines) {
			const auto columns = S3U::Notify::Width(line);
			widest             = columns > widest ? columns : widest;
			std::printf("    %2zu | %s\n", columns, line.c_str());
		}

		if (widest > a_limit) {
			std::printf("    FAIL: a line is %zu columns wide, limit is %zu\n", widest, a_limit);
			++s_failures;
		}

		// Nothing may be lost: concatenating the lines back (without the
		// separators the wrapper adds) must contain every atom of the source.
		std::string joined;
		for (const auto& line : lines) {
			joined += line;
		}
		std::size_t atoms = 0;
		std::size_t pos   = 0;
		while (pos <= a_text.size()) {
			const auto sep = a_text.find(" \u00B7 ", pos);
			const auto end = (sep == std::string_view::npos) ? a_text.size() : sep;
			const auto atom = std::string{ a_text.substr(pos, end - pos) };
			if (!atom.empty() && joined.find(atom) == std::string::npos) {
				std::printf("    FAIL: atom lost: %s\n", atom.c_str());
				++s_failures;
			}
			++atoms;
			if (sep == std::string_view::npos) {
				break;
			}
			pos = sep + 4;
		}
		std::printf("    atoms=%zu\n", atoms);
		return widest;
	}
}

int main()
{
	using namespace S3U;

	struct Case
	{
		const char* title;
		const char* text;
	};

	// The four toasts this plugin actually pushes, verbatim.
	const Case cases[] = {
		{ "guard", "存档校验未通过 · 备份目录里有更早的档可退回" },
		{ "risky", "危险：缺失 3 个插件 · 2 个换序 · 档里留有它们的记录" },
		{ "vault", "保险库 tx 12 · 条目 7 · 引用 2/2 · 新增 3" },
		{ "audit1", "存档体检 · form 18.1万 · 脚本 6.6万" },
		{ "audit2", "残留 5.1% · 悬空 0 · 删除壳 33" },
	};

	const auto width = std::size_t{ 48 };
	for (const auto& item : cases) {
		Check(item.title, item.text, width, width);
	}

	// A single run with no separators must still be broken, not clipped.
	std::printf("\n[hard-wrap] a long run without separators\n");
	const auto broken = Notify::Wrap(
		"这一段故意不写任何分隔符用来验证字符级折行是否真的能把超长的一整句切开而不是交给游戏去裁掉"
		"后面半截让玩家以为插件只报了一半的信息", width);
	std::size_t widest = 0;
	for (const auto& line : broken) {
		const auto columns = Notify::Width(line);
		widest             = columns > widest ? columns : widest;
		std::printf("    %2zu | %s\n", columns, line.c_str());
	}
	if (widest > width) {
		std::printf("    FAIL: hard wrap exceeded %zu columns\n", width);
		++s_failures;
	}

	// Narrow HUD (scaled UI): every case must still respect the smaller width.
	std::printf("\n[scaled-hud] same cases at width=32\n");
	for (const auto& item : cases) {
		Check(item.title, item.text, 32, 32);
	}

	std::printf("\n%s (%d failure(s))\n", s_failures == 0 ? "PASS" : "FAILED", s_failures);
	return s_failures == 0 ? 0 : 1;
}
