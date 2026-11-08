#include "PCH.h"

#include "Ini.h"
#include "Notify.h"

namespace S3U
{
	namespace Notify
	{
		void Push(std::string_view a_utf8)
		{
			if (!Ini::Get().notify) {
				return;
			}

			const auto width = static_cast<std::size_t>(
				std::max<std::int32_t>(8, Ini::Get().notifyWidth));

			// One notification per line.  The HUD stacks consecutive
			// notifications downwards, so a wrapped block still reads as a
			// block rather than as three unrelated messages.
			for (const auto& line : Wrap(a_utf8, width)) {
				RE::DebugNotification(line.c_str());
			}
		}
	}
}
