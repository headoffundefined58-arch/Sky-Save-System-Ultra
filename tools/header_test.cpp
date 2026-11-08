// Regression test for SaveHeader::Parse, the byte-level .ess header check.
//
// The sample below is the real first 32 bytes of a Skyrim SE/AE save
// (Save5_..._WindhelmPalaceoftheKings...ess).  It exists because the previous
// implementation only checked "TESV" and read offset 4 as the version, which
// is "_SAV" = 1447121759, so every healthy save was reported as corrupt.
// That false positive also made SaveGuard delete freshly written backups.

#include "SaveHeader.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		std::printf("  [%s] %s\n", a_ok ? "PASS" : "FAIL", a_what);
		if (!a_ok) {
			++g_failures;
		}
	}

	// "TESV_SAVEGAMES" + 3 zero + version(12) + saveNumber(5) + nameLen(6) ...
	constexpr unsigned char kReal[32] = {
		0x54, 0x45, 0x53, 0x56, 0x5F, 0x53, 0x41, 0x56,
		0x45, 0x47, 0x41, 0x4D, 0x45, 0x53, 0x00, 0x00,
		0x00, 0x0C, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
		0x00, 0x06, 0x00, 0xE5, 0x9B, 0x9A, 0xE7, 0x8A
	};

	constexpr std::uintmax_t kRealSize = 4209747ull;

	S3U::SaveHeader::Verdict Parse(const unsigned char* a_bytes,
		std::size_t a_len, std::uintmax_t a_size = kRealSize)
	{
		return S3U::SaveHeader::Parse(reinterpret_cast<const char*>(a_bytes),
			a_len, a_size);
	}

	void TestRealHeader()
	{
		std::printf("[real SE/AE header]\n");

		const auto v = Parse(kReal, sizeof(kReal));
		Check(v.ok, "healthy 4 MB SE save is accepted");
		Check(v.version == 12, "version read as 12 from offset 17");
		Check(v.note.find("版本 12") != std::string::npos, "note reports version 12");

		Check(std::memcmp(kReal, S3U::SaveHeader::kMagic,
			S3U::SaveHeader::kMagicLen) == 0, "sample really is TESV_SAVEGAMES");
		Check(S3U::SaveHeader::ReadU32(reinterpret_cast<const char*>(kReal), 21) == 5,
			"offset 21 is the save number (5), confirming the layout");
		Check(S3U::SaveHeader::ReadU32(reinterpret_cast<const char*>(kReal), 4) == 1447121759u,
			"the old offset 4 really is the bogus 1447121759");
	}

	void TestCorruption()
	{
		std::printf("[reject damaged headers]\n");

		unsigned char buf[32];
		std::memcpy(buf, kReal, sizeof(buf));

		buf[0] = 'X';
		Check(!Parse(buf, sizeof(buf)).ok, "bad magic rejected");

		std::memcpy(buf, kReal, sizeof(buf));
		buf[S3U::SaveHeader::kVersionOffset] = 0;
		Check(!Parse(buf, sizeof(buf)).ok, "version 0 rejected");

		std::memcpy(buf, kReal, sizeof(buf));
		buf[S3U::SaveHeader::kVersionOffset] = 0xFF;
		buf[S3U::SaveHeader::kVersionOffset + 1] = 0xFF;
		Check(!Parse(buf, sizeof(buf)).ok, "implausible version rejected");

		Check(!Parse(kReal, sizeof(kReal), 1024).ok, "truncated file rejected by size");
	}

	void TestEveryPrefix()
	{
		std::printf("[every truncated prefix]\n");

		bool allRejected = true;
		for (std::size_t n = 0; n < S3U::SaveHeader::kProbeBytes; ++n) {
			if (Parse(kReal, n).ok) {
				allRejected = false;
				std::printf("    prefix of %zu bytes was wrongly accepted\n", n);
			}
		}
		Check(allRejected, "all 32 short reads rejected");
		Check(Parse(kReal, S3U::SaveHeader::kProbeBytes).ok,
			"exactly 32 bytes is enough");
	}

	void TestLegacyFallback()
	{
		std::printf("[legacy four-byte magic]\n");

		unsigned char buf[32]{};
		std::memcpy(buf, "TESV", 4);
		buf[4] = 9;  // pre-SE version at offset 4
		const auto v = Parse(buf, sizeof(buf));
		Check(v.ok && v.version == 9, "non-SE save still parses via offset 4");
	}

	// v3.0.0 regression: byte 13 is headerSize, not the 14th magic character.
	void TestHeaderSizeIsNotMagic()
	{
		std::printf("[headerSize low byte is not part of the magic]\n");

		unsigned char buf[32];
		std::memcpy(buf, kReal, sizeof(buf));

		buf[13] = 86;  // 'V' - the old 14-byte "TESV_SAVEGAMES" check failed here
		const auto v = Parse(buf, sizeof(buf));
		Check(v.ok && v.version == 12,
			"headerSize=86 is still parsed as an SE save with version 12");
		Check(v.headerSize == 86, "headerSize is exposed for container probing");

		buf[13] = 83;  // 'S' - the value that used to make the bug invisible
		const auto v2 = Parse(buf, sizeof(buf));
		Check(v2.ok && v2.headerSize == 83, "headerSize=83 still parses");

		buf[13] = 0;
		Check(!Parse(buf, sizeof(buf)).ok || Parse(buf, sizeof(buf)).version == 12,
			"implausible headerSize falls back to a checked legacy verdict");
	}

	// v3.0.0: "container intact" is a real, testable claim - and it is not a
	// claim about loadability.
	void TestContainer()
	{
		std::printf("[container geometry]\n");
		using S3U::SaveHeader::CheckContainer;

		// Real numbers from a 4,102,209 byte SE save:
		//   bodyOffset = 17 + 83 + 4*320*192 = 245860
		//   245860 + 8 + 3856341 = 4102209
		const auto c = CheckContainer(4102209ull, 83, 320, 192, 2, 11412350, 3856341);
		Check(c.ok, "real save container accepted");
		Check(c.bodyOffset == 245860ull, "bodyOffset from headerSize + screenshot");
		Check(c.note.find("Save Doctor") != std::string::npos,
			"note states plugin-table compatibility is out of scope");
		Check(c.note.find("结构正常") == std::string::npos,
			"never claims 'structure normal' again");

		Check(!CheckContainer(4102208ull, 83, 320, 192, 2, 11412350, 3856341).ok,
			"file one byte short is rejected");
		Check(!CheckContainer(4102209ull, 83, 320, 192, 2, 11412350, 3856340).ok,
			"truncated compressed block is rejected");
		Check(!CheckContainer(4102209ull, 83, 0, 192, 2, 11412350, 3856341).ok,
			"zero screenshot width rejected");
		Check(!CheckContainer(4102209ull, 83, 320, 192, 3, 11412350, 3856341).ok,
			"unknown compression type rejected");
		Check(!CheckContainer(4102209ull, 83, 320, 192, 2, 1024, 3856341).ok,
			"implausible uncompressed length rejected");
	}
}

int main()
{
	std::printf("=== SaveHeaderTest ===\n");
	TestRealHeader();
	TestCorruption();
	TestEveryPrefix();
	TestLegacyFallback();
	TestHeaderSizeIsNotMagic();
	TestContainer();
	std::printf("=== %s (%d failures) ===\n",
		g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
	return g_failures == 0 ? 0 : 1;
}
