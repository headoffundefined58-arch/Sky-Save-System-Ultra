#include "PCH.h"

#include "FormRef.h"

#include <cstdio>

namespace S3U
{
	namespace
	{
		std::string ToLower(std::string_view a_view)
		{
			std::string out{ a_view };
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}
	}

	RE::TESForm* FormRef::Resolve() const noexcept
	{
		// Every path out of this function is either a validated live form or
		// nullptr.  There is deliberately no "best effort" branch: returning a
		// pointer we are not sure about is the exact failure mode this type
		// exists to prevent.
		if (localID == 0) {
			return nullptr;
		}

		try {
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return nullptr;
			}

			RE::FormID runtimeID = 0;

			if (plugin.empty()) {
				// No owning plugin: a runtime-created form (0xFF......).  Its
				// only chance of being found again is the engine's own ID-remap
				// table, which SKSE exposes for exactly this case.  A miss is
				// normal and is reported as a tombstone, not as an error.
				RE::FormID mapped = 0;
				auto*      serialization = SKSE::GetSerializationInterface();
				if (serialization && runtimeIDAtSave != 0 &&
					serialization->ResolveFormID(runtimeIDAtSave, mapped) && mapped != 0) {
					runtimeID = mapped;
				} else {
					runtimeID = localID;
				}
			} else {
				runtimeID = dh->LookupFormID(localID, plugin);
				if (runtimeID == 0) {
					return nullptr;  // plugin not loaded, or record not in it
				}
			}

			auto* form = RE::TESForm::LookupByID(runtimeID);
			if (!form) {
				return nullptr;
			}

			// A deleted record is a tombstone: the engine still resolves it, and
			// touching it is how "the object existed a second ago" crashes turn
			// up.  Treat it as absent, which is what every caller means.
			if (form->IsDeleted()) {
				return nullptr;
			}

			return form;
		} catch (...) {
			return nullptr;
		}
	}

	std::string FormRef::ToString() const
	{
		if (Empty()) {
			return "<none>";
		}
		if (plugin.empty()) {
			return FormIDToString(localID) + " (runtime)";
		}
		return plugin + ":" + FormIDToString(localID);
	}

	FormRef MakeFormRef(const RE::TESForm* a_form)
	{
		FormRef ref;

		if (!a_form) {
			return ref;
		}

		try {
			const auto formID = a_form->GetFormID();
			if (formID == 0) {
				return ref;
			}

			// Runtime-created forms live at 0xFF...... and are not addressable
			// after a reload.  Recording them as a stable reference would be a
			// lie, so only the raw ID is kept and Resolve() will fail closed.
			if (a_form->IsDynamicForm()) {
				ref.plugin            = {};
				ref.localID           = formID;
				ref.runtimeIDAtSave   = formID;
				return ref;
			}

			ref.localID = formID & 0x00FFFFFFu;

			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return ref;
			}

			const auto count = dh->GetLoadedModCount();
			auto**     mods  = dh->GetLoadedMods();
			if (!mods) {
				return ref;
			}

			for (std::uint32_t i = 0; i < count; ++i) {
				auto* file = mods[i];
				if (!file) {
					continue;
				}
				if (file->IsFormInMod(formID)) {
					ref.plugin = ToLower(file->GetFilename());
					return ref;
				}
			}

			// Owner unknown: keep the local ID, leave plugin empty so Resolve()
			// does not guess a wrong owner.
			return ref;
		} catch (...) {
			return {};
		}
	}

	std::string FormIDToString(std::uint32_t a_formID)
	{
		char buffer[16]{};
		std::snprintf(buffer, sizeof(buffer), "0x%08X", a_formID);
		return buffer;
	}
}
