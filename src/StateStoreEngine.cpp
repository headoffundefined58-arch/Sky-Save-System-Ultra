#include "PCH.h"

#include "StateStoreEngine.h"

#include "Audit.h"
#include "Ini.h"
#include "SaveGuard.h"
#include "Trend.h"

#include <cmath>
#include <string>

namespace S3U::StateStore
{
	namespace
	{
		using State::Kind;

		// The derive provider only ever READS.  It is called while a save is
		// being deserialised, so every engine singleton is null-checked and a
		// miss simply leaves the field at its declared default.  Nothing here
		// can throw into an SKSE callback, and nothing here can become stale
		// on disk because derive fields are never written.
		bool EngineDerive(const char* a_key, Value& a_out)
		{
			const std::string_view key{ a_key };

			if (key == "meta/gameDays") {
				if (auto* calendar = RE::Calendar::GetSingleton()) {
					a_out.f = static_cast<double>(calendar->GetDaysPassed());
					return true;
				}
				return false;
			}

			if (key == "vault/entryCount") {
				a_out.i = static_cast<std::int64_t>(Last().entries);
				return true;
			}
			if (key == "vault/bodyCrc") {
				a_out.i = 0;  // recomputed by the reader, never stored
				return true;
			}

			if (key == "guard/headerOk") {
				const auto& g = Guard::Last();
				a_out.b = g.verified && g.ok && g.located;
				return true;
			}
			if (key == "guard/lastCheck") {
				a_out.s = Guard::Last().note;
				return true;
			}

			if (key.starts_with("stats/") && Audit::HasResult()) {
				const auto& r = Audit::Last();
				if (key == "stats/forms")         { a_out.i = static_cast<std::int64_t>(r.totalForms); return true; }
				if (key == "stats/scripts")       { a_out.i = static_cast<std::int64_t>(r.scriptInstances); return true; }
				if (key == "stats/refs")          { a_out.i = static_cast<std::int64_t>(r.refs); return true; }
				if (key == "stats/deleted")       { a_out.i = static_cast<std::int64_t>(r.deletedRefs); return true; }
				if (key == "stats/nullBaseRefs")  { a_out.i = static_cast<std::int64_t>(r.nullBaseRefs); return true; }
			}

			if (key.starts_with("player/") && key != "player/lastCell") {
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player) {
					return false;
				}
				std::string label;
				if (key == "player/ref") {
					label = "player:" + std::to_string(player->GetFormID());
				} else if (key == "player/cell") {
					label = player->GetParentCell() ?
						"cell:" + std::to_string(player->GetParentCell()->GetFormID()) : "";
				} else if (key == "player/world") {
					label = player->GetWorldspace() ?
						"world:" + std::to_string(player->GetWorldspace()->GetFormID()) : "";
				} else {
					return false;
				}
				a_out.s = label;
				return true;
			}

			if (key.starts_with("trend/")) {
				const auto& history = Trend::History();
				if (history.empty()) {
					return false;
				}
				const auto& e = history.back();
				if (key == "trend/saveIndex") { a_out.i = e.saveIndex; return true; }
				if (key == "trend/forms")     { a_out.i = static_cast<std::int64_t>(e.totalForms); return true; }
				if (key == "trend/scripts")   { a_out.i = static_cast<std::int64_t>(e.scriptInstances); return true; }
				if (key == "trend/refs")      { a_out.i = static_cast<std::int64_t>(e.refs); return true; }
				if (key == "trend/deleted")   { a_out.i = static_cast<std::int64_t>(e.deletedRefs); return true; }
			}

			if (key == "loadorder/count") {
				std::string baseline;
				if (!GetString("loadorder/list", baseline) || baseline.empty()) {
					return false;
				}
				std::size_t n = 1;
				for (const char c : baseline) { if (c == '\n') { ++n; } }
				a_out.i = static_cast<std::int64_t>(n);
				return true;
			}

			// loadorder/current, loadorder/diff and audit/report are produced
			// by their own modules on demand; they have no independent value
			// to recompute here, and leaving them at the default is honest.
			return false;
		}
	}

	void InitializeEngine()
	{
		const auto dir = Paths::EnsureDataDir() / "State";
		Initialize(dir);
		SetDeriveProvider(&EngineDerive);
		// v2.0.0: the schema table is emitted into the single report by
		// main.cpp.  Writing a separate state-schema.txt would violate the
		// one-txt retention rule.
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc) {
			return;
		}
		const auto bytes = Serialize();
		if (bytes.empty()) {
			return;
		}
		if (!a_intfc->OpenRecord(kRecordType, kFormat)) {
			return;
		}
		a_intfc->WriteRecordData(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
	}

	bool SetForm(std::string_view a_key, const RE::TESForm* a_form)
	{
		if (!a_form) {
			return false;
		}
		const auto* spec = State::Find(a_key);
		if (!spec || spec->kind != Kind::kString) {
			return false;
		}
		// This is the point of the new classification in practice: an old
		// Vault::SetForm would have persisted a raw FormID that might dangle
		// after a load-order change.  Now the value only ever reaches the
		// derived in-memory view, where it is recomputed on the next load.
		Value v;
		v.kind = Kind::kString;
		v.s = std::to_string(a_form->GetFormID());
		return Set(a_key, v);
	}

	RE::TESForm* GetForm(std::string_view a_key)
	{
		// No stored FormID is trusted any more.  Resolve from the live engine.
		if (a_key == "player/ref") {
			return RE::PlayerCharacter::GetSingleton();
		}
		return nullptr;
	}
}
