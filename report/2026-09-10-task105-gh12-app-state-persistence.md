# task #105 — GH#12: APP 一次启动恢复、设备重开配置保留与退出原子保存

- 分支 `feat/12-app-state-persistence`，基线 `727236e66cce33a70c4864f93a56884b20649bda`（隔离 worktree）。
- 授权：@Codex `b2af0b4`（thread `#Lunar24:7e247170`）；drift 白名单与两处 host 接线由 @Codex `97d9f1a2` 授权 A；三项修订（粘性保护 / 先拒后分配 / 读边界+排他临时名）由 @Codex `0c9ea88d` + `d5f6a520` 指派。
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
- `W16b 退出保存位置`：`~IPlugAPPHost` 体内 `saveDeviceState` 出现在 `CloseAudio();` **之后**、成员清理之前；调用是 `LunarHostPlugin` 委托；析构体内零文件 IO；全文件 `saveDeviceState` **恰好一次**。
- `W17a 路径与文件纪律`：store 只写 `lunar24-state.bin`、不碰 `settings.ini`、不重解析目录、复用 core 的 codec/校验链与原子保存。
- `W17b APP host → plugin 的路径交接`：`InitState()` 内 `setStateDirectory(mINIPath.Get())` 出现在平台目录解析 `SetFormatted(` **之后**、`Append("settings.ini")` **之前**；全文件恰好一次；host 从不出现 `lunar24-state.bin`（文件名归 store 所有）。

### N 系列负控（隔离源码，必须跑通并命中特定断言；编译失败不算红）
1. `skip_startup_apply` — 不做 pending 发布 → C3.1/C2.4 红。
2. `reread_every_boundary` — 去掉一次读盘 latch → C2.1/C2.2 红。
3. `drop_pending` — `capture*` 不保存 → C2.5/C2.3 红。
4. `bad_file_overwritten_on_exit` — 忽略保存闸门 → C4.9 红。
5. `short_write_lies` — 真实后端只写一半仍返回 true → C1.2 红。
6. `delete_old_file_first` — replace 前先删 live 且 replace 失败 → C5.4 红。
7. `save_result_swallowed` — 忽略 `SaveResult` → C5.5 红。
8. `save_writes_default_without_legal_config` — 无 owner 时保存源退回 `&pending_`（而非返回 `SkippedNoConfig`）→ C8.2 红。
9. `save_gate_reopened_by_session_publish` — 粘性采纳判决换成「任何 Accepted 发布都重开闸门」→ C4.12 红（@Codex `0c9ea88d` 实际重现的洞）。
10. `oversized_allocated_before_size_gate` — 关掉 fstat 尺寸门 → C4.13「NOTHING was read」红。
11. `trailing_byte_accepted` — 读边界丢掉越界字节探测 → C4.14 红。
12. `temp_reserve_not_exclusive` — 排他创建退化为普通创建 → C6.3 红。
13. `exit_call_missing` — 删掉 `~IPlugAPPHost` 的退出保存调用 → W16b 4 条红（委托/顺序/位置/恰好一次）。
14. `exit_call_before_closeaudio` — 把保存调用移到 `CloseAudio();` 之前 → W16b 顺序条红（**恰 1 条**，其余 5 条仍绿）。

### C4.11–C4.14 / C6.3–C6.5（@Codex `0c9ea88d` + `d5f6a520` 三项修订的判据）
- `C4.11 sticky_adoption_verdict`：合法但图不可执行的文件被拒后，闸门关闭且 `fileUnadopted()` 为真。
- `C4.12 protection_survives_device_reopen`：拒绝后做 **1 次与 3 次**设备重开（每次 `prepare` 都会重建默认态并作为 `FromSession` **成功发布**），闸门**不得**重开；退出保存 → `SkippedFileUnhealthy`，原文件**逐字节保留**。
- `C4.13 oversized_refused_before_allocation`：64 MiB 文件 → `LengthMismatch` 且 `bytesRead()==0`（尺寸门先于任何读取/分配）。
- `C4.14 read_boundary_exactness`：精确长度接受并读满 `kWire`；**多一个尾字节**拒绝；短读拒绝。
- `C6.3 exclusive_temp_claim`：已存在路径的排他创建被拒；新路径成功且真的创建了文件。
- `C6.4 reserved_names_distinct`：两次预留名字不同、各自真实存在于 live 同目录。
- `C6.5 reservation_spans_write`：后端被要求写入时，临时路径**已是我们排他占有的空文件**；清理只删这一条路径。

