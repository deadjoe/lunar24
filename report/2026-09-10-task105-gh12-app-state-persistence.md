# task #105 — GH#12: APP 一次启动恢复、设备重开配置保留与退出原子保存

- 分支 `feat/12-app-state-persistence`，基线 `727236e66cce33a70c4864f93a56884b20649bda`（隔离 worktree）。
- 授权：@Codex `b2af0b4`（thread `#Lunar24:7e247170`）。
- 纪律：不 merge / 不关 GH#12 / 不发布 / 不 MET；`--require-full` 原 12 缺口单列不动。

## §0 本文件先写判据（mandate 顺序：判据 → 实现 → 负控 → 门禁）

判据分三类，**每一条都可红**：

1. **C 系列**（行为，`tests/host/test_app_state_store.cpp`，框架无关：只依赖 core + `host/include/host/app_state_store.h`，可被普通 CTest 编译）——真实临时目录往返 + 真实 engine 候选路径。
2. **W15–W17**（结构，`tools/check_host_engine_wiring.py` 扩展）——真实 plugin / APP host 委托结构证据（iPlug2 虚函数不可单测，沿用 W1–W14 的静态钉法）。
3. **N 系列**（负控，`tools/run_app_state_negatives.py`，隔离影子 include 树，复用 `run_preset_engine_negatives.py` 的信任模型）。

## §1 判据（实现前锁定）

### C1 真实文件往返（真实临时目录 + 真实 FileOps）
- `C1.1 save_creates_exact_wire_size`：保存后 live 文件存在，长度**精确等于** `kDeviceStorageSchema.totalBytesHint`(6297)。
- `C1.2 saved_bytes_decode_to_saved_state`：读回文件字节 → decode → 与保存的 state 逐字段相同。**这条就是钉「后端谎报短写」的判据**：后端只写一半却返回 true 时，读回长度/内容都不对，本判据红。
- `C1.3 settings_ini_untouched`：同目录预置 `settings.ini` 哨兵内容，全程逐字节不变（产品文件 `lunar24-state.bin` 独立，不覆盖 ini）。
- `C1.4 temp_in_same_dir_unique`：临时文件与 live 同目录、名字非 live、且两次保存的临时名不同（多进程非碰撞）。
- `C1.5 load_round_trips_non_default_state`：非默认态（双侧键盘 / 4 槽 / cable / identity 非默认）保存 → 新实例读回 → 与保存态相同。
- `C1.6 identity_seed_from_file_not_constant`：文件里的 `identity_seed` 被原样恢复（不是 `kLunarStartupSeed`）。

### C2 一次读盘 / pending 传递（§2、§4）
- `C2.1 read_attempted_exactly_once`：N 次「停机边界序列」后 `readAttempts()==1`。
- `C2.2 no_reread_on_second_boundary`：第一次读盘后改写磁盘文件，第二次边界仍用**会话内**配置（不重读、不丢会话）。
- `C2.3 pending_survives_illegal_format`：非法格式（sr=0/NaN）使 `prepare()` 失败并释放 owner，pending 仍保留最后一次合法配置。
- `C2.4 next_legal_boundary_restores_retained`：随后一次合法边界把保留配置恢复为 canonical。
- `C2.5 device_reopen_keeps_last_committed_session`：引擎已提交 Y 后触发边界（prepare 会重建默认态）→ canonical 仍是 Y（既不回上电默认，也不回启动文件）。
- `C2.6 canonical_is_single_authority`：pending 成功发布后即清空，之后以 engine canonical 为唯一权威（无第二个可独立编辑的 bank）。

### C3 启动恢复
- `C3.1 fresh_instance_restores_file_state`：新 engine + 新 store：`prepare()` 先给默认态，随后 pending 经**真实 candidate** 发布 → canonical == 文件态且 ready。
- `C3.2 missing_file_boots_default_seed`：无文件 → `NoFile`，无 pending，canonical == `make_default_device_state(seed)`。
- `C3.3 missing_file_allows_lifecycle_save`：无文件时退出保存被允许（首次落盘）。

