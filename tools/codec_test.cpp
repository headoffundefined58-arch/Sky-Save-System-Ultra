// Standalone checks for the byte layer that every vault record sits on.
//
// This file deliberately includes nothing from the game: it is a plain console
// program, so it can be run without Skyrim, without SKSE, and without a save
// file.  What it proves is narrow but it is the narrow part that matters:
//
//   - the checksum is a real CRC-32 and not something that only agrees with
//     itself
//   - a value written always reads back bit-identical
//   - a *truncated* buffer can never read out of bounds, never throws, and
//     always reports failure instead of inventing data
//
// The third property is the whole reason the reader is written the way it is.
// The engine's own deserialiser trusts its input; ours does not.

#include "../src/Codec.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		if (!a_ok) {
			++g_failures;
			std::printf("  FAIL  %s\n", a_what);
		}
	}

	void TestCrc()
	{
		std::printf("[crc]\n");
		const std::string check = "123456789";
		const auto        crc   = S3U::Codec::Crc32(check);
		std::printf("  crc32(\"123456789\") = 0x%08X (expected 0xCBF43926)\n", crc);
		Check(crc == 0xCBF43926u, "CRC-32 standard check value");

		Check(S3U::Codec::Crc32("", 0) == 0x00000000u, "CRC of empty input is zero");
	}

	void TestRoundTrip()
	{
		std::printf("[round-trip]\n");

		S3U::Codec::Writer w;
		w.U8(0xA5);
		w.U16(0xBEEF);
		w.U32(0xDEADBEEFu);
		w.U64(0x0123456789ABCDEFull);
		w.F64(3.14159265358979);
		w.Str("player/ref");
		w.Str("");

		const auto& buffer = w.Data();

		S3U::Codec::Reader r(buffer.data(), buffer.size());
		Check(r.U8() == 0xA5, "u8");
		Check(r.U16() == 0xBEEF, "u16");
		Check(r.U32() == 0xDEADBEEFu, "u32");
		Check(r.U64() == 0x0123456789ABCDEFull, "u64");
		Check(r.F64() == 3.14159265358979, "f64");
		Check(r.Str() == "player/ref", "string");
		Check(r.Str().empty(), "empty string");
		Check(r.Ok(), "still ok after a full read");
		Check(r.Remaining() == 0, "cursor consumed the whole buffer");
	}

	// Every prefix of a valid buffer is fed to a reader.  Some prefixes must
	// fail (they end mid-value); none may crash, and none may claim success
	// while having produced a value out of thin air.
	void TestTruncation()
	{
		std::printf("[truncation]\n");

		S3U::Codec::Writer w;
		w.U32(0x11223344);
		w.Str("a fairly long key that straddles several boundaries");
		w.F64(2.718281828);

		const auto& full = w.Data();

		for (std::size_t len = 0; len <= full.size(); ++len) {
			S3U::Codec::Reader r(full.data(), len);
			r.U32();
			const auto text = r.Str();
			r.F64();

			if (len == full.size()) {
				Check(r.Ok(), "complete buffer reads cleanly");
				Check(!text.empty(), "complete buffer yields the string");
			} else {
				Check(!r.Ok(), "truncated buffer is reported as failed");
				Check(r.Remaining() == 0, "failed reader stops consuming");
			}
		}
		std::printf("  %zu prefixes checked\n", full.size() + 1);
	}

	// A string whose declared length runs past the end of the buffer must be
	// rejected, not trusted.  This is the classic "length field from disk"
	// over-read that turns a damaged file into an access violation.
	void TestHostileLength()
	{
		std::printf("[hostile-length]\n");

		std::vector<std::uint8_t> buffer;
		buffer.push_back(0xFF);  // length = 65535
		buffer.push_back(0xFF);
		buffer.push_back('x');

		S3U::Codec::Reader r(buffer.data(), buffer.size());
		const auto                text = r.Str();
		Check(!r.Ok(), "over-long length is rejected");
		Check(text.empty(), "nothing is produced from a rejected length");
	}

	void TestCrcDetectsDamage()
	{
		std::printf("[damage-detection]\n");

		S3U::Codec::Writer w;
		w.Str("trend/forms");
		w.U64(181031);

		auto buffer = w.Data();
		const auto good = S3U::Codec::Crc32(buffer.data(), buffer.size());

		buffer.back() ^= 0x01;
		const auto damaged = S3U::Codec::Crc32(buffer.data(), buffer.size());
		Check(good != damaged, "a single flipped bit changes the checksum");
		std::printf("  good=0x%08X damaged=0x%08X\n", good, damaged);
	}

	// The record-count overwrite is what keeps a vault header consistent with
	// the number of records in its body.  If it wrote the count up front, the
	// header could disagree with the body the moment the writer stops early;
	// patching it after the fact must land on exactly the right bytes.
	void TestOverwriteU32()
	{
		std::printf("[overwrite-u32]\n");

		S3U::Codec::Writer w;
		w.U32(0xDEADBEEFu);  // magic, never touched
		const auto countOffset = w.Size();
		w.U32(0xFFFFFFFFu);  // placeholder count
		w.Str("player/ref");
		w.Str("meta/saveCount");

		w.OverwriteU32(countOffset, 2);  // two records were written

		S3U::Codec::Reader r(w.Data().data(), w.Size());
		Check(r.U32() == 0xDEADBEEFu, "bytes before the patch are untouched");
		Check(r.U32() == 2u, "patched count reads back as 2");
		Check(r.Str() == "player/ref", "first record intact after patch");
		Check(r.Str() == "meta/saveCount", "second record intact after patch");
		Check(r.Ok() && r.Remaining() == 0, "patch did not shift the buffer");

		// An out-of-range offset must be a no-op, never a write out of bounds.
		S3U::Codec::Writer w2;
		w2.U32(7);
		const auto before = w2.Data();
		w2.OverwriteU32(100, 0xABCD);
		Check(w2.Data() == before, "out-of-range overwrite is a safe no-op");
	}
}

int main()
{
	std::printf("3SU codec checks\n\n");

	TestCrc();
	TestRoundTrip();
	TestTruncation();
	TestHostileLength();
	TestCrcDetectsDamage();
	TestOverwriteU32();

	std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
