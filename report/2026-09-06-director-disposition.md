<!-- SPDX-License-Identifier: Apache-2.0 -->
# 2026-09-06 工程复核与实施安排

作者：Codex；输入为 owner 发起报告及 566d701 ERRATA E1–E7/N-8。核查代码基线566d701，产品代码与4a4c7b9一致；随后阶段合并34ece0df43d0aebd8732e20770b3cdbc96b5c8b2。本文区分复测、源码事实、工程裁决。未听音、未测真实音频设备、未比较真机，不声明音色准确度。

## 分支处理结果

PR #17已合入main，SHA34ece0df43d0aebd8732e20770b3cdbc96b5c8b2。合并前exacthead566d701的四平台构建/测试及macOS/Windows实际host产物均绿（run33981054050）；full-coverage只因8结构性+4无值域缺口失败。Owner在Raft293e25dc明确批准本次阶段合并豁免；没有删除gate、扁平化结构或编造值域，豁免不扩展到以后PR或发布。

两条review分支的10个独有提交、18份文档先合入，再删除本地/远端旧foundation、两条review和旧full-registry分支。旧审查工作树切到detached以保留可能有用的本地证据；无效worktree登记已prune。共享checkout返回clean main==origin/main。Pi新工作在独立fix/18-vco-normal-source工作树，不混入阶段PR。

## 逐项核查

| 报告项 | 复核结论 | 实施裁决 |
|---|---|---|
| N-1 默认DRY B锁死 | [复测] 原探针sample1530开始50ms平坦，末秒−1V；cvAmt0.5/0.2/0.05/0存活。[源码/手册] RouteId4自源与手册411–414 A→B矛盾 | GH #18、task #83先修。改正常连接来源，不改cvAmt默认或线性公式掩盖症状 |
| N-2 混叠 | [复测] 220Hz saw probe RMS−27.7dB、worst−45.8dB；1864.66Hz RMS−19.3dB、worst−27.6dB。它是孤立Vco，不是产品波形选择全路径 | GH #19：先质量/时序/CPU与测量契约，再分模块实现；不把report的−60dB建议直接宣布为全域硬指标 |
| N-3 截止帽/跨率差 | [复测] norm1/LP/res0/8kHz，44.1k→0.4109，96k→0.6319，约3.74dB。44.1k norm>0.813不再升截止 | GH #20确认修复；参考响应、误差带、旋钮单调性和高共振稳定性先定。TPT候选不等于已证明整段频响相同 |
| N-4 SVF/反馈非线性 | [源码] 当前Chamberlin+输入tanh属实；新版Solar反馈电路位置/自激没有本项目直接证据 | 与GH #20关联调查，不冒称硬件拓扑已知，不为满足错误THD判据造反馈非线性或自激 |
| N-5 平滑 | [源码] product连续参数直接赋值，JoystickCv中的smoother字样是注释，不是执行消费者 | GH #21；实时旋钮/CC平滑与离散edge/audio-rate CV严格分开，state restore初始化不能缓慢滑向目标 |
| N-6 输入数组 | [源码] processBlock按inputs[i]访问，必须有n帧，输出亦为n帧。报告承认原probe调用错误 | 随task #83补文档即可；裸指针不能靠assert检查真实分配长度，不为此换接口/引入C++20 |
| N-7 旧注释 | [源码] runtime约2101仍说六源未集成，与实际execution disposition冲突 | task #83相关注释更新，保留keyboard/voices/effector实际未集成边界 |
| N-8 sub | [源码] 2*subPhase−1是saw，注释square错误 | 注释修正，不将纠正文案变成未经证实的音色行为修改 |

### 对报告测法/根因的保留

1. 使用owner ERRATA后的混叠口径；旧−54dB作废。频率集合必须检查真实折叠像，测量结果不能由"sr/f0是否整数"一句话替代。有限窗、重复折叠频率/相邻不可分辨谱线及谐波重合会影响聚合值；建立验收前需去重/参考对照。波形枚举存在不表示当前产品控制路径可选择它，当前setMorph与setWaveform不能混称。
2. N-1的修复保留线性1+cvEff，故"回退成该线性公式必红"不能当负控；应恢复错误B自源。刻意静音或用户显式自反馈导致DC不属于默认路由存活判据。四路全域必须有声也不是产品契约。
3. N-3的预畸变只对指定映射提供对齐条件，不能直接推出所有probe跨率误差≤0.5dB。阈值要有通带/阻带、参考响应和噪声底口径，不能由算法名称背书。
4. N-4"纯输入非线性下THD不随res变化"不成立：输出第k谐波/基波包含H(kf,res)/H(f,res)，后级线性滤波器也能随res改变该比值。该断言不能识别非线性在反馈中的位置。设计07要求新版Polivoks行为，不授权直接照搬经典电路。
5. N-5输出一阶差分比值只能作特定激励的补充，不能替代秒数轨迹、sampleoffset、分块等价和edge不平滑证明。