### C4 失败语义（原文件原样保留 + 安全启动 + 不许覆盖）
- `C4.1 truncated_is_length_mismatch`（100B）、`C4.2 oversized_is_length_mismatch`（6298B）。
- `C4.3a old_version_unsupported`（schemaVersion 4）、`C4.3b newer_version_requires_newer_codec`（6）。
- `C4.4 invalid_state_rejected`（`validate_device_state` 家族/字段可见）。
- `C4.5 valid_but_unexecutable_is_candidate_rejected`：state 通过校验但真实 candidate 拒绝（`RejectedGraph` fixture：`env_follower_env_out → effector_cv_x_in`，见 `tests/host/test_machine_cable_restore.cpp:365-368`）。
- `C4.6 unreadable_reported`：路径不可读（路径处是目录 / 权限拒绝）→ `Unreadable`，**不得**降级成「文件不存在」。
- `C4.7 failure_preserves_original_bytes`：C4.1–C4.6 全部原文件逐字节不变。
- `C4.8 failure_boots_default_and_ready`：失败仍以默认态安全启动并 ready。
- `C4.9 failure_blocks_lifecycle_save`：`save()` → `SkippedFileUnhealthy`，文件仍逐字节不变（退出保存不得覆盖坏文件）。
- `C4.10 no_path_reported`：未注入目录 → `NoPath`（不是 NoFile），`save()` → `SkippedNoPath`，且**不**落到 cwd / 其他目录。

### C5 原子保存失败矩阵（故障替身，隔离 + 先红）
- `C5.1 temp_write_failed_leaves_live`、`C5.2 flush_failed_leaves_live`、`C5.3 replace_failed_leaves_live`：live 保持上一次完整态且仍可解码；临时文件被丢弃。
- `C5.4 no_delete_before_replace`：replace 失败时 live 必须**仍在**（先删后换的突变在此红）。
- `C5.5 save_failure_not_swallowed`：失败必须以类型化状态上报（吞掉 `SaveResult` 的突变在此红）。

### C6 多实例
- `C6.1 interleaved_saves_never_expose_half_file`：两个 store 交替保存，live 始终是二者之一的完整态。
- `C6.2 temp_names_differ_across_instances`：两个实例的临时名不同（非碰撞，无锁服务）。

### C7 音频路径零 IO
- `C7.1 render_does_not_touch_store`：注入计数 FileOps，渲染 N 块后读写计数不变，且引擎正常 `Rendered`。

### C8 退出保存的**来源**（mandate §5 末句）
- `C8.1 retained_config_is_save_source`：无 owner（`canonicalState()==nullptr`）时，保存来源必须是**保留的最后合法配置**；写出的字节解回后等于该配置，且**不等于**上电默认。
- `C8.2 no_legal_config_means_no_write`：本次会话从未成功提交过合法配置 → `SkippedNoConfig`，**不写任何文件**（不得退回写默认态）。

### W15–W17 结构门（真实委托证据）
- `W15 OnReset 停机边界顺序`：`capture*` → `loadOnce` → `engine_.prepare(` → `publish*` 单调递增；`prepare()` 失败契约不变（仍只调 `engine_.prepare`）。
- `W16a 音频路径`：`processBlock` 不得引用 store、不得有任何文件 IO。
- `W16b 退出保存位置`：`~IPlugAPPHost` 体内 `saveDeviceState` 出现在 `CloseAudio();` **之后** —— **HELD**（见 §5）。
- `W17a 路径与文件纪律`：store 只写 `lunar24-state.bin`、不碰 `settings.ini`、不重解析目录、复用 core 的 codec/校验链与原子保存。
- `W17b APP host → plugin 的路径交接` —— **HELD**（见 §5）。

### N 系列负控（隔离源码，必须跑通并命中特定断言；编译失败不算红）
1. `skip_startup_apply` — 不做 pending 发布 → C3.1/C2.4 红。
2. `reread_every_boundary` — 去掉一次读盘 latch → C2.1/C2.2 红。
3. `drop_pending` — `capture*` 不保存 → C2.5/C2.3 红。
4. `bad_file_overwritten_on_exit` — 忽略保存闸门 → C4.9 红。
5. `short_write_lies` — 真实后端只写一半仍返回 true → C1.2 红。
6. `delete_old_file_first` — replace 前先删 live 且 replace 失败 → C5.4 红。
7. `save_result_swallowed` — 忽略 `SaveResult` → C5.5 红。
8. `save_writes_default_without_legal_config` — 无 owner 时保存源退回 `&pending_`（而非返回 `SkippedNoConfig`）→ C8.2 红。
9. `exit_call_missing` / `exit_call_before_closeaudio` — 结构突变 → W16b 红（静态门）—— **HELD**（见 §5）。