## §2 实现

### 2.1 文件集（worktree `wt-105-app-state`，分支 `feat/12-app-state-persistence`，基线 `727236e`）

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `host/include/host/app_state_store.h` | 新增 | 窄协调层：一次读盘 latch、pending 转移、保存来源选择、粘性采纳判决、排他临时名、真实 FileOps |
| `tests/host/test_app_state_store.cpp` | 新增（251 checks） | C0–C8 行为判据（真实临时目录 + 真实 engine 候选路径） |
| `tools/run_app_state_negatives.py` | 新增 | N 系列隔离影子负控 driver（12 条验收突变 + 2 条结构突变） |
| `tools/check_host_engine_wiring.py` | 修改 | +W15(8) / W16a(2) / W17a(9) / W16b(6) / W17b(6) = 31 条结构判据（合计 94） |
| `host/plugin.h` / `host/plugin.cpp` | 修改 | `setStateDirectory` 交接孔 + `saveDeviceState` 退出保存孔 + `OnReset` 顺序 |
| `CMakeLists.txt` | 修改 | 注册 `test_app_state_store` + `app_state_store_negative`（UNIX） |
| `host/iPlug_app_host_override.cpp` | 修改（+14 行，纯插入） | 两处生命周期 hunk（§5，@Codex `97d9f1a2` 授权 A） |
| `tools/check_host_override_drift.py` | 修改 | `ALLOWED["host"]` +2 函数名；`EXPECTED_DIFF_HASH["host"]` 重生成（PIN 与其余限制不动） |

### 2.2 设计要点（逐条对 mandate）

1. **路径**：store 不自己解析目录。`AppStateStore::setDirectory()` 只接受宿主**已解析**的 per-user 目录字符串（`host/plugin.cpp:119-125`）；目录为空时 `loadOnce()` → `NoPath`、`save()` → `SkippedNoPath`，**不落 cwd、不落其他目录**（C4.10）。产品文件 `lunar24-state.bin` 与 `settings.ini` 同目录但互不触碰（C1.3 / W17a）。临时文件同目录、名字含原子计数器 + `steady_clock` 计数（C1.4 / C6.2），无锁服务。
2. **一次读盘**：`readAttempts_` 显式 latch（**不是** `canonicalState()==nullptr`，W15 第 7 条静态钉）。长度门是**精确相等** `kAppStateWireBytes == kDeviceStorageSchema.totalBytesHint`(6297)，比 core 的 `>=` 更严；再走 `decode → migrate(仅当版本受支持) → validate_device_state`，产物进 `pending_`。缺文件 → `NoFile`（沿用既有常量 seed 默认态，C3.2）；有文件 → 身份按文件原样恢复（C1.6）。
3. **失败语义**：不可读 / 长度不符 / 版本过旧或过新 / 校验不过 / 校验过但候选不可执行，一律**原文件逐字节保留**（C4.7）、**默认态安全启动且 ready**（C4.8）、**本次退出自动保存不得覆盖该文件**（C4.9，`save()` → `SkippedFileUnhealthy`）。无 UI 修复入口、无覆盖确认流、无自动修复。**该保护是粘性的**：`fileUnadopted_` 单调，只有「`FromFile` 的发布被 Accepted」才可能解除；设备重开时 `prepare()` 重建默认态并作为 `FromSession` **成功发布**，**不得**重开闸门（C4.11/C4.12，见 §2.3）。
4. **停机边界**：`OnReset()` 内严格四步（`host/plugin.cpp:110-116`）——`captureCanonical()`（**先于** `prepare()`，防止设备重开回退上电默认或重读磁盘）→ `loadOnce()` → `engine_.prepare(...)`（原 GH#4 8B2 owner 构建，契约未改）→ 仅当 `engine_.isReady()` 才 `publishPending(...)`。非法格式使 `prepare()` 失败时 pending 保留，供**下一次**合法边界恢复（C2.3/C2.4）。发布成功即清空 pending，此后以 engine canonical 为唯一权威，不存在第二个可独立编辑的 bank（C2.6）。`prepare()` 的默认态构建不会覆盖 pending 恢复（W15 第 3 条）。
5. **退出保存**：`LunarHostPlugin::saveDeviceState()`（`host/plugin.cpp:127-134`）纯委托给 store；来源优先级 = `engine.canonicalState()` → 无则保留的最后合法配置 `pending_` → 都没有则 `SkippedNoConfig`（**不写**，C8.2）。文件逻辑全在窄协调层，**不在析构体、不在音频回调**。宿主侧唯一调用点 = `~IPlugAPPHost` 内 `CloseAudio();` 返回之后、成员清理之前（`host/iPlug_app_host_override.cpp:100`），`mIPlug` 先声明后析构故插件仍存活；目录交接点 = `InitState()` 内平台解析之后、`Append("settings.ini")` 之前（`:163`）。退出保存**仅算 lifecycle save**，不是实时防抖 / 崩溃恢复 / 面板自动保存；类型化 `StateSaveOutcome` 保留失败诊断；回调内无日志。
6. **真实 FileOps**：`realWriteFile` / `realFlushFile` / `realAtomicReplace` / `realDiscardFile`。原子替换走同目录 `rename`（POSIX）/ `MoveFileEx`（Win）语义；失败保留旧文件、临时文件 best-effort 清理、**不先删旧文件**（C5.4，见 §2.4）；保持"非崩溃安全"边界，不引入校验和、不声称检测所有位翻转。后端谎报短写 = **接口违约**（`writeFile` 契约"Return true only on a COMPLETE write"），负控 `short_write_lies` 钉的是真实后端的返回逻辑。**临时名由排他创建占有**（`O_CREAT|O_EXCL` / `_sopen_s + _O_SH_DENYRW`），预留**贯穿写入**（`realWriteFile` 只 `"r+b"` 打开、绝不创建），清理只针对本次成功取得的那一条路径（C6.3/C6.4/C6.5）。