## 既有issue与状态口径

- GH #12：169 DSP应用及用户cable恢复已完成；报告仍列patch恢复未做，在其标注的4a4c7b9基线上也不准确。剩余是双bank键盘真实owner、35参数/structured current state/4 presets和APP startup/save。4条keyboard normalized还deferred不等于用户cable恢复仍缺。
- GH #6：identity/calibration和非线性core已完成，candidate已调用configureVcfIdentity；当前OPEN是收口证据整理未完成，不应把新N-3/N-4塞进去扩大原关闭条件。最终关闭前按实际codec→owner消费者、左右隔离/非线性测试逐条对账，不把直接setter测试冒称codec全链。
- GH #15：16项consumer缺口仍在，包括PWM与newdrone7×2；保持lossless/transfer_unavailable，不改计数掩盖。先明确抗混叠接口，再接其对应能力。
- GH #16：125项及左右program选择仍保留不执行；P6目标33/39、P8余6不改。
- P3默认四输出出口因N-1不满足；六控制源已完成的GH #11不回退为"未实现"。P4应写"模块已实现，产品owner/实际输入集成尚未完成"。P5有窗口/transform，不代表219控件已铺。代码行数比例可提示集成优先级，不能当工作价值或音色完成百分比。

## 本轮执行顺序与续行方式

1. **当前唯一主实施：task #83 / GH #18。** 完整授权见Raft4fe298c8；独立分支，从先红判据到实现、负控、Release/Debug/ASan+UBSan、host/generator、独立feature push、draftPR、exacthead CI修正都无需再次等待Codex。只在新增冻结项/格式/能力冲突处暂停该部分。保留RouteId4及legacy字符串，source使用已发布A信号(vco_a.dry_out)，不新增孔、不改B OSC归属。
2. **接续：GH #19测量与反混叠契约。** 当task83进入独立复核等待窗，Pi可继续只读参考测量/CPU/时序/延迟方案，Codex负责选定契约；不靠主观听感或未知硬件常数决定。随后先稳定VCO数值/波形路径，再classic/nonlinear/Schmitt，以明确接口交给后续VCF及effects。
3. **GH #20 VCF数值改造**使用上述采样/延迟契约；保留GH6消费者及低频行为，N4只按已有证据实现。不得同时偷偷修改完整声音模型。
4. **GH #12键盘产品owner先于大面积铺控件**：复用InputStateMachine/KeyboardBehaviour/ArpSeq及bank/preset API，完成真正可演奏的codec→owner→CV/gate→音频链，之后接实际输入与面板。35里的硬件context-only项按既有设计如实保持，不伪造声效。APP持久化最后接唯一完整candidate。
5. **GH #21平滑与P5控制入口一起落地**；随后GH15对应consumer与P6/P8按已有依赖推进。不把本轮开始工作宣称整个乐器已经完成。

每个新任务必须重复给出：exactbase/owner/隔离分支、可修改清单与排除项、提前裁定的provisional政策、成功/失败语义、真实产品验收、旧错误负控、检查/提交/CI权限、范围冲突时可继续什么。实施完成允许提交review；独立review与merge由Codex承担。Director暂离不是实施者停止已授权工作或跳过验证的理由，也不是自行改变设计/合并门禁的授权。阶段合并豁免仅属于PR17。

## 复现与证据位置

独立探针从报告cpp代码块提取，以clang++ -std=c++17 -O2 -I core/include -I generated编译；本次原样跑了N1/N2/N3。工作区audit-20260906/probe-9.cpp、probe-10.cpp、probe-11.cpp为所运行初版正文探针；ERRATA已逐项核读，数字引用限本次确实复跑的48k默认、100–5000Hz别名频带和四采样率频响。当前更新的额外频带数字未另作复测。运行时/source/manifest证据路径参见GH18–21正文及原报告file:line。未重新跑全套或硬件测量；引用的完整检查是逐批验收与exacthead hosted记录。