## §2 实现

### 2.1 文件集（worktree `wt-105-app-state`，分支 `feat/12-app-state-persistence`，基线 `727236e`）

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `host/include/host/app_state_store.h` | 新增（425 行） | 窄协调层：一次读盘 latch、pending 转移、保存来源选择、真实 FileOps |
| `tests/host/test_app_state_store.cpp` | 新增（~900 行，190 checks） | C0–C8 行为判据（真实临时目录 + 真实 engine 候选路径） |
| `tools/run_app_state_negatives.py` | 新增（~520 行） | N 系列隔离影子负控 driver |
| `tools/check_host_engine_wiring.py` | 修改 | +W15(8) / W16a(2) / W17a(9) = 19 条结构判据 |
| `host/plugin.h` / `host/plugin.cpp` | 修改 | `setStateDirectory` 交接孔 + `saveDeviceState` 退出保存孔 + `OnReset` 顺序 |
| `CMakeLists.txt` | 修改 | 注册 `test_app_state_store` + `app_state_store_negative`（UNIX） |
| `host/iPlug_app_host_override.cpp` | **未改** | 两处 hunk 被 drift 门钉死 → **HELD**（§5） |
| `tools/check_host_override_drift.py` | **未改** | 同上 |

### 2.2 设计要点（逐条对 mandate）

1. **路径**：store 不自己解析目录。`AppStateStore::setDirectory()` 只接受宿主**已解析**的 per-user 目录字符串（`host/plugin.cpp:119-125`）；目录为空时 `loadOnce()` → `NoPath`、`save()` → `SkippedNoPath`，**不落 cwd、不落其他目录**（C4.10）。产品文件 `lunar24-state.bin` 与 `settings.ini` 同目录但互不触碰（C1.3 / W17a）。临时文件同目录、名字含原子计数器 + `steady_clock` 计数（C1.4 / C6.2），无锁服务。
2. **一次读盘**：`readAttempts_` 显式 latch（**不是** `canonicalState()==nullptr`，W15 第 7 条静态钉）。长度门是**精确相等** `kAppStateWireBytes == kDeviceStorageSchema.totalBytesHint`(6297)，比 core 的 `>=` 更严；再走 `decode → migrate(仅当版本受支持) → validate_device_state`，产物进 `pending_`。缺文件 → `NoFile`（沿用既有常量 seed 默认态，C3.2）；有文件 → 身份按文件原样恢复（C1.6）。
3. **失败语义**：不可读 / 长度不符 / 版本过旧或过新 / 校验不过 / 校验过但候选不可执行，一律**原文件逐字节保留**（C4.7）、**默认态安全启动且 ready**（C4.8）、**本次退出自动保存不得覆盖该文件**（C4.9，`save()` → `SkippedFileUnhealthy`）。无 UI 修复入口、无覆盖确认流、无自动修复。
4. **停机边界**：`OnReset()` 内严格四步（`host/plugin.cpp:110-116`）——`captureCanonical()`（**先于** `prepare()`，防止设备重开回退上电默认或重读磁盘）→ `loadOnce()` → `engine_.prepare(...)`（原 GH#4 8B2 owner 构建，契约未改）→ 仅当 `engine_.isReady()` 才 `publishPending(...)`。非法格式使 `prepare()` 失败时 pending 保留，供**下一次**合法边界恢复（C2.3/C2.4）。发布成功即清空 pending，此后以 engine canonical 为唯一权威，不存在第二个可独立编辑的 bank（C2.6）。`prepare()` 的默认态构建不会覆盖 pending 恢复（W15 第 3 条）。
5. **退出保存**：`LunarHostPlugin::saveDeviceState()`（`host/plugin.cpp:127-134`）纯委托给 store；来源优先级 = `engine.canonicalState()` → 无则保留的最后合法配置 `pending_` → 都没有则 `SkippedNoConfig`（**不写**，C8.2）。文件逻辑全在窄协调层，**不在析构体、不在音频回调**。退出保存**仅算 lifecycle save**，不是实时防抖 / 崩溃恢复 / 面板自动保存；类型化 `StateSaveOutcome` 保留失败诊断；回调内无日志。
6. **真实 FileOps**：`realWriteFile` / `realFlushFile` / `realAtomicReplace` / `realDiscardFile`。原子替换走同目录 `rename`（POSIX）/ `MoveFileEx`（Win）语义；失败保留旧文件、临时文件 best-effort 清理、**不先删旧文件**（C5.4，见 §2.3）；保持"非崩溃安全"边界，不引入校验和、不声称检测所有位翻转。后端谎报短写 = **接口违约**（`writeFile` 契约"Return true only on a COMPLETE write"），负控 `short_write_lies` 钉的是真实后端的返回逻辑。

