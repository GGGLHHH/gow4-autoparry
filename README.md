# gow4-autoparry

战神4（God of War 2018，PC 版）单机辅助：自动弹反 / 自动闪避 / 连按 QTE 自动完成 / 可收集物透视。个人使用。

## 这是什么

- **`gow4_autoparry.dll`**：注入进自己这份 `GoW.exe` 的 DLL，钩游戏自己的格挡判定、命中结算、飞行物扫掠、
  输入条件判定和手柄轮询。
  - 正面来的、盾接得住的攻击 → 判成弹反（飞行物提前按 L1，让游戏自己弹回去）。
  - 背后来的、红光（不可格挡）、盾的掩码接不了的 → 提前闪避；来不及就吞掉这一下再闪。
  - 开宝箱 / 抬东西 / 挣脱这类「连按某键」的 QTE → 走游戏自带的自动成功路径。
- **`gow4_esp.dll`**：钩 DXGI Present，用 Dear ImGui 把附近的可收集物画在游戏画面上
  （宝箱、诺恩宝箱和它的机关——符文封印 / 铃铛 / 转盘、古物、奥丁的乌鸦等；已经拿走的不画）。
- **`gow4_inject.exe`**：注入 / 卸载上面的 DLL（`LoadLibraryW` 远程线程；卸载靠 DLL 自己等的命名事件）。
- **`gow4_probe.dll` / `gow4_blockprobe.dll`**：早期的只记录探针（QTE 输入判定、格挡判定），不改任何结果。

逆向结论、各版本的机制和还没验证的点都记在 [docs/gow4-findings.md](docs/gow4-findings.md)。

## 为什么在这款游戏上做是 OK 的

- **纯单机游戏**，没有联机模式，也没有反作弊；改的只是自己这一局。
- 工具不碰 DRM、不改游戏文件，只在运行时作用于本机自己启动的那一个游戏进程，卸载 DLL 时摘掉全部钩子。

## 规矩

- 这个项目用的是**进程内注入**，不是「外部读写内存」：
  是 2026-11-14 明确选的方案（按性能最好的方式），只限这款单机游戏。
- 偏移和特征全部对应 **`GoW.exe` v1.0.13**
  （SHA-256 `A3B83B8747D44E1162F5ABF951C9901A5E0ACED9B9FF7DF47C931849B5EAF4F7`）；换了版本要重新核对。
- 数值从游戏自己的静态数据（招式表、攻击定义、对象层级）里读，不靠「打一遍学出来」。
- 第三方程序、反编译产物、构建产物都不进版本库（见 `.gitignore`）。

## 目录

```
gow4-autoparry/
├── README.md
├── CMakeLists.txt
├── build.ps1                  构建脚本（用 VS Build Tools 自带的 CMake）
├── docs/
│   ├── gow4-findings.md       逆向记录（先读这个）
│   └── scr-action-hashes.txt  脚本动作名 ↔ 哈希
├── src/
│   ├── inject.cpp             gow4_inject：注入 / 卸载
│   ├── autoparry/
│   │   ├── autoparry.cpp      当前版本（横幅里写着版本号）
│   │   ├── gow4_autoparry.ini 配置（每项都有注释）
│   │   └── autoparry_v*_stable.cpp.txt   建仓库之前手工留的稳定版源码快照
│   ├── esp/                   透视 DLL 和它的配置
│   └── probe/                 QTE / 格挡探针（只记录）
├── tests/esp_host.cpp         透视 DLL 的测试宿主（不碰游戏）
├── tools/                     只读分析脚本（Python：读运行中游戏的内存、统计日志、查 exe）
├── third_party/imgui/         Dear ImGui（MIT）
└── release/                   手工冻结过的稳定版（二进制不进版本库，只留各版的 ini）
```

## 构建与运行

```powershell
.\build.ps1                                   # 产物在 build\Release\
cd build\Release
.\gow4_inject.exe load gow4_autoparry.dll     # 游戏运行中注入自动弹反
.\gow4_inject.exe load gow4_esp.dll           # 注入透视
.\gow4_inject.exe unload gow4_autoparry       # 卸载（名字不带 .dll）
.\gow4_inject.exe unload gow4_esp
```

- 注入前确认 `build\Release` 里的 DLL 不比源码旧；注入后看日志里的版本横幅和钩子数。
- 配置文件放在 DLL 同目录，游戏运行中改了保存就生效（自动弹反每秒重读，透视 2 秒内）。
  `gow4_esp.ini` 构建后会自动拷过去；`gow4_autoparry.ini` 要从 `src\autoparry\` 手工拷。
- 日志在 `%TEMP%`：`gow4_autoparry.log`、`gow4_esp.log`、`gow4_qte.log`。
- 分析脚本的环境：`python -m venv tools\.venv`，再 `tools\.venv\Scripts\pip install capstone pefile`。

## 当前状态（2026-11-15）

- [x] 近战自动弹反（正面）、背后 / 红光 / 盾接不了的自动闪避
- [x] 预判闪避 + 招式预读：出伤前提前闪，不等碰到（v6.22）
- [x] 飞行物：盾接得住的提前按 L1 弹回去，接不了的 / 背后来的提前侧闪（v6.23，**待实战验证**）
- [x] 连按 / 定时单按 QTE 自动完成（v6.20，**待实测**）
- [x] 透视：可收集物 + 诺恩宝箱机关按游戏对象层级归属，箱子开过就不再标
- [ ] R3 / L3+R3 处决提示自动按
- [ ] 投掷物 / 光束类攻击接进招式预读
- [ ] 稳定后冻结一版
