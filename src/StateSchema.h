#pragma once

// -----------------------------------------------------------------------------
// StateSchema - the single source of truth for what survives a save.
//
// Two independent questions are answered here, and the earlier design answered
// neither of them:
//
//   1. WHETHER a value must be written at all        -> Policy
//   2. WHOSE value it is, i.e. who may inherit it    -> Scope
//
// Question 1 stops a stale copy of a derivable value from contradicting the
// live world.  Question 2 stops a value from being inherited by a save that
// never produced it - which is the real reason a mod "still needs a new game"
// even after load-time initialisation exists:
//
//   A save written before the mod was installed has no baseline for it.  If
//   the baseline and the initialisation receipts are inherited from *any*
//   other save of the same character, the comparison reports "nothing added",
//   initialisation is skipped, and the mod stays dead.  The bug is not in the
//   initialiser; it is in the inheritance rule.
//
// Therefore every field is also classified by Scope:
//
//   kSave       - belongs to exactly one .ess.  Another save must never
//                 inherit it; the co-save block of the .ess being loaded may,
//                 because that block travels inside the .ess itself.
//   kCharacter  - describes the character across all of their saves.
//   kGlobal     - format/engine level, identical for everybody.
//
// The writer consults this table before emitting a byte, and the reader
// consults it before accepting one: an old save's kDerive/kEphemeral field is
// discarded, and a kSave field arriving from a different save is discarded
// too.  That is "no longer inheriting the old retention rules" expressed as
// code, with a per-field reason attached.
//
// Pure header: no engine, no SKSE, no filesystem.  tools/state_test.cpp
// compiles this exact header to prove the classification itself.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace S3U::State
{
	// What is the field's relationship to a save/load cycle?
	enum class Policy : std::uint8_t
	{
		// A historical fact.  If it is missing after a load, information has
		// been lost and no amount of recomputation can bring it back.
		kPersist,

		// A value that describes the world or the file right now.  It must be
		// recomputed after every load; keeping a copy would only let it go
		// stale and contradict the world it claims to describe.
		kDerive,

		// Meaningful only inside one process.  It must be reset to its default
		// on every load: carrying it over is a bug, not a convenience.
		kEphemeral
	};

	// Where a kPersist field is actually written.  This is deliberately a
	// separate axis from Policy: a fact can be authoritative yet still need a
	// different durability mechanism (a receipt written before the world is
	// touched cannot wait until the next save).
	enum class Storage : std::uint8_t
	{
		kSnapshot,  // travels in the co-save and the disk snapshot
		kWal,       // append-only log, fsynced at the moment the fact occurs
		kNone       // never written; derive and ephemeral fields live here
	};

	// Who owns the value.  See the header comment: this is the axis that
	// decides whether a value may be inherited by the save being loaded.
	enum class Scope : std::uint8_t
	{
		kSave,       // one .ess only
		kCharacter,  // one character, every save of theirs
		kGlobal      // everyone
	};

	enum class Kind : std::uint8_t
	{
		kInt    = 0,
		kFloat  = 1,
		kBool   = 2,
		kString = 3,
		kBlob   = 4
	};

	struct Spec
	{
		const char* key;         // trailing '*' marks a dynamic family
		Policy      policy;
		Storage     storage;
		Scope       scope;
		Kind        kind;
		const char* rationale;   // shown verbatim in the generated report
	};

	inline constexpr Spec kSpecs[] = {
		// ---- kPersist / kSnapshot / kGlobal --------------------------------
		{ "schema/version", Policy::kPersist, Storage::kSnapshot, Scope::kGlobal, Kind::kString,
			"格式版本。缺了它就无法解释其余字节，必须随状态本身一起留存；所有存档同一套格式。" },
		{ "meta/version", Policy::kPersist, Storage::kSnapshot, Scope::kGlobal, Kind::kString,
			"写档时的插件版本。旧档识别与格式升级的判据，与具体角色无关。" },

		// ---- kPersist / kSnapshot / kCharacter -----------------------------
		{ "identity/character", Policy::kPersist, Storage::kSnapshot, Scope::kCharacter, Kind::kString,
			"角色归属。写档时的角色身份是历史事实；读取时用它拒绝另一个角色的状态。" },
		{ "tx/generation", Policy::kPersist, Storage::kSnapshot, Scope::kCharacter, Kind::kInt,
			"事务代数。整个角色共用一条单调序列，跨存档比较才有意义。" },
		{ "meta/saveCount", Policy::kPersist, Storage::kSnapshot, Scope::kCharacter, Kind::kInt,
			"累计保存次数。历史计数，任何当前状态都推不出来；属于角色而非某一份存档。" },
		{ "meta/firstSeen", Policy::kPersist, Storage::kSnapshot, Scope::kCharacter, Kind::kString,
			"首次为该角色建立状态的时间。历史事实，不重算。" },
		{ "trend/history", Policy::kPersist, Storage::kSnapshot, Scope::kCharacter, Kind::kBlob,
			"逐次保存的观测序列。过去的世界已不在内存，序列无法重算；换档回看仍应延续。" },

		// ---- kPersist / kSnapshot / kSave ----------------------------------
		// Facts that describe ONE save.  Inheriting them from a different save
		// is exactly how "nothing was added" gets reported for a save that
		// predates half the load order.
		{ "identity/saveName", Policy::kPersist, Storage::kSnapshot, Scope::kSave, Kind::kString,
			"写档时的文件名。它是存档作用域的钥匙：换档时据此拒绝继承存档级字段。" },
		{ "loadorder/list", Policy::kPersist, Storage::kSnapshot, Scope::kSave, Kind::kBlob,
			"写这份存档当时的加载顺序基线。换档后当前引擎只能给出「现在」，「当时」无从重算；"
			"继承别的存档的基线会让新增插件被误判为「一个都没加」。" },
		{ "init/generation", Policy::kPersist, Storage::kSnapshot, Scope::kSave, Kind::kInt,
			"这份存档已经执行过的初始化轮数。换档必须从零计，否则回滚与重试判据全错。" },
		{ "init/applied/*", Policy::kPersist, Storage::kSnapshot, Scope::kSave, Kind::kString,
			"这份存档已完成初始化的插件收据。随 .ess 走，所以改名另存也仍然有效；"
			"但另一份存档绝不能拿它当自己的收据，否则该档永远等不到初始化。" },
		{ "migrate/*", Policy::kPersist, Storage::kSnapshot, Scope::kSave, Kind::kInt,
			"迁移清单执行收据。additem 不幂等，而物品是写进具体某份存档的，因此按存档记账。" },
		{ "player/lastCell", Policy::kPersist, Storage::kSnapshot, Scope::kSave, Kind::kString,
			"上次保存时所在单元的具名引用，用于逐档对比。它描述的是那一份存档，不是角色当前位置。" },

		// ---- kPersist / kWal / kSave ---------------------------------------
		{ "init/receipts/*", Policy::kPersist, Storage::kWal, Scope::kSave, Kind::kString,
			"载入期初始化的写前收据。当场 flush，不依赖玩家保存；按存档绑定，"
			"因为「初始化做完但还没存」这件事只对那一份 .ess 成立。" },

		// ---- kDerive --------------------------------------------------------
		// Values that describe the present.  Recomputed after every load;
		// deliberately never written, deliberately dropped if an old save
		// still carries them.
		{ "loadorder/count", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：基线插件数可直接从 loadorder/list 数出，单存一份只可能与基线不一致。" },
		{ "loadorder/current", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kBlob,
			"舍弃：当前加载顺序直接读 DataHandler，存的是会过期的副本。" },
		{ "loadorder/diff", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kBlob,
			"舍弃：新增/缺失/换序由基线与当前顺序推导，结论每次读档都会变。" },
		{ "meta/gameDays", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kFloat,
			"舍弃：加载后 Calendar 已是存档值，直接读取即可，存副本纯属冗余。" },
		{ "player/ref", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kString,
			"舍弃：玩家引用从 PlayerCharacter 取;存 FormID 反而可能在换序后悬空。" },
		{ "player/cell", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kString,
			"舍弃：当前所在单元由引擎维护，不属于插件状态。" },
		{ "player/world", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kString,
			"舍弃：当前世界空间由引擎维护，不属于插件状态。" },
		{ "stats/forms", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：form 总数每次读档扫世界即得，存旧值会与实况矛盾。" },
		{ "stats/scripts", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：脚本实例数同上。" },
		{ "stats/refs", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：引用数同上，且只覆盖已加载单元。" },
		{ "stats/deleted", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：已删除引用数同上。" },
		{ "stats/nullBaseRefs", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：空基引用数同上，是本次扫描的结论而非事实。" },
		{ "guard/headerOk", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kBool,
			"舍弃：本次读入的 .ess 头校验结论，每次读档都要重做。" },
		{ "guard/lastCheck", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kString,
			"舍弃：校验说明文本，同上。" },
		{ "audit/report", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kBlob,
			"舍弃：体检结论每次读档重算;留存旧结论会误导判断。" },
		{ "vault/entryCount", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：快照条目数在读文件时重算。" },
		{ "vault/bodyCrc", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：快照校验值在读文件时重算。" },
		{ "trend/saveIndex", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：已并入 trend/history 序列，单独存会与序列末项不一致。" },
		{ "trend/forms", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：同上，最后一次观测值由序列末项给出。" },
		{ "trend/scripts", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：同上。" },
		{ "trend/refs", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：同上。" },
		{ "trend/deleted", Policy::kDerive, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：同上。" },

		// ---- kEphemeral -----------------------------------------------------
		// Process-local scratch.  Reset on every load, never written.
		{ "session/notifyQueue", Policy::kEphemeral, Storage::kNone, Scope::kGlobal, Kind::kBlob,
			"舍弃：本会话待显示通知，载入后无意义。" },
		{ "session/pendingInitUnits", Policy::kEphemeral, Storage::kNone, Scope::kGlobal, Kind::kBlob,
			"舍弃：本轮待投递的初始化单元，任务队列是进程内的。" },
		{ "session/lastError", Policy::kEphemeral, Storage::kNone, Scope::kGlobal, Kind::kString,
			"舍弃：本次错误文本，不应污染下次读档。" },
		{ "session/reportDirty", Policy::kEphemeral, Storage::kNone, Scope::kGlobal, Kind::kBool,
			"舍弃：报告待重写标志，仅在本次载入内有效。" },
		{ "session/coLoadBuffer", Policy::kEphemeral, Storage::kNone, Scope::kGlobal, Kind::kBlob,
			"舍弃：co-save 读入缓冲，读完即弃。" },
		{ "session/elapsedMs", Policy::kEphemeral, Storage::kNone, Scope::kGlobal, Kind::kFloat,
			"舍弃：本次耗时是测量值，不是状态。" },
		{ "runtime/frameNo", Policy::kEphemeral, Storage::kNone, Scope::kGlobal, Kind::kInt,
			"舍弃：帧号是进程内计数。" },
	};

	inline constexpr std::size_t kSpecCount = std::size(kSpecs);

	inline constexpr bool IsFamily(const Spec& a_spec) noexcept
	{
		const std::string_view key{ a_spec.key };
		return !key.empty() && key.back() == '*';
	}

	// Exact match wins; otherwise a trailing-* family matches by prefix.
	// Returns nullptr for an undeclared key - the caller MUST treat that as
	// "not a state field at all", never as an implicit kPersist.
	inline const Spec* Find(std::string_view a_key) noexcept
	{
		for (const auto& spec : kSpecs) {
			if (!IsFamily(spec) && std::string_view{ spec.key } == a_key) {
				return &spec;
			}
		}
		for (const auto& spec : kSpecs) {
			if (!IsFamily(spec)) {
				continue;
			}
			const std::string_view pattern{ spec.key };
			const auto             prefix = pattern.substr(0, pattern.size() - 1);
			if (a_key.size() >= prefix.size() && a_key.substr(0, prefix.size()) == prefix) {
				return &spec;
			}
		}
		return nullptr;
	}

	inline constexpr const char* PolicyName(Policy a_policy) noexcept
	{
		switch (a_policy) {
		case Policy::kPersist:   return "必须持久化";
		case Policy::kDerive:    return "可推导·舍弃";
		case Policy::kEphemeral: return "临时·舍弃";
		default:                 return "未知";
		}
	}

	inline constexpr const char* StorageName(Storage a_storage) noexcept
	{
		switch (a_storage) {
		case Storage::kSnapshot: return "快照";
		case Storage::kWal:      return "预写日志";
		case Storage::kNone:     return "不落盘";
		default:                 return "未知";
		}
	}

	inline constexpr const char* ScopeName(Scope a_scope) noexcept
	{
		switch (a_scope) {
		case Scope::kSave:      return "存档级";
		case Scope::kCharacter: return "角色级";
		case Scope::kGlobal:    return "全局";
		default:                return "未知";
		}
	}

	inline constexpr const char* KindName(Kind a_kind) noexcept
	{
		switch (a_kind) {
		case Kind::kInt:    return "整数";
		case Kind::kFloat:  return "浮点";
		case Kind::kBool:   return "布尔";
		case Kind::kString: return "文本";
		case Kind::kBlob:   return "字节块";
		default:            return "未知";
		}
	}

	inline constexpr std::size_t CountBy(Policy a_policy) noexcept
	{
		std::size_t n = 0;
		for (const auto& spec : kSpecs) {
			if (spec.policy == a_policy) {
				++n;
			}
		}
		return n;
	}

	inline constexpr std::size_t CountBy(Scope a_scope) noexcept
	{
		std::size_t n = 0;
		for (const auto& spec : kSpecs) {
			if (spec.policy == Policy::kPersist && spec.scope == a_scope) {
				++n;
			}
		}
		return n;
	}
}