### 2.3 过程中发现并修掉的两处判据弱点（先红后修）

- **C5.4 判据过弱（被 `delete_old_file_first` 负控暴露）**：原判据只查 `exists(livePath)`。突变"先删目标再 rename"在测试里**成功**（目标是个目录时 rename 仍可完成），于是路径上确实"有东西"，弱判据假绿。改为 `is_directory(livePath)` 并加注释：必须**同一个目标对象**存活，而不是"路径上存在任意东西"。
- **mandate §5 末句缺判据**：新增 C8 两条 + 第 8 条负控。突变选择 `source = &pending_` 而非 `make_default_device_state(...)`——store 头文件不 include `state_default.h`，写默认态会**编译失败**，而编译失败按 driver 信任模型**不算红**，所以选了能编译且语义正确的突变。

## §3 门禁输出（最终文件集）

| 门 | 命令 | 结果 |
| --- | --- | --- |
| 结构门 | `python3 tools/check_host_engine_wiring.py` | rc=0，**82/82 PASS**（含新 W15 8 + W16a 2 + W17a 9 = 19） |
| override drift | `python3 tools/check_host_override_drift.py` | rc=0，**11/11 PASS**（未改 `host/iPlug_app_host_override.cpp`） |
| 行为验收 Release | `./build-rel/test_app_state_store` | **190 checks OK** |
| 行为验收 Debug+ASan+UBSan | `./build-debug/test_app_state_store` | **190 checks OK** |
| 负控 driver | `python3 tools/run_app_state_negatives.py` | rc=0，**OVERALL: PASS**（12/12 自检 + 保留性正控绿 + 8/8 可跑负控命中，2 HELD） |
| Release 快门 | `ctest --label-exclude slow -j4`（build-rel） | **74/74**（最终文件集，含 `test_app_state_store` 190 OK + `app_state_store_negative`） |
| Debug+ASan+UBSan 快门 | `ctest --label-exclude slow`（build-debug） | 74/74（04:40，见 §3.1） |
| Release slow | `ctest -j4 -L slow`（build-rel） | **7/7 PASS** rc=0（897.65 sec*proc / 394.72s real） |
| Debug+ASan+UBSan slow | `ctest -j4 -L slow`（build-debug） | 进行中（后台） |

### 3.1 时序说明（避免误读）

- Release 快门已在**最终文件集**上整跑：74/74，含 `test_app_state_store`（190 checks OK）与 `app_state_store_negative`（OVERALL PASS）。
- Debug+ASan+UBSan 快门 74/74 是在 **C8 加入之前**跑的（04:40 结束，二进制重建于 04:41）；C8 之后唯一变化的文件就是 `tests/host/test_app_state_store.cpp`，该目标已在 Debug+ASan+UBSan 下单独重跑 **190 checks OK**；待 Debug slow 跑完后在最终文件集上整跑一遍，结果补进本节，不单独再提报告。
- Release slow 已完：**7/7 PASS**（gh19_alias_probe / gh19_blamp_acceptance / gh20_vcf_probe / gh20_vcf_acceptance / gh12_keyboard_owner_probe / gh12_keyboard_side_restore_probe / test_d3_divider_restore）。
- `--require-full` 原 **12 项缺口单列不动**，未列入本卡门禁。

