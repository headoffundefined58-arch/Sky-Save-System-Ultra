#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace S3U
{
	// -------------------------------------------------------------------------
	// Notify - HUD toasts that fit on the screen.
	//
	// The game draws every notification as a single left-aligned line starting
	// at a fixed offset inside hudmenu.gfx.  That offset is compiled into the
	// movie: there is no ini key for it and no scripted setter, so "move the
	// text left" can at best reclaim the few percent of margin before the text
	// starts.  It cannot recover a line that is 30% wider than the screen.
	//
	// What actually works is not making the line that long.  So this module
	// measures a string in display columns (a CJK glyph is two, ASCII one),
	// breaks it at the separators this plugin already uses, and emits one
	// notification per line.  Consecutive notifications stack downwards, which
	// is exactly the multi-line block we want.
	//
	// The width is configurable (iNotifyWidth) because HUD scale, aspect ratio
	// and font mods all change how many columns fit.
	// -------------------------------------------------------------------------
	namespace Notify
	{
		// Display columns of a UTF-8 string: wide glyphs count 2, others 1.
		std::size_t Width(std::string_view a_utf8);

		// Greedy wrap on break-friendly characters.  A single unbreakable run
		// longer than a_max is split by glyph, never dropped.
		std::vector<std::string> Wrap(std::string_view a_utf8, std::size_t a_max);

		// Wrap at the configured width and push each line to the HUD.
		// Honours bNotify, so callers never have to check the setting.
		void Push(std::string_view a_utf8);
	}
}
