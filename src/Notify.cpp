// Deliberately free of CommonLibSSE and of the plugin's own settings: this is
// pure string logic, so tools/notify_test.cpp can compile it alone and prove
// the wrapping instead of asserting it.
#include "Notify.h"

#include <cstdint>

namespace S3U
{
	namespace
	{
		// UTF-8 decode.  Returns the byte length of the cluster and its code
		// point.  Malformed input is treated as a single 1-byte cluster so a
		// broken string can never loop forever or overflow the buffer.
		struct Cluster
		{
			std::size_t   len{ 1 };
			std::uint32_t cp{ 0 };
		};

		Cluster Decode(std::string_view a_text, std::size_t a_at)
		{
			const auto byte = static_cast<unsigned char>(a_text[a_at]);
			if (byte < 0x80) {
				return { 1, byte };
			}

			std::size_t   len = 0;
			std::uint32_t cp  = 0;
			if ((byte & 0xE0) == 0xC0) {
				len = 2;
				cp  = byte & 0x1F;
			} else if ((byte & 0xF0) == 0xE0) {
				len = 3;
				cp  = byte & 0x0F;
			} else if ((byte & 0xF8) == 0xF0) {
				len = 4;
				cp  = byte & 0x07;
			} else {
				return { 1, byte };
			}

			if (a_at + len > a_text.size()) {
				return { 1, byte };
			}
			for (std::size_t i = 1; i < len; ++i) {
				const auto cont = static_cast<unsigned char>(a_text[a_at + i]);
				if ((cont & 0xC0) != 0x80) {
					return { 1, byte };
				}
				cp = (cp << 6) | (cont & 0x3F);
			}
			return { len, cp };
		}

		std::size_t Columns(std::uint32_t a_cp)
		{
			// East Asian wide + fullwidth forms.  Everything else is narrow;
			// being wrong by one column only moves a wrap point, never loses
			// text, so this stays deliberately simple.
			if (a_cp >= 0x1100 && a_cp <= 0x115F) return 2;   // Hangul Jamo
			if (a_cp >= 0x2E80 && a_cp <= 0x303E) return 2;   // CJK radicals, punctuation
			if (a_cp >= 0x3041 && a_cp <= 0x33FF) return 2;   // Kana, CJK compat
			if (a_cp >= 0x3400 && a_cp <= 0x4DBF) return 2;   // CJK ext A
			if (a_cp >= 0x4E00 && a_cp <= 0x9FFF) return 2;   // CJK unified
			if (a_cp >= 0xA000 && a_cp <= 0xA4CF) return 2;   // Yi
			if (a_cp >= 0xAC00 && a_cp <= 0xD7A3) return 2;   // Hangul syllables
			if (a_cp >= 0xF900 && a_cp <= 0xFAFF) return 2;   // CJK compat ideographs
			if (a_cp >= 0xFE30 && a_cp <= 0xFE6F) return 2;   // CJK compat forms
			if (a_cp >= 0xFF00 && a_cp <= 0xFF60) return 2;   // Fullwidth forms
			if (a_cp >= 0xFFE0 && a_cp <= 0xFFE6) return 2;   // Fullwidth signs
			if (a_cp >= 0x20000 && a_cp <= 0x3FFFD) return 2; // CJK ext B+
			return 1;
		}

		bool IsBreakAfter(std::uint32_t a_cp)
		{
			switch (a_cp) {
			case U'，':
			case U'、':
			case U'。':
			case U'；':
			case U'：':
			case U'/':
			case U'|':
			case U'）':
			case U')':
				return true;
			default:
				return false;
			}
		}

		std::string Trim(std::string_view a_text)
		{
			std::size_t begin = 0;
			std::size_t end   = a_text.size();
			while (begin < end && (a_text[begin] == ' ' || a_text[begin] == '\t')) {
				++begin;
			}
			while (end > begin && (a_text[end - 1] == ' ' || a_text[end - 1] == '\t')) {
				--end;
			}
			return std::string{ a_text.substr(begin, end - begin) };
		}

		// The field separator is " · ": space, U+00B7 (two bytes in UTF-8),
		// space.  Its byte length must be taken from the literal - writing 3
		// here leaves the trailing lead byte on the next atom, which shows up
		// as invisible garbage and an off-by-one column.
		constexpr std::string_view kSeparator{ " \u00B7 " };