## §4 负控证据（隔离影子源码；编译失败不算红）

信任模型与 `run_preset_engine_negatives.py` 同构：影子 include 树 `-I` 顺序覆盖真实头文件，**在内存里改真实文本**，编译真实验收测试；judge 要求 `rc 恰为 1`、终止摘要完整、must-hold 标签必须真打印 PASS、期望 RED 标签必须真打印 FAIL、先跑 `--self-check`。

| 负控 | 突变 | 命中判据 |
| --- | --- | --- |
| `skip_startup_apply` | 不做 pending 发布 | C3.1（邻带 C2.6） |
| `reread_every_boundary` | 去掉一次读盘 latch | C2.2（邻带 C2.1） |
| `drop_pending` | `captureCanonical` 不保存 | C2.5（邻带 C2.3） |
| `bad_file_overwritten_on_exit` | 忽略保存闸门 | C4.9（邻带 C4.7） |
| `short_write_lies` | 真实后端只写一半仍返回 true | C1.2（邻带 C1.1） |
| `delete_old_file_first` | replace 前先删 live | C5.4（邻带 C5.3） |
| `save_result_swallowed` | 忽略 `SaveResult` | C5.5（邻带 C5.1） |
| `save_writes_default_without_legal_config` | 无 owner 时退回 `&pending_` | C8.2（邻带 C8.1） |
| `exit_call_missing` / `exit_call_before_closeaudio` | 结构突变 | W16b —— **HELD** |

**HELD 原因**：两者都必须改 `host/iPlug_app_host_override.cpp`，而该文件整份 upstream→fork 差分被 `tools/check_host_override_drift.py` 的 `PIN` + `EXPECTED_DIFF_HASH["host"]` 逐字节钉死，该门自己的注释把"重新生成"保留给 @Codex 对 curated hunk 的裁决（已在 msg `1bc54368` 报 `file:line` 并给两个选项，未获裁决）。按 mandate 的冲突规则：**只暂停相应部分**，其余继续。

## §5 HELD：退出接线（`file:line` + 所需裁决）

### 5.1 被阻塞的两处 hunk（均未落盘）

1. **路径交接** —— `host/iPlug_app_host_override.cpp:139` `InitState()`，目录在 `:144`(Win) / `:146`(mac) 解析完成后、`:151` `struct stat st;` 之前插入：
   ```cpp
   static_cast<LunarHostPlugin*>(GetPlug())->setStateDirectory(mINIPath.Get());
   ```
   必须在此处（`mINIPath.Append("settings.ini")` 于 `:155`/`:194`/`:203` 之前），否则拿到的是 ini 全路径而非目录。强制转换写法与 `:70` / `:732` 既有模式一致。
2. **退出保存** —— `host/iPlug_app_host_override.cpp:89` `~IPlugAPPHost()`，`:93` `CloseAudio();` 之后、`:95` midi 清理之前插入：
   ```cpp
   static_cast<LunarHostPlugin*>(GetPlug())->saveDeviceState();
   ```
   `mExiting = true;` 在 `:91`，早于 `:93`，满足 mandate 的"`CloseAudio()` 返回后、成员析构前"。

### 5.2 解除 HELD 需要 @Codex 裁决的机械项

- `tools/check_host_override_drift.py:74-78` `ALLOWED["host"]` 增加两个函数名：`IPlugAPPHost::~IPlugAPPHost`、`IPlugAPPHost::InitState`。
- `tools/check_host_override_drift.py:64-67` `EXPECTED_DIFF_HASH["host"]` 重新生成（当前 `8e46b85646917044c8e8f16bdfff8aee87becdaf04f403cec09defeefa81d36f`，本次未动）。
- `tools/check_host_engine_wiring.py` 增加 W16b（析构体内 `CloseAudio()` 之后）与 W17b（APP host 交接调用点）。
- `tools/run_app_state_negatives.py` 把 2 条 HELD 负控从 HELD 移入可跑集合。

**未做**：未改 `host/iPlug_app_host_override.cpp`、未改 `tools/check_host_override_drift.py`、未自行 merge / 关 GH#12 / 发布 / MET。

