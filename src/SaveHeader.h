#pragma once

// -----------------------------------------------------------------------------
// SaveHeader - pure-byte checks for Skyrim SE/AE .ess files.
//
// v3.0.0: this header used to claim "structure OK" after reading 32 bytes.
// That was a false positive waiting to happen, and it happened: a save with
// 189 plugin names against a 178-plugin load order was reported as
// "结构正常（4006 KB，版本 12）" while the engine refused to load it.
//
// Two real bugs were fixed here:
//
//   1. The magic is 13 bytes "TESV_SAVEGAME", NOT 14 bytes "TESV_SAVEGAMES".
//      Byte 13 is the low byte of the uint32 headerSize.  It only *looked*
//      like an 'S' because headerSize happened to be 83 (0x53).  Saves with
//      headerSize 86 (0x56 = 'V') were misdetected as pre-SE saves and had
//      their version read from offset 4 ("_SAV" = 1447121759).
//
//   2. "Container intact" is not "save loadable".  The container check now
//      verifies the LZ4 block length against the real file size, and says
//      explicitly that plugin-table/load-order compatibility is out of scope
//      for an in-game probe - that is 3SU Save Doctor's job, before launch.
//
// No engine, SKSE or filesystem dependencies: tools/header_test.cpp runs the
// exact same code.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <cstring>
#include <string>

namespace S3U::SaveHeader
{
	inline constexpr std::uintmax_t kMinSaveSize = 64ull * 1024ull;
	inline constexpr std::size_t    kProbeBytes  = 32;

	// 13 ASCII bytes, no terminator. Offset 13 is headerSize, NOT part of it.
	inline constexpr char        kMagic[]             = "TESV_SAVEGAME";
	inline constexpr std::size_t kMagicLen            = 13;
	inline constexpr std::size_t kHeaderSizeOffset    = 13;
	inline constexpr std::size_t kVersionOffset       = 17;
	inline constexpr std::size_t kLegacyVersionOffset = 4;
	inline constexpr std::uint32_t kMaxVersion        = 64;
	inline constexpr std::uint32_t kMaxHeaderSize     = 4096;

	struct Verdict
	{
		bool          ok{ false };
		std::uint32_t version{ 0 };
		std::uint32_t headerSize{ 0 };   // 0 when this is a legacy (pre-SE) save
		std::string   note;
	};

	inline std::uint32_t ReadU32(const char* a_bytes, std::size_t a_offset)
	{
		std::uint32_t value = 0;
		std::memcpy(&value, a_bytes + a_offset, sizeof(value));
		return value;
	}

	inline Verdict Parse(const char* a_header, std::size_t a_headerLen,
		std::uintmax_t a_fileSize)
	{
		Verdict v;

		if (a_fileSize < kMinSaveSize) {
			v.note = "文件过小（" + std::to_string(a_fileSize) + " 字节），极可能是写入中断";
			return v;
		}
		if (a_headerLen < kProbeBytes) {
			v.note = "文件头读取不完整";
			return v;
		}
		if (std::memcmp(a_header, "TESV", 4) != 0) {
			v.note = "文件头魔数不是 TESV，不是有效的天际存档";
			return v;
		}

		const auto headerSize = ReadU32(a_header, kHeaderSizeOffset);
		const auto seVersion  = ReadU32(a_header, kVersionOffset);

		// An SE/AE save has the 13-byte magic, a sane header size and a sane
		// version at 17.  Everything else falls back to the old LE layout.
		// Never decide by looking at byte 13 as if it were a character.
		const bool isSE = std::memcmp(a_header, kMagic, kMagicLen) == 0 &&
		                  headerSize >= 32 && headerSize <= kMaxHeaderSize &&
		                  seVersion > 0 && seVersion <= kMaxVersion;

		const auto offset  = isSE ? kVersionOffset : kLegacyVersionOffset;
		const auto version = isSE ? seVersion : ReadU32(a_header, offset);

		if (version == 0 || version > kMaxVersion) {
			v.note = "存档版本号异常（" + std::to_string(version) +
			         "，偏移 " + std::to_string(offset) + "）";
			return v;
		}

		v.ok         = true;
		v.version    = version;
		v.headerSize = isSE ? headerSize : 0;
		v.note       = "容器头正常（" + std::to_string(a_fileSize / 1024) +
		               " KB，版本 " + std::to_string(version) + "）";
		return v;
	}

	// -------------------------------------------------------------------------
	// Container check. Verifies the parts of an .ess that can be proven without
	// decompressing 11 MB on the game's main thread. It deliberately does NOT
	// claim the plugin table matches the current load order.
	// -------------------------------------------------------------------------
	struct Container
	{
		bool            ok{ false };
		std::uintmax_t  bodyOffset{ 0 };
		std::uint32_t   uncompressed{ 0 };
		std::uint32_t   compressed{ 0 };
		std::string     note;
	};

	inline Container CheckContainer(std::uintmax_t a_fileSize,
		std::uint32_t a_headerSize, std::uint32_t a_shotW, std::uint32_t a_shotH,
		std::uint16_t a_compression, std::uint32_t a_unc, std::uint32_t a_cmp)
	{
		Container c;
		c.uncompressed = a_unc;
		c.compressed   = a_cmp;

		if (a_shotW == 0 || a_shotW > 4096 || a_shotH == 0 || a_shotH > 4096) {
			c.note = "截图尺寸异常，容器可能已损坏";
			return c;
		}
		if (a_compression > 2) {
			c.note = "未知压缩类型（" + std::to_string(a_compression) + "）";
			return c;
		}
		if (a_headerSize < 32 || a_headerSize > kMaxHeaderSize) {
			c.note = "头部长度异常";
			return c;
		}
		if (a_unc < kMinSaveSize || a_cmp == 0) {
			c.note = "压缩块长度异常，存档可能写入中断";
			return c;
		}

		c.bodyOffset = 17ull + a_headerSize + 4ull * a_shotW * a_shotH;
		if (c.bodyOffset + 8ull > a_fileSize) {
			c.note = "截图区超出文件末尾，存档被截断";
			return c;
		}
		if (c.bodyOffset + 8ull + a_cmp != a_fileSize) {
			c.note = "压缩块长度与文件大小不符（" +
			         std::to_string(c.bodyOffset + 8ull + a_cmp) + " ≠ " +
			         std::to_string(a_fileSize) + "）";
			return c;
		}

		c.ok   = true;
		c.note = "容器完整（压缩块 " + std::to_string(a_cmp) + " 字节）；"
		         "插件表与加载顺序是否匹配需由 3SU Save Doctor 在读档前检查";
		return c;
	}
}
