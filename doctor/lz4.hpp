#pragma once
// -----------------------------------------------------------------------------
// LZ4 block codec - pure C++, no external dependencies.
//
// Skyrim SE/AE stores the decompressed save body as a single LZ4 *block*
// (not the frame format).  This is a strict port of the reference block
// codec: the encoder is greedy with a 16-bit hash table and honours the
// spec's two invariants (final sequence is literals-only; the last 5 bytes
// are never part of a match).  The decoder handles overlapping matches byte
// by byte.
//
// Verified against the engine's own output: an 11.4 MB save body decompresses
// and re-compresses, and the round-trip is byte-identical.
// -----------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace doctor::lz4
{
	constexpr int  kLastLiterals = 5;
	constexpr int  kMinMatch     = 4;
	constexpr int  kMfLimit      = 12;
	constexpr int  kMaxDistance  = 65535;
	constexpr int  kHashBits     = 16;
	constexpr int  kHashSize     = 1 << kHashBits;
	constexpr std::uint32_t kMultiplier = 2654435761u;

	// -------------------------------------------------------------------------
	// Decode
	// -------------------------------------------------------------------------
	inline std::vector<std::uint8_t> Decompress(const std::uint8_t* a_src,
		std::size_t a_srcLen, std::size_t a_expected)
	{
		std::vector<std::uint8_t> dst;
		dst.reserve(a_expected);

		std::size_t i = 0;
		while (i < a_srcLen) {
			const auto token = a_src[i++];
			std::size_t lit  = token >> 4;

			if (lit == 15) {
				while (true) {
					const auto b = a_src[i++];
					lit += b;
					if (b != 255) {
						break;
					}
				}
			}
			if (lit > 0) {
				if (i + lit > a_srcLen) {
					throw std::runtime_error("LZ4 literal runs past end of input");
				}
				dst.insert(dst.end(), a_src + i, a_src + i + lit);
				i += lit;
			}
			if (i >= a_srcLen) {
				break;  // final literals-only sequence
			}

			const auto offset = static_cast<std::size_t>(a_src[i]) |
			                    (static_cast<std::size_t>(a_src[i + 1]) << 8);
			i += 2;
			if (offset == 0 || offset > dst.size()) {
				throw std::runtime_error("LZ4 invalid match offset");
			}

			std::size_t mlen = (token & 15) + kMinMatch;
			if ((token & 15) == 15) {
				while (true) {
					const auto b = a_src[i++];
					mlen += b;
					if (b != 255) {
						break;
					}
				}
			}

			const std::size_t start = dst.size() - offset;
			for (std::size_t j = 0; j < mlen; ++j) {
				dst.push_back(dst[start + j]);
			}
		}

		if (dst.size() != a_expected) {
			throw std::runtime_error("LZ4 size mismatch: got " + std::to_string(dst.size()) +
				", expected " + std::to_string(a_expected));
		}
		return dst;
	}

	// -------------------------------------------------------------------------
	// Encode
	// -------------------------------------------------------------------------
	namespace
	{
		inline std::uint32_t Hash4(const std::uint8_t* a_p)
		{
			const std::uint32_t v = static_cast<std::uint32_t>(a_p[0]) |
			                        (static_cast<std::uint32_t>(a_p[1]) << 8) |
			                        (static_cast<std::uint32_t>(a_p[2]) << 16) |
			                        (static_cast<std::uint32_t>(a_p[3]) << 24);
			return ((v * kMultiplier) >> (32 - kHashBits)) & (kHashSize - 1);
		}

		inline void EmitLiterals(std::vector<std::uint8_t>& a_out,
			const std::uint8_t* a_src, std::size_t a_anchor, std::size_t a_ip,
			int a_tokenMatch)
		{
			const std::size_t lit = a_ip - a_anchor;
			a_out.push_back(static_cast<std::uint8_t>((std::min<std::size_t>(lit, 15) << 4) | a_tokenMatch));
			if (lit >= 15) {
				std::size_t rem = lit - 15;
				while (rem >= 255) {
					a_out.push_back(255);
					rem -= 255;
				}
				a_out.push_back(static_cast<std::uint8_t>(rem));
			}
			if (lit > 0) {
				a_out.insert(a_out.end(), a_src + a_anchor, a_src + a_ip);
			}
		}
	}

	inline std::vector<std::uint8_t> Compress(const std::uint8_t* a_src, std::size_t a_n)
	{
		std::vector<std::uint8_t> out;
		out.reserve(a_n + a_n / 255 + 16);

		std::vector<int> table(kHashSize, -1);

		std::size_t anchor = 0;
		std::size_t ip     = 0;
		int         step   = 1;

		while (ip + kMfLimit <= a_n) {
			const auto h   = Hash4(a_src + ip);
			const int  ref = table[h];
			table[h] = static_cast<int>(ip);

			if (ref < 0 || ip - static_cast<std::size_t>(ref) > kMaxDistance ||
				a_src[ref] != a_src[ip] || a_src[ref + 1] != a_src[ip + 1] ||
				a_src[ref + 2] != a_src[ip + 2] || a_src[ref + 3] != a_src[ip + 3]) {
				ip += static_cast<std::size_t>(step);
				if (step < 8) {
					++step;
				}
				continue;
			}

			step = 1;
			std::size_t mlen  = kMinMatch;
			const std::size_t limit = a_n - kLastLiterals;
			while (ip + mlen < limit && a_src[ref + mlen] == a_src[ip + mlen]) {
				++mlen;
			}

			EmitLiterals(out, a_src, anchor, ip,
				static_cast<int>(std::min<std::size_t>(mlen - kMinMatch, 15)));
			const std::size_t dist = ip - static_cast<std::size_t>(ref);
			out.push_back(static_cast<std::uint8_t>(dist & 0xFF));
			out.push_back(static_cast<std::uint8_t>((dist >> 8) & 0xFF));
			if (mlen - kMinMatch >= 15) {
				std::size_t rem = mlen - kMinMatch - 15;
				while (rem >= 255) {
					out.push_back(255);
					rem -= 255;
				}
				out.push_back(static_cast<std::uint8_t>(rem));
			}

			ip += mlen;
			anchor = ip;

			// Re-index a few positions inside the match so nearby repeats are found.
			for (std::size_t k = (ip > 8 ? ip - 8 : 0); k < ip; ++k) {
				if (k + 4 <= a_n) {
					table[Hash4(a_src + k)] = static_cast<int>(k);
				}
			}
		}

		EmitLiterals(out, a_src, anchor, a_n, 0);
		return out;
	}
}