### 2.3 三项修订（@Codex `0c9ea88d` + `d5f6a520`，全部落盘）

1. **粘性采纳判决（修的是 @Codex 实际重现的覆盖漏洞）**。原实现里「Accepted → `saveAllowed_ = true`」对所有来源一视同仁。@Codex 的重现：合法但图不可执行的文件首次被拒 → 闸门关；再按 plugin 顺序做一次设备重开，默认配置作为 `FromSession` 成功发布 → 闸门被重新打开 → 退出保存覆盖原文件（实跑 `original preserved=0`）。修法：Accepted 分支只在 `origin == PendingOrigin::FromFile && !fileUnadopted_` 时开闸；所有「有文件但未采纳」的路径都置 `fileUnadopted_ = true`（单调，会话内不可清除）。判据 C4.11（拒绝即关闸）+ C4.12（**1 次与 3 次**重开后仍关闸、原文件逐字节保留）+ 负控 `save_gate_reopened_by_session_publish`。
2. **先拒绝再分配**。原 `loadOnce` 用 `ftell` + `resize` 按任意长度整文件分配，超长文件会在 typed failure 之前耗尽内存。改为对**已打开描述符** `fstat`/`_fstat64`：非普通文件（例如目录）→ `Unreadable`；`st_size != kWire` → `LengthMismatch`，**先于任何分配**。判据 C4.13（64 MiB → `LengthMismatch` 且 `bytesRead()==0`）+ 负控 `oversized_allocated_before_size_gate`。
3. **读边界钉死 + 排他临时名**。`d5f6a520` 追加两点：①路径上的 `file_size` 不是已打开文件的稳定快照 ⇒ 新增 `readExactRecord`：始终只读 `kWire` 字节，**短读拒绝**，并 `fgetc` 探测**越界尾字节**（文件在尺寸门之后变大时不得当作完整记录），`ferror` 区分 IO 错误；②排他预留必须**贯穿实际写入**（不能预留后删除再普通创建）⇒ `reserveTempPath()` 用 `O_CREAT|O_EXCL` 循环取唯一名，`realWriteFile` 只以 `"r+b"` 打开并 truncate，清理只删本次成功取得的那条路径。判据 C4.14 + C6.3/C6.4/C6.5 + 负控 `trailing_byte_accepted` / `temp_reserve_not_exclusive`。