		// Character-level wrap for a run that has no usable separator.
		void HardWrap(std::string_view a_text, std::size_t a_max, std::vector<std::string>& a_out)
		{
			std::size_t start = 0;
			std::size_t i     = 0;
			std::size_t width = 0;
			std::size_t lastBreak = std::string::npos;

			while (i < a_text.size()) {
				const auto cluster = Decode(a_text, i);
				const auto columns = Columns(cluster.cp);
				if (width + columns > a_max && width > 0) {
					std::size_t cut = i;
					// Prefer to end just after punctuation, if that is not so
					// early that the line becomes a stub.
					if (lastBreak != std::string::npos && lastBreak > start + (i - start) / 2) {
						cut = lastBreak;
					}
					a_out.push_back(Trim(a_text.substr(start, cut - start)));
					start     = cut;
					i         = cut;
					width     = 0;
					lastBreak = std::string::npos;
					continue;
				}
				width += columns;
				i += cluster.len;
				if (IsBreakAfter(cluster.cp)) {
					lastBreak = i;
				}
			}

			if (start < a_text.size()) {
				auto tail = Trim(a_text.substr(start));
				if (!tail.empty()) {
					a_out.push_back(std::move(tail));
				}
			}
		}
	}

	namespace Notify
	{
		std::size_t Width(std::string_view a_utf8)
		{
			std::size_t width = 0;
			std::size_t i     = 0;
			while (i < a_utf8.size()) {
				const auto cluster = Decode(a_utf8, i);
				width += Columns(cluster.cp);
				i += cluster.len;
			}
			return width;
		}

		std::vector<std::string> Wrap(std::string_view a_utf8, std::size_t a_max)
		{
			std::vector<std::string> out;
			if (a_max == 0) {
				a_max = 48;
			}

			// Forced line breaks first: a "\n" in the source is the author
			// saying "this is where it breaks".
			std::size_t paragraphStart = 0;
			while (paragraphStart <= a_utf8.size()) {
				auto        nl       = a_utf8.find('\n', paragraphStart);
				const auto  paraEnd  = (nl == std::string_view::npos) ? a_utf8.size() : nl;
				const auto  paragraph = a_utf8.substr(paragraphStart, paraEnd - paragraphStart);

				// Fields are written as "a · b · c".  Breaking *before* the
				// separator keeps both the glyph and the meaning on one side
				// of the line, so a wrapped block reads like a list.
				std::vector<std::string> atoms;
				std::size_t              pos = 0;
				while (pos <= paragraph.size()) {
					auto       sep = paragraph.find(kSeparator, pos);
					const auto end = (sep == std::string_view::npos) ? paragraph.size() : sep;
					auto       atom = Trim(paragraph.substr(pos, end - pos));
					if (!atom.empty()) {
						atoms.push_back(std::move(atom));
					}
					if (sep == std::string_view::npos) {
						break;
					}
					pos = sep + kSeparator.size();
				}

				std::string line;
				std::size_t lineWidth = 0;
				for (auto& atom : atoms) {
					const auto atomWidth = Width(atom);
					const auto needed    = line.empty() ? atomWidth : lineWidth + 3 + atomWidth;
					if (!line.empty() && needed > a_max) {
						out.push_back(line);
						line.clear();
						lineWidth = 0;
					}
					if (atomWidth > a_max) {
						std::vector<std::string> pieces;
						HardWrap(atom, a_max, pieces);
						if (!line.empty()) {
							out.push_back(line);
							line.clear();
							lineWidth = 0;
						}
						for (std::size_t i = 0; i + 1 < pieces.size(); ++i) {
							out.push_back(std::move(pieces[i]));
						}
						if (!pieces.empty()) {
							line      = std::move(pieces.back());
							lineWidth = Width(line);
						}
						continue;
					}
					if (line.empty()) {
						line      = atom;
						lineWidth = atomWidth;
					} else {
						line += " · ";
						line += atom;
						lineWidth = needed;
					}
				}
				if (!line.empty()) {
					out.push_back(std::move(line));
				}

				if (nl == std::string_view::npos) {
					break;
				}
				paragraphStart = nl + 1;
			}

			return out;
		}
	}
}
