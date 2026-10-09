> 状态：待拍板

# 开放决策与收官盘点（2026-10-08）

架构评审后续（`architecture-review-followup-2026-10-07.md`）执行至此的终态快照：
**backlog 66 行 closed / 15 行余量（r126 复算：r125 计划覆盖核补挂 D11 后）**（余量全部为：拍板、真机、设计三类——可机械面已清零）。
本文把「等你一句话」的事项收成一页；每项给问题、选项、影响，拍板后即可按项推进。

## 一、验证基线（本页所有数字均本机/CI 实测）

_r100 全基线复测：755/755 · 7/7 · 10+22 · success@c756dbd（六项全数当轮实取）。r127 更新：guard 11 检查 · 单测 24/24（protocol-events 与 D11 双测入列）。_

| 基线 | 数值 |
|---|---|
| macOS 测试 runner | **PASSED 768/768**（r116 连接 +4 / r117 状态 +3 / r118 电量 +3 / r119 电源 +3：四链统一范式，r119 附 Core PowerMgmtEvent public init 一行放宽） |
| 固件 host 测试 | **7/7 目标全过**（gateway+voice_ble 纯逻辑） |
| release_guard | **11 检查全过**（r127 增 protocol-events；r100 曾纠 9 之误）+ 单测 **24/24**（r127 增 D11 双测） |
| CI | 七 job（macos/firmware/host/windows/script-tests/release-guard/website）全绿 |
| Windows 测试面 | 20 套件（N8 十五刀切分后）CI ctest 全绿 |
| 文档引用完整性 | r99 定向核：本会话迁移/删除 6 旧指针全 clean；现行三文档 dir-ful 断链 0（唯一旗标 firmware/latest/ = 发布产物路径按设计不在库）|
| 提交范围 | r101 近 12 提交逐个核：触达文件与信息主张一一对应，零越界零漏提（docs 类仅 Doc/Plan、feat/fix 类为 main.c+台账配对）|
| 测试注册完整性 | r102 Windows 测试三面核：磁盘↔CMake 16=16 双向空差；test_suites.h 声明↔core_tests.cc 定义↔main 调用 14=14=14 四向交叉全空（N8 拆分纪律结构化实证）|
| macOS 测试注册 | r103 三面核：21 文件 ↔ main.swift 24 调用 ↔ 24 定义，call↔def 双向空差；未挂套件仅 Runner/TestSupport 两基础设施件（按设计豁免）|
| 台账行间引用 | r104：实有 81 行；文中引用而无行的 16 个 ID 逐名=backlog 行6 声明豁免集（9-22 三批已关闭项见上游记录，逐项吻合）；本文档 11 拍板项引用零缺 |
| Hub 索引实存 | r105：AGENTS 文档索引 22 个 Doc 路径全实存；6 技能名全挂目录（磁盘 6=索引 6）；三 hub 原始 sha 差异=标题/自指行设计差，check_hub 经 normalize 比对为权威语义（持续绿）|
| 固件组件注册 | r106：main SRCS↔磁盘 1=1 双向空差；REQUIRES 全列 8/8 本地组件（audio_pipeline/bmi270/gateway/mini_encoder_c/power_log/stick_s3_board/ui_status/voice_ble），首版 PRIV_REQUIRES 单模式正则漏抓=误报，实文定谳 |
| 配置字段三面 | r107：example↔desktop-config 文档↔解析器——抓真漂移 14 键（example 有、文档零提及，含 air_mouse 调参 8 键 + resource_id/auto_enter/debug_audio_cache/两设备覆盖/translation_target）→ 当轮已补录 desktop-config 文末；解析器两侧读取风格异构致镜头噪声（win 值 9/mac 值 47 不作互证，target/transform=子表键已文档化为 [output] 形式）|
| 现行文档全域引用 | r108：8 份现行文档（backlog/盘点/desktop-config/三 hub/release/build-and-test）scripts 域 24 处 + Doc 域 92 处 = 116 引用全实存零缺；与 r99（三文档 dir-ful）/r105（hub 索引）合龙，引用域闭环 |
| README 双语对等 | r109：EN/ZH 结构全平（17 标题/8 链接/12 围栏逐一相等）、双语相对链接零断链；VERSION=2.4.9 串双 README 同缺=对称（README 版本无关设计，版本权威在 VERSION/appcast）|
| 网站 i18n 用键 | r110：zh/en 字典 80=80 对等（check_i18n 复证）；Vue/TS 字面量用键 67 个全命中、字典缺 0（动态拼键不在字面量镜头内=已声明盲区）；补足 check_i18n 只查对等不查 usage 覆盖的缺口 |
| 双模板对账 | r111：example 与 windows config.template tomllib 解析双绿；键差 10/32 分层定谳（角色异构+注释态呈现算在场+兼容别名噪声剔除后真缺 5 键：launch_at_login/global_hotkey/global_hotkey_enabled/developer_mode/llm_disable_thinking=mac 解析而 example 零现） 当轮按 mac 默认值注释式补入 example；tomllib 有效性纳入基线 |
| template 键文档覆盖 | r112：32 个 template-only 键对 desktop-config 正文——零现 9 键（r111 的 4 跨平台键文档侧亦缺 + selection_hotword_enabled/tencent_engine_model_type/tencent_hotword_id + 2 兼容别名 output_target/text_transform）→ 当轮 9 键补录文档并标注别名等义 |
| 协议 UUID 三端 | r113：protocol.md 10 个 UUID 三端零现=EMPTY（10/10 在 win+mac 字面量在场）；固件镜头须读小端数组形——voice_ble.c BLE_UUID128_INIT @109-125 六条（服务+5100-5105 特征）与 AB5E0002 在 gateway client 均实证（字面量搜法误判 fw=0，格式形核后归真）|
| 下载三端核 | r114：version=VERSION(2.4.9) build 内联；实资产 gh 核——mac dmg ✓ 固件 merged/ota bin ✓ 齐、**windows msi v2.4.9 缺席=按钮 404 真断链**（v2.4.6 有 msi=模板命名无误、仅发布未至）→ 转拍板第 12 项；appcast 各平台版本=最后已发布（win 2.4.6/mac 2.4.9 各自正确非漂移）|