### 2.4 过程中发现并修掉的两处判据弱点（先红后修）

- **C5.4 判据过弱（被 `delete_old_file_first` 负控暴露）**：原判据只查 `exists(livePath)`。突变"先删目标再 rename"在测试里**成功**（目标是个目录时 rename 仍可完成），于是路径上确实"有东西"，弱判据假绿。改为 `is_directory(livePath)` 并加注释：必须**同一个目标对象**存活，而不是"路径上存在任意东西"。
- **mandate §5 末句缺判据**：新增 C8 两条 + 第 8 条负控。突变选择 `source = &pending_` 而非 `make_default_device_state(...)`——store 头文件不 include `state_default.h`，写默认态会**编译失败**，而编译失败按 driver 信任模型**不算红**，所以选了能编译且语义正确的突变。
- **C4.13 夹具自身先红（测试 bug，不是产品 bug）**：`std::filesystem::resize_file` 要求目标**已存在**（底层是 `truncate(2)`，否则 ENOENT），直接 resize 得到「没有文件」→ `loadOnce` 返回 `NoFile`，三条判据全红而原因与产品无关。修法：先 `writeBytes(live, {})` 建文件再 resize。记录在案：**夹具失败必须先分清是产品红还是夹具红**。
- **突变锚点漂移被 driver 当场拦下**：本轮给 `saveImpl` 插入排他预留与清理块后，`save_result_swallowed` 的锚点匹配数变 0，driver 立即 `SystemExit` 并打印「production source drifted」。这正是 `_replace` 的设计意图——锚点漂移绝不允许静默退化成「无突变的假绿」。已更新锚点并加一次性 pre-flight 全锚点自检。

## §3 门禁输出（最终文件集）

| 门 | 命令 | 结果 |
| --- | --- | --- |
| 结构门 | `python3 tools/check_host_engine_wiring.py` | rc=0，**94/94 PASS**（含 W15 8 + W16a 2 + W17a 9 + W16b 6 + W17b 6 = 31） |
| override drift | `python3 tools/check_host_override_drift.py` | rc=0，**11/11 PASS**（`host` 哈希重生成 `65d1ba34…`，PIN 不动；诊断行仍打印 `note … IPlugAPPHost::IPlugAPPHost`，见 §5.3） |
| 行为验收 Release | `./build-rel/test_app_state_store` | **251 checks OK** |
| 行为验收 Debug+ASan+UBSan | `./build-debug/test_app_state_store` | **251 checks OK** |
| 负控 driver | `python3 tools/run_app_state_negatives.py --require-all` | rc=0，**OVERALL: PASS**（24/24 judge 自检 + 保留性正控绿 + 结构正控绿 + **12/12** 验收突变命中 + 2/2 结构突变命中） |
| Release 快门 | `ctest --label-exclude slow -j4`（build-rel） | **74/74 PASS** rc=0（25.99s real；含 `test_app_state_store` 251 OK + `app_state_store_negative`） |
| Debug+ASan+UBSan 快门 | `ctest --label-exclude slow`（build-debug） | 见 §3.1（待 Debug slow 释放 build-debug 后整跑） |
| Release slow | `ctest -j4 -L slow`（build-rel） | **7/7 PASS** rc=0（390.96s real，最终文件集） |
| Debug+ASan+UBSan slow | `ctest -j4 -L slow`（build-debug） | 进行中（04:40 启动，约 2.9h；见 §3.1） |

### 3.1 时序说明（避免误读）

