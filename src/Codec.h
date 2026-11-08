#pragma once

// -----------------------------------------------------------------------------
// Codec - the only place where bytes become values.
//
// Two rules hold everywhere in this file, because they are the two rules whose
// absence makes save formats fragile:
//
//   1. A Reader can never read out of bounds.  A truncated or hostile buffer
//      sets a sticky `ok` flag and yields zeroes; it never throws and never
//      walks off the end.  Deserialising a damaged file is therefore a normal,
//      recoverable outcome instead of an access violation.
//   2. Every byte a Writer emits has a defined order, so a file written on one
//      machine reads back identically on another.
//
// Little-endian throughout, which is what both x64 Windows and the engine use.
// -----------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace S3U::Codec
{
	inline std::uint32_t Crc32(const void* a_data, std::size_t a_len) noexcept
	{
		static const auto table = [] {
			std::array<std::uint32_t, 256> t{};
			for (std::uint32_t i = 0; i < 256; ++i) {
				std::uint32_t c = i;
				for (int k = 0; k < 8; ++k) {
					c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				}
				t[i] = c;
			}
			return t;
		}();

		std::uint32_t crc = 0xFFFFFFFFu;
		const auto*   p   = static_cast<const std::uint8_t*>(a_data);
		for (std::size_t i = 0; i < a_len; ++i) {
			crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
		}
		return crc ^ 0xFFFFFFFFu;
	}

	inline std::uint32_t Crc32(std::string_view a_view) noexcept
	{
		return Crc32(a_view.data(), a_view.size());
	}

	class Writer
	{
	public:
		void U8(std::uint8_t a_value) { m_buf.push_back(a_value); }

		void U16(std::uint16_t a_value)
		{
			m_buf.push_back(static_cast<std::uint8_t>(a_value & 0xFFu));
			m_buf.push_back(static_cast<std::uint8_t>((a_value >> 8) & 0xFFu));
		}

		void U32(std::uint32_t a_value)
		{
			for (int i = 0; i < 4; ++i) {
				m_buf.push_back(static_cast<std::uint8_t>((a_value >> (8 * i)) & 0xFFu));
			}
		}

		// Rewrites a little-endian u32 that was previously emitted at a known
		// offset.  Used for the header field whose value is only known after the
		// body has been written (a record count, a checksum).  The offset must
		// point at the start of an already-written u32; writing past the end is
		// a no-op that leaves the buffer unchanged.
		void OverwriteU32(std::size_t a_offset, std::uint32_t a_value)
		{
			if (a_offset + 4 > m_buf.size()) {
				return;
			}
			for (int i = 0; i < 4; ++i) {
				m_buf[a_offset + i] = static_cast<std::uint8_t>((a_value >> (8 * i)) & 0xFFu);
			}
		}

		void U64(std::uint64_t a_value)
		{
			for (int i = 0; i < 8; ++i) {
				m_buf.push_back(static_cast<std::uint8_t>((a_value >> (8 * i)) & 0xFFu));
			}
		}

		void F64(double a_value)
		{
			std::uint64_t bits = 0;
			std::memcpy(&bits, &a_value, sizeof(bits));
			U64(bits);
		}

		void Raw(const void* a_data, std::size_t a_len)
		{
			const auto* p = static_cast<const std::uint8_t*>(a_data);
			m_buf.insert(m_buf.end(), p, p + a_len);
		}

		// Length-prefixed UTF-8.  The 16-bit prefix caps a single string at
		// 65535 bytes, which is far beyond any key we store and keeps the
		// length from needing a varint.
		void Str(std::string_view a_value)
		{
			const auto len = static_cast<std::uint16_t>(
				std::min<std::size_t>(a_value.size(), 0xFFFFu));
			U16(len);
			Raw(a_value.data(), len);
		}

		[[nodiscard]] const std::vector<std::uint8_t>& Data() const noexcept { return m_buf; }
		[[nodiscard]] std::size_t                      Size() const noexcept { return m_buf.size(); }
		void                                           Reset() noexcept { m_buf.clear(); }

	private:
		std::vector<std::uint8_t> m_buf;
	};

	class Reader
	{
	public:
		Reader(const void* a_data, std::size_t a_len) noexcept :
			m_p(static_cast<const std::uint8_t*>(a_data)), m_n(a_len)
		{}

		[[nodiscard]] bool        Ok() const noexcept { return m_ok; }
		[[nodiscard]] std::size_t Remaining() const noexcept { return m_ok ? m_n - m_at : 0; }
		void                      Fail() noexcept { m_ok = false; }

		std::uint8_t U8() noexcept
		{
			if (!Need(1)) {
				return 0;
			}
			return m_p[m_at++];
		}

		std::uint16_t U16() noexcept
		{
			if (!Need(2)) {
				return 0;
			}
			const std::uint16_t v = static_cast<std::uint16_t>(m_p[m_at]) |
			                        (static_cast<std::uint16_t>(m_p[m_at + 1]) << 8);
			m_at += 2;
			return v;
		}

		std::uint32_t U32() noexcept
		{
			if (!Need(4)) {
				return 0;
			}
			std::uint32_t v = 0;
			for (int i = 0; i < 4; ++i) {
				v |= static_cast<std::uint32_t>(m_p[m_at + i]) << (8 * i);
			}
			m_at += 4;
			return v;
		}

		std::uint64_t U64() noexcept
		{
			if (!Need(8)) {
				return 0;
			}
			std::uint64_t v = 0;
			for (int i = 0; i < 8; ++i) {
				v |= static_cast<std::uint64_t>(m_p[m_at + i]) << (8 * i);
			}
			m_at += 8;
			return v;
		}

		double F64() noexcept
		{
			const std::uint64_t bits = U64();
			double              v    = 0.0;
			std::memcpy(&v, &bits, sizeof(v));
			return v;
		}

		bool Raw(void* a_dst, std::size_t a_len) noexcept
		{
			if (!Need(a_len)) {
				return false;
			}
			std::memcpy(a_dst, m_p + m_at, a_len);
			m_at += a_len;
			return true;
		}

		std::string Str() noexcept
		{
			const std::uint16_t len = U16();
			if (!Need(len)) {
				m_ok = false;
				return {};
			}
			std::string out(reinterpret_cast<const char*>(m_p + m_at), len);
			m_at += len;
			return out;
		}

		// Cursor access, used when a caller wants to checksum exactly the span
		// it has consumed.
		[[nodiscard]] const std::uint8_t* Base() const noexcept { return m_p; }
		[[nodiscard]] std::size_t         Cursor() const noexcept { return m_at; }
		void                              Seek(std::size_t a_at) noexcept
		{
			if (a_at > m_n) {
				m_ok = false;
				return;
			}
			m_at = a_at;
		}

	private:
		[[nodiscard]] bool Need(std::size_t a_len) noexcept
		{
			if (!m_ok || m_at + a_len > m_n) {
				m_ok = false;
				return false;
			}
			return true;
		}

		const std::uint8_t* m_p{ nullptr };
		std::size_t         m_n{ 0 };
		std::size_t         m_at{ 0 };
		bool                m_ok{ true };
	};
}