## 二、拍板邀请（12 项，按解锁价值排序；r114 增第 12 项 Windows msi）

| # | 问题 | 选项 | 拍板后动作 |
|---|---|---|---|
| 1 | **P0-2** 固件按住松开即回执（+一次性 eFuse） | 做 / 不做 / 改方案 | 设计+真机（唯一 P0） |
| 2 | **A4b** OS-HID+app 双入站并存时广播放开 | 放开 / 维持现状 | 产品语义定后实现+真机 |
| 3 | **N6-a** 根 9 个 .bat | 仅留 build_win(+do_build) 其余删除 / 全留 / 移 scripts/ | 清理即关 |
| 4 | **N6-b** m0/p1 跟踪矛盾（盘在+跟踪48/61+ignore 并存） | A=git rm --cached（史留出索引）/ B=取消 ignore 正式入库 / C=保持 | 一条命令 |
| 5 | **N6-c** docs/superpowers 历史目录 | 删除 / 留档 | 一条命令 |
| 6 | **A6b** 小米遥控跨模块 API+真机复现 | 立项范围 | 需先定范围 |
| 7 | **C4** 后端侧（试用/凭据服务端） | 要 / 不要 | 后端排期 |
| 8 | **C7b** 云试用凭据下发加设备证明 | 加固方案选型 | 与 C4 可并案 |
| 9 | **B14b** 热词③提炼口径并入统一校验 | 并入（收紧）/ 维持有意差异 | 一行决策+回归 |
| 10 | **A7b** app_event 丢弃计数上报通道 | A=state 突发帧加键（须重算 237B 预算+16B 初始帧红线+protocol 同步）/ B=power_log 条目 / C=仅日志（接受即关） | 按选项实现或直接关 |
| 11 | **C1 加固** 防删 config 重置试用 | 做（注册表镜像+DPAPI 包绑+加载取 min）/ 不做（接受现状） | 安全加固排期或关 |
| 12 | **Windows 2.4.9 msi 缺席**：网站按钮按 VERSION 拼 GH 链接、v2.4.9 无 msi 资产 → **按钮 404**（r114 实核；dmg/固件 bin 齐、v2.4.6 命名规范佐证模板无误） | A=补发 windows 2.4.9 msi 至 GH release（发布动作）/ B=按钮改指 COS 主源（dl.davenger.cloud 同 appcast 通道）/ C=页面回退最近含 msi 版本 | 发布轮执行或前端改链 |

## 三、真机清单（须设备在手，4 项）

- **A3** 真机判定门槛 · **A4b**（与拍板 2 并案）· **A6b**（与拍板 6 并案）· **P0-2**（与拍板 1 并案）
- **A9b 真机验收**（r92 实施后）：tap/air_mouse 延迟无感、双击窗不抖、OTA 看门狗不因 I2C 顺延、encoder 掉线补发 up 不变

## 四、设计类（1 项可自主推进）

- **A9b** 传感轮询任务 —— 设计+实施已落（r91/92：sensor_poll_task 方案文档 + main.c 五回调改造，fd209f4/e547374）；余真机验收项已并入下方真机清单（第 5 项）

## 五、已收官大项（详见 backlog 对应行）

- **N1 六刀**（协调器/配置/Ogg 入 Core，类型依赖清零，FSM 测试落地）
- **N7 三切**（Windows 前三大源削平 4069/3816/3422→2601/2760/2558）+ **N7b 画像关闭**（70 virtual 不拆）
- **N8 十五刀**（core_tests 17011→199，-98.8%）· **N9/N10 门禁**（i18n-desktop + doc-plan-status）
- **9-22 复核队列 9 行全清**（B11/B12/B15/B16/B17/B18/B19/B20/C1：8 修 1 证伪）
- **A7/E9** 收尾（重试+计数 / 退出码两层审计）
