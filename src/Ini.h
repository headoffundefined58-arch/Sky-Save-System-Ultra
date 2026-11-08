#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace S3U
{
	// -------------------------------------------------------------------------
	// Paths
	//
	// Everything the plugin reads or writes lives under
	// <game>\Data\SKSE\Plugins\3SU.  The game's own directory is recovered
	// from the module path instead of the working directory: MO2 launches the
	// game with a different cwd on some setups, and the working directory is
	// also what the VFS hook rewrites, so it is not a trustworthy anchor.
	// -------------------------------------------------------------------------
	namespace Paths
	{
		std::filesystem::path GameRoot();
		std::filesystem::path PluginDir();
		std::filesystem::path IniFile();
		std::filesystem::path EnsureDataDir();
	}

	namespace Ini
	{
		struct Settings
		{
			// Run the read-only audit once every time a save finishes loading.
			bool auditOnLoad{ true };

			// Walk attached cells and count references.  Costs a few ms on a
			// heavy load order; disable to make the audit nearly free.
			bool scanReferences{ true };

			// "存档体检完成" toast in the top-left corner.
			bool notify{ true };

			// How many display columns one notification line may use, where a
			// CJK glyph counts 2 and an ASCII character 1.  The HUD draws a
			// notification as a single line at a fixed x, so anything wider
			// than the screen is simply clipped - there is no scrolling and no
			// wrapping inside the game.  This plugin therefore breaks long
			// text itself and pushes one notification per line.
			// 48 = 24 Chinese characters, which fits at every HUD scale this
			// was tested at.  Lower it if your HUD is enlarged by another mod.
			std::int32_t notifyWidth{ 48 };

			// 0 = no limit.  Caps how many cells are inspected, for people who
			// want a bounded worst-case cost.  Cells are visited in form order,
			// so a cap makes the numbers a sample rather than a total.
			std::int32_t maxCells{ 0 };

			// Warn in the log when the change-form / reference count grows by
			// more than this percentage between two saves.
			std::int32_t growthWarnPercent{ 15 };

			// Log per-script-name histograms (top 20) into the report.
			bool verbose{ false };

		// ---- SaveGuard ---------------------------------------------------
		// v2.0.0: NO BACKUPS.  The whole backup/rotation subsystem was removed
		// at the user's request.  This switch only controls the read-only
		// structural check performed before a load; it never writes a file.
		bool guardEnabled{ true };

		// Deprecated keys, parsed so old ini files do not warn, but ignored.
		// The system does not provide backups and does not keep history.
		std::int32_t backupCount{ 0 };
		std::int32_t vaultHistory{ 0 };

		// Empty = <Documents>\My Games\Skyrim Special Edition\Saves.
		// Only needed when the game lives somewhere the shell cannot see.
		std::string saveDir;

		// ---- Vault -------------------------------------------------------
		// Keep an independent, checksummed copy of this plugin's state
		// outside the .ess, so a damaged save file cannot take it with it.
		bool vaultEnabled{ true };

			// Resolve every stored form reference after a load and name the ones
			// that no longer exist.  Cheap, and it turns a future crash into a
			// present log line.
			bool vaultReconcile{ true };

			// ---- ModGuard ----------------------------------------------------
			// Apply declarative migration manifests found in
			// Data\SKSE\Plugins\3SU\Migrations.  With no manifests
			// present this does nothing, so it is safe to leave on.
			bool migrateEnabled{ true };

			// Deprecated: the old Migrate candidate auto-start.  InitEngine owns
			// startup quests now; keeping the key avoids breaking old ini files.
			bool autoStartNewModQuests{ false };

		// ---- Load-time initialisation ------------------------------------
		// Replay New-Game initialisation for plugins added after this save
		// was written.  This is the actual fix for "requires a new game".
		// On by default: it is guarded by a save-bound write-ahead receipt
		// on disk, so it runs at most once per plugin per save file, and a
		// failed pass is rolled back rather than left half-applied.
		bool initEnabled{ true };

		// ---- Load-time self-healing --------------------------------------
		// Actually repair a risky save after it has loaded: disable and mark
		// dangling references for deletion, purge receipts of removed mods,
		// and rewrite the load-order baseline so the next load is clean.
		bool repairEnabled{ true };

		// Show the explicit "save now, then reload this save" message box and
		// HUD lines when a verified repair happened.
		bool repairPrompt{ true };
		};

		const Settings& Get();
		void            Load();
	}
}
