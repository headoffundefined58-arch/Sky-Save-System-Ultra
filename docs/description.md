# Sky Save System Ultra — 宣传文案（可直接粘贴）

> 版本以编译产物为准：**2.0.0**（源码 kPluginVersion / CMakeLists 均为 2.0.0）。
> 注意：模组目录里的 README.txt 与 SkySaveSystemUltra.ini 头部写的是 v4.0.0，
> 与编译版本不一致，发布前请统一（要么改代码常量，要么 Nexus 页面就用 2.0.0）。
> 另：运行时数据目录在代码里是 `Data\SKSE\Plugins\3SU\`，README 写的
> `SkySaveSystemUltra\` 是旧路径，正文已按代码实际路径写。

---

## 一、Nexus Mods 简介（Summary 栏，一句话）

> Automatically repairs Skyrim saves that won't load because they reference deleted mods — silently, before you ever see the "save is corrupted" error.

> 自动修复因引用了已删除 MOD 而无法载入的存档——在你看到"存档已损坏"之前，静默修好。

---

## 二、Nexus Mods 正文（English，BBCode）

[b][size=6]Sky Save System Ultra — Automatic Save Repair[/size][/b]
[i]Corrupted saves that reference deleted mods, fixed automatically — before the engine ever shows the "save is corrupted" error.[/i]

[size=4][b]What it does[/b][/size]

Sky Save System Ultra targets the most common cause of an unloadable save: a plugin list that names mods you have since removed. The engine resolves every plugin name in the save against the current load order; when one is missing, every index after it shifts and the save is rejected as corrupted — often before you can even enter the world.

It fixes this at the file level, before the engine reads the save. At game start it scans every save directory and silently repairs any save that points at a missing mod, so the "the following content is no longer available" prompt never appears in the Load menu. If a save slips through, it catches it again at pre-load and repairs on the spot.

[size=4][b]Features[/b][/size]
[list]
[*]Startup pre-scan: repairs all corrupted saves across every save directory before the main menu's Load list is shown
[*]Pre-load fallback: a save missed by the pre-scan is detected the moment you pick it, repaired, then auto-reloaded
[*]File-level repair with no data guessing: the missing plugin name is replaced in place with an equal-length currently-loaded plugin name, so the plugin table's byte count stays identical and the rest of the (multi-MB) file is never touched
[*]Atomic replacement: the repaired save is written to a temp file and renamed over the original, so a crash mid-repair can never leave a half-written .ess
[*]Silent helper: a small standalone .exe does the repair with no window and exits the instant it finishes — near-zero footprint, no background process left running
[*]Load-order baseline rebuilt on reload: after repair, leftover changeforms pointing at the removed mod are discarded by the engine on the next load, so the save comes back clean
[*]New-game-required mods initialize in old saves: plugins added after a save was written get their Start-Game-Enabled quests replayed — the same path the engine uses on a fresh game — each guarded by a save-bound receipt so it runs at most once
[*]Post-load self-healing: dangling references to removed mods are marked for deletion, removed-mod receipts are purged, and the load-order baseline is rewritten so the next load is clean
[*]Read-only diagnostics after every load: reference counts, dangling-reference tallies, and per-script histograms, with a growth warning when form counts jump between saves
[*]ESPless: no plugin file, no MCM, no SkyUI — configuration is a plain .ini you edit by hand
[/list]

[size=4][b]Compatibility[/b][/size]

There is no mod whitelist. The repair keys off the save's own plugin table versus the engine's live load order, so it works regardless of which mods are missing — including ones released after this tool. It does not touch your load order, does not edit other mods, and never writes into a healthy save.

[b]Load order:[/b] no ESP/ESL of its own, so it cannot conflict with anything in your order. The repair only acts on saves flagged as referencing missing plugins.

[size=4][b]Interface[/b][/size]

There is no on-screen UI for the repair itself — by design it is invisible. Diagnostics surface as a short top-left notification and a written report under the plugin's data folder.

[size=4][b]Configuration[/b][/size]

A single [b]SkySaveSystemUltra.ini[/b] under [code]Data\SKSE\Plugins\[/code]. Toggles cover repair-on-load, the post-repair prompt, the startup pre-scan, audit verbosity, and notification width. All settings are plain text and editable by hand; none are written into your save.

[size=4][b]Performance[/b][/size]

The helper .exe launches only for a flagged save and exits when done — no process is left running. The in-game work is event-driven (once per load), not polled every frame; the audit's cell walk is the only step with real cost (tens of milliseconds on a heavy order) and can be capped or disabled.

[size=4][b]Requirements[/b][/size]
[list]
[*]Skyrim Special Edition [b]1.5.97[/b] [i]or[/i] Anniversary Edition [b]1.6.x[/b] (two DLLs shipped — see Installation)
[*][url=https://skse.silverlock.org/]SKSE64[/url] matching your game version
[*][url=https://www.nexusmods.com/skyrimspecialedition/mods/32444]Address Library for SKSE Plugins[/url] — required by the CommonLibSSE-NG build, matching your game version
[/list]

[size=4][b]Installation[/b][/size]

Install with MO2 or Vortex (or drop the files in manually), enable [b]SkySaveSystemUltra[/b], and launch through SKSE64. No in-game setup is needed.

Two DLLs are shipped because the engine's internal struct layout differs between SE 1.5.97 and AE 1.6.x and is baked in at compile time:
[list]
[*][code]SKSE\Plugins\SkySaveSystemUltra.dll[/code] — built for AE 1.6.x (default)
[*][code]SkySaveSystemUltra\SE\SkySaveSystemUltra.dll[/code] — built for SE 1.5.97
[/list]
If you run SE 1.5.97, copy [code]SkySaveSystemUltra\SE\SkySaveSystemUltra.dll[/code] (and its .pdb) into [code]SKSE\Plugins\[/code], replacing the AE copy. AE users leave the default in place.

Log: [code]Data\SKSE\Plugins\3SU\[/code] — the report and audit log there show exactly which saves were repaired and why.

[size=4][b]Known limitations[/b][/size]
[list]
[*]Repair guarantees the save loads; it cannot restore the items, quests, or NPCs the removed mod provided. Those are gone with the mod — no tool can bring them back.
[*]No backup is made. Replacement is atomic but the original .ess is overwritten. Back up your Saves folder before relying on this.
[*]After a repair or self-heal, save once and reload that save so the engine actually writes the cleaned changeforms.
[*]Only the "references missing mods" corruption class is handled. A genuinely truncated or overwritten .ess is outside its scope.
[*]SE 1.5.97 and AE 1.6.x are both supported via two shipped DLLs; VR is not. Pick the DLL matching your runtime (see Installation).
[/list]

[size=4][b]Credits[/b][/size]

Built with CommonLibSSE-NG. No third-party assets; the repair engine is original code. Crash logs and feedback welcome.

---

## 三、中文通用版

Sky Save System Ultra — 自动修复坏档

最常见、也最让人崩溃的坏档，是因为存档引用了你已经删掉的 MOD。引擎把存档插件表里的每个名字对一遍当前加载顺序，只要有一个对不上，后面的索引整体错位，存档就被判"已损坏"——常常连世界都进不去。

本模组在【文件层】、引擎读档之前就把这个问题解决。游戏启动、进主菜单之前，它扫一遍所有存档目录，把引用了缺失 MOD 的坏档静默修好，于是"以下内容不存在"那个确认框永远不会出现在载入菜单里。万一漏了，读档前的那一刻它还会再拦一次，当场修、当场重载。

· 启动预扫：进主菜单前修好所有坏档，载入菜单里不再弹确认框
· 读档前兜底：预扫漏掉的档，在你点选它的瞬间被拦下、修好、自动重载
· 文件层修复、不猜数据：缺失插件名在插件表里原位替身成等长当前插件名，整张表字节数不变，存档其余部分一个字节不动
· 原子替换：先写临时文件再 rename 覆盖，修复途中崩溃也不会留下半截 .ess
· 静默小工具：配套 exe 不弹窗、修完即退，不驻留后台，占用趋近于零
· 重载即清场：修复后，指向已删除 MOD 的残留 changeform 在下次载入时被引擎自动丢弃，存档干净回来
· 让"必须开新档"的 MOD 在旧档里初始化：存档写好后才加进来的插件，会重跑它开新档时走的 Start Game Enabled 任务，每条都有按存档绑定的收据，最多只跑一次
· 进世界后自愈：把指向已删 MOD 的悬空引用标记删除、清掉它们的收据、重写加载顺序基线，下次读档即干净
· 每次读档后只读体检：引用计数、悬空引用数、脚本实例数，两次存档间涨太多会打警告
· ESPless：无 esp、无 MCM、无 SkyUI，配置就是一个能手改的 .ini

兼容性：没有 MOD 白名单。修复只看"存档插件表 vs 引擎当前加载顺序"这一对，不管缺的是哪个 MOD，包括日后新出的。它不动你的加载顺序、不改别的 MOD、从不写入健康存档。本模组自己不带 ESP/ESL，不可能和你的排序冲突。

界面：修复本身没有界面，刻意隐形。体检结果以左上角一行提示 + 数据目录里的报告文件呈现。

配置：单个 SkySaveSystemUltra.ini，在 Data\SKSE\Plugins\ 下。开关覆盖"读档时修""修完提示""启动预扫""体检详细度""提示宽度"。全是纯文本，能手改，不写进存档。

性能：配套 exe 只在判定坏档时启动、修完即退，不残留进程。游戏内工作是事件驱动（每次读档一次），不是逐帧轮询；体检遍历单元是唯一有开销的步骤（重度排序约几十毫秒），可设上限或关掉。

前置：上古卷轴5 SE 1.5.97 或 AE 1.6.x（随包两份 DLL，见下）、SKSE64（对应你游戏版本）、Address Library for SKSE Plugins（CommonLibSSE-NG 构建需要，对应你游戏版本）。

已知限制：
· 修复只保证存档能载入，无法还原被删 MOD 提供的物品/任务/NPC——它们随 MOD 一起没了，任何工具都救不回。
· 不做备份。替换是原子的，但原 .ess 会被覆盖。依赖它之前请先手动备份 Saves 文件夹。
· 修复或自愈后，请先保存一次、再重载该存档，引擎才会真的把清理后的 changeform 写进 .ess。
· 只处理"引用了缺失 MOD"这一类坏档。文件被截断、被覆盖的真·损坏不在范围内。
· SE 1.5.97 与 AE 1.6.x 都支持（随包两份 DLL），VR 不支持。按你的游戏版本选对应 DLL（见兼容性/安装）。

---

## 四、中文平台变体

### 4.1 贴吧（首楼）

【原创】Sky Save System Ultra——删了 MOD 导致存档打不开？它自动修

身上一堆 MOD，手一抖禁用/删掉几个，再读档就弹"存档已损坏"或"以下内容不存在"，进不了世界？这就是存档插件表指向了已消失的 MOD，索引整体错位。

本模组在游戏启动、你点开载入菜单之前，就把这类坏档静默修好——不弹窗、不拦你、修完那个确认框根本不会出现。万一漏了，读档前还会再拦一次，当场修、当场重载。

功能
· 启动预扫，载入菜单前修好所有坏档
· 读档前兜底，漏网的当场修+自动重载
· 文件层原位替身，存档其余字节一个不动
· 原子替换，中途崩也不留半截档
· 配套 exe 修完即退，不占后台
· 让"必须开新档"的 MOD 在旧档里也能初始化
· 每次读档后只读体检，涨太多会提醒

兼容
没有 MOD 白名单，看的是"存档插件表 vs 当前加载顺序"，不管缺哪个 MOD 都不冲突；本模组不带 ESP，不可能和你的排序打架。

前置/安装
SE 1.5.97 或 AE 1.6.x、对应版本 SKSE64、Address Library。用 MO2/Vortex 装好启用即可，无需游戏内设置。
随包两份 DLL：SKSE\Plugins\ 下默认是 AE 版；SE 1.5.97 用户把 SkySaveSystemUltra\SE\ 里的 dll（含 pdb）复制到 SKSE\Plugins\ 覆盖即可。

已知问题
· 只保证能进，删掉 MOD 给的物品/任务回不来
· 不做备份，原档会被覆盖，先手动备 Saves
· SE 1.5.97 与 AE 1.6.x 均支持（两份 DLL），VR 不支持

下载见楼中楼，有问题回帖说，日志在 Data\SKSE\Plugins\3SU\。

### 4.2 3DM 论坛（Discuz）

[b][size=4]Sky Save System Ultra v2.0.0 —— 自动修复引用了已删除 MOD 的坏档[/size][/b]

[b]【说明】[/b]
最常见的坏档，是存档引用了你已经删除或禁用的 MOD，导致引擎索引错位、判定"已损坏"。本模组在文件层、引擎读档之前静默修好，不弹窗、不打断启动。

[b]【功能】[/b]
· 启动预扫：进主菜单前修好所有引用缺失 MOD 的坏档
· 读档前兜底：漏网的档当场修+自动重载
· 文件层原位替身，插件表字节数不变，其余数据不动
· 原子替换，中途崩溃不留半截档
· 配套 exe 修完即退，不驻留后台
· 让"必须开新档"的 MOD 在旧档里初始化
· 进世界后自愈 + 每次读档只读体检

[b]【兼容性】[/b]
没有 MOD 白名单，只看"存档插件表 vs 当前加载顺序"，与加载顺序无关、不带 ESP，不冲突。

[b]【前置】[/b]
· 上古卷轴5 SE 1.5.97 或 AE 1.6.x（随包两份 DLL，SE 用户需替换）
· SKSE64（对应游戏版本）
· Address Library for SKSE Plugins（对应游戏版本）

[b]【安装】[/b]
1. MO2/Vortex 安装并启用 SkySaveSystemUltra
2. 通过 SKSE64 启动游戏
3. 无需游戏内设置，配置在 SkySaveSystemUltra.ini 手改

[b]【已知问题】[/b]
· 只保证能载入，删掉 MOD 的物品/任务无法还原
· 不做备份，原 .ess 会被覆盖
· SE 1.5.97 与 AE 1.6.x 均支持（两份 DLL），VR 不支持

[b]【下载】[/b]
见附件。

### 4.3 B站 视频简介

Sky Save System Ultra｜删了MOD导致存档打不开？它自动修｜上古卷轴5 MOD

SE 1.5.97 或 AE 1.6.x / 对应版本 SKSE64 / Address Library，随包两份 DLL（SE 用户替换 SkySaveSystemUltra\SE\ 那份），ESPless 无设置，装完即用。

—— 时间轴 ——
00:00 问题演示：删 MOD 后读档弹"已损坏"
00:25 启动预扫：进菜单前已修好
00:50 读档前兜底 + 自动重载
01:20 文件层修复原理（原位替身/原子替换）
01:50 进世界后自愈 + 体检报告
02:15 安装与前置

—— 前置 ——
游戏 SE 1.5.97 或 AE 1.6.x、对应版本 SKSE64、Address Library（SE 用户记得把 SkySaveSystemUltra\SE\ 里的 dll 复制到 SKSE\Plugins\）

—— 下载 ——
见置顶评论（简介放链接会被限流）

—— 已知问题 ——
· 只保证能进，删 MOD 给的内容回不来
· 不做备份，先手动备 Saves

---

## 五、更新日志

[b]2.0.0[/b]
[list]
[*]Automatic file-level repair of saves referencing deleted/missing mods
[*]Startup pre-scan repairs all corrupted saves before the Load menu
[*]Pre-load fallback with silent helper .exe and auto-reload
[*]Load-time init for new-game-required mods; post-load self-healing and diagnostics
[*]ESPless: no ESP, MCM, or SkyUI; plain .ini configuration
[/list]
