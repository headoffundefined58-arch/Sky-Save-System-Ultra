#pragma once

#include <cstdint>
#include <string>

namespace RE
{
	class TESForm;
}

namespace S3U
{
	// -------------------------------------------------------------------------
	// FormRef - a reference to a game object that survives a save/load cycle.
	//
	// The rule this type enforces is the whole point of it:
	//
	//     no address and no runtime form ID ever crosses a save boundary.
	//
	// A runtime form ID is only meaningful inside one process; the instant the
	// load order changes, or the plugin that owned the form is uninstalled, the
	// same number points at a different object.  Storing it and dereferencing it
	// later is exactly how "worked yesterday, crash today" happens.
	//
	// So the identity we persist is the pair (plugin file name, local ID) - the
	// same idea as Minecraft's namespace:id.  Resolution happens at load time
	// against the actual load order, and *fails closed*: a missing plugin or a
	// deleted record yields nullptr, never a stale pointer.  A caller that
	// checks for null cannot crash; a caller that dereferences garbage will.
	// -------------------------------------------------------------------------
	struct FormRef
	{
		// Lower-case file name of the owning plugin ("skyrim.esm"), or empty
		// for a runtime-created form, which has no stable identity at all.
		std::string plugin;

		// Form ID inside that plugin, i.e. the low 24 bits.
		std::uint32_t localID{ 0 };

		// The raw runtime ID as it was at save time.  Only used for forms with
		// no owning plugin (0xFF...... runtime objects): those have no stable
		// name, so the engine's own ID-remap table is the only chance of
		// finding them again.  Ignored for normal records.
		std::uint32_t runtimeIDAtSave{ 0 };

		[[nodiscard]] bool Empty() const noexcept { return plugin.empty() && localID == 0; }

		// nullptr when the plugin is gone, the record is gone, or the form was
		// deleted.  Never returns a dangling pointer.
		[[nodiscard]] RE::TESForm* Resolve() const noexcept;

		// Same, but typed, and equally null-safe.
		template <class T>
		[[nodiscard]] T* ResolveAs() const noexcept
		{
			auto* form = Resolve();
			return form ? form->As<T>() : nullptr;
		}

		[[nodiscard]] std::string ToString() const;
	};

	// Captures the persistent identity of a live form.  Returns an empty ref
	// when the form has no stable identity (runtime-created objects), which is
	// the honest answer: those must be rebuilt by their owning mod, not stored.
	[[nodiscard]] FormRef MakeFormRef(const RE::TESForm* a_form);

	// "skyrim.esm:0x0001A2B3", for logs and reports.
	[[nodiscard]] std::string FormIDToString(std::uint32_t a_formID);
}