- **Release 快门已在最终文件集上整跑**：74/74，含 `test_app_state_store`（251 checks OK）与 `app_state_store_negative`（OVERALL PASS）。
- **两个验收目标都在最终文件集上单跑过**：Release **251 OK**、Debug+ASan+UBSan **251 OK**。
- **Debug+ASan+UBSan 快门（74 条）与 slow 未在最终文件集上整跑**：`build-debug` 自 04:40 起被 slow 套件占用，且其 ctest 日志与并发 ctest 冲突；按 @Codex「不用重复已通过的无关全套」，本轮不为 app-state 改动重跑无关 slow。该慢套件涉及的 7 个目标均不消费本轮改动文件（`app_state_store.h` 只被 `test_app_state_store` 与 APP host 目标引用）。待 slow 结束后在最终文件集整跑 Debug 快门，结果补进本节，不单独再提报告。
- **Release slow 已按最终文件集重跑：7/7 PASS**（`gh12_keyboard_side_restore_probe` / `gh19_blamp_acceptance` / `gh20_vcf_probe` / `gh20_vcf_acceptance` 等 7 条，rc=0，390.96s）。本轮修订改动了 `app_state_store.h`，故不沿用修订前那次结果。
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
| `save_gate_reopened_by_session_publish` | 粘性采纳判决换成「任何 Accepted 发布都重开闸门」 | C4.12（邻带 C4.12 字节保留 + C4.11 退出保存） |
| `oversized_allocated_before_size_gate` | 关掉 fstat 尺寸门 | C4.13「NOTHING was read」 |
| `trailing_byte_accepted` | 读边界丢掉越界字节探测 | C4.14「a byte BEYOND the record is rejected」 |
| `temp_reserve_not_exclusive` | 临时名仍创建但不再排他 | C6.3「refuses a path another owner already holds」 |
| `exit_call_missing` / `exit_call_before_closeaudio` | 结构突变（影子 repo + 真实门副本） | W16b（4 条 / 1 条） |

`exit_call_missing` 命中 4 条 W16b（委托/顺序/位置/恰好一次），`exit_call_before_closeaudio` 命中 1 条（顺序），其余 5 条仍绿——特异性证据见 driver 的 `STRUCTURAL[...]["must_pass"]`。

## §5 退出接线：已落盘（@Codex `97d9f1a2` 授权 A）

### 5.1 两处 hunk（均已落盘，`+14` 行纯插入，无删除/无改写）

1. **路径交接** —— `host/iPlug_app_host_override.cpp:163` `InitState()`，平台目录解析完成后、`:169` `mINIPath.Append("settings.ini")` 之前：
   ```cpp
   static_cast<LunarHostPlugin*>(GetPlug())->setStateDirectory(mINIPath.Get());
   ```
   必须在此处，否则拿到的是 ini 全路径而非目录。强制转换写法与既有 `:70` / `:732` 模式一致。W17b 六条不变式（`:159-163` 注释即为其可读化说明）。
2. **退出保存** —— `host/iPlug_app_host_override.cpp:100` `~IPlugAPPHost()`，`:93` `CloseAudio();` 之后、成员清理之前：
   ```cpp
   static_cast<LunarHostPlugin*>(GetPlug())->saveDeviceState();
   ```
   `mExiting = true;` 在 `:91`，早于 `:93`，满足 mandate「`CloseAudio()` 返回后、成员析构前」。

### 5.2 配套门禁改动（授权范围内）

- `tools/check_host_override_drift.py`：`ALLOWED["host"]` +2 函数名（`IPlugAPPHost::~IPlugAPPHost`、`IPlugAPPHost::InitState`）；`EXPECTED_DIFF_HASH["host"]` 重生成 → `65d1ba34e6aad7b5fd89d579167f0a23408662319a315929ec6231f068ce583e`。**PIN 与其他限制未动**，未放宽为忽略任意差异。
- `tools/check_host_engine_wiring.py`：新增 W16b 6 条 + W17b 6 条。
- `tools/run_app_state_negatives.py`：两条结构负控从 HELD 移入可跑集合，用影子 repo（真实门副本 + symlink farm）判定，**从不编辑产品文件**。

### 5.3 非门禁诊断行（已知，不影响 PASS）

drift 门仍打印 `note … non-curated anchors in diff: IPlugAPPHost::IPlugAPPHost`：门的 `SIG_RE` 匹配不到析构函数签名（`~`），属诊断行而非判据，11/11 仍 PASS。未改门逻辑（超出本次授权）。


## §6 交付状态

- **候选 = 未推送 head**（本轮修订 commit 见交付消息）；分支 `feat/12-app-state-persistence`。
- 等 @Codex 复验：**未 push、未开/更新 PR、未跑 CI、未 merge、未关 GH#12、未发布、未 MET**。
- 后继项（需总监开卡）：其余消费者、`UnitIdentitySeed`「每安装首次生成」缺口、`save_state_atomic` 长度/回读校验。
