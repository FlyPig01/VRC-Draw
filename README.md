# VRC-Draw

<p align="center">
  <img alt="VRC-Draw 图标" src="assets/vrc-draw.svg" width="128" height="128">
</p>

<p align="center">
  <img alt="C++ 23" src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus">
  <img alt="Windows" src="https://img.shields.io/badge/platform-Windows-0078D4?logo=windows11">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
  <img alt="Status" src="https://img.shields.io/badge/status-MVP-orange">
</p>

VRC-Draw 是一个轻量级 Windows 桌面工具：它把用户导入的图片转换成单色笔画路径，再通过鼠标相对移动和鼠标左键控制 VRChat 画笔进行近似绘制。

项目的目标是“让图案可以被大致辨认”，不是在 VRChat 中进行像素级复刻。由于 VRChat 的画笔、视角和输入方式本身存在误差，绘制结果出现位置、尺寸、比例和细节差异属于正常现象。

> [!IMPORTANT]
> 当前项目处于 MVP 阶段。核心输入链路已经实现，但仍需要在不同 VRChat 绘画世界中继续验证速度和线条连续性。

> [!WARNING]
> 开发版本不会检查当前前台程序是否为 VRChat。按下开始快捷键后，鼠标移动和左键输入会发送给当时的前台程序；调试时请先准备本地测试画布，并随时按 `F8` 暂停。

## 功能

- 使用 C++23 开发。
- 导入或拖放 PNG、JPEG、BMP 图片。
- 线稿保留清理后的完整墨迹和原有线宽；绘画路线另行骨架化为单中心线。
- 原图、单色线稿和最终绘画路线三个独立预览页面。
- 绘画路线直接显示最终会落笔绘制的鼠标路径，不显示笔画之间的空笔移动。
- 将完整线稿同时导出为 PNG 和 SVG。
- 使用 Windows `SendInput` 发送相对鼠标移动。
- 控制鼠标左键按下和释放。
- 开始/暂停快捷键可以修改并保存，默认值为 `F8`。
- 开始绘制时自动显示置顶悬浮窗，暂停、完成、取消或出错时自动隐藏。
- 悬浮窗只显示绘制进度和预计剩余时间，使用黑色半透明背景、白色文字，无标题、图标和按钮。
- 悬浮窗支持鼠标穿透，不使用 Win32 原生控件，不注入 VRChat，也不会夺取目标窗口焦点。
- 右侧大预览区上方显示绘制进度与预计剩余时间，不向普通用户显示内部鼠标命令数。
- 左侧控制栏集中放置文件操作、预留滑块和快捷键设置，不再放置紧急停止按钮。
- 开发阶段不限制前台程序，便于使用本地测试画布调试。
- Dear ImGui + Direct3D 11 轻量界面。
- 纯便携存储，不写入 AppData 或注册表。

“笔画上限”滑块目前是预留 UI，可以拖动并保存数值，但暂时不会影响路径生成结果。

## 工作原理

```text
导入图片
   ↓
WIC 解码并缩放到固定工作尺寸
   ↓
灰度化、自动墨迹/XDoG 提取和噪点清理
   ↓
完整黑白墨迹 ─────────→ 线稿预览、PNG/SVG 导出
   ↓（仅路线分支）
Zhang-Suen 骨架细化，粗线收敛为单中心线
   ↓
按像素图结构追踪、平滑和 RDP 简化
   ↓
ExecutionPlan：DDA 插值、整数鼠标增量、抬落笔等待
   ↓
同一 ExecutionPlan 同时用于路线预览和实际绘制
```

VRC-Draw 不识别或框选 VRChat 画布，也不计算三维坐标、摄像机角度或透视关系。鼠标移动本身就是绘制输入，软件只负责播放图片生成的相对路径。

图片最长边会缩放到最多 768 像素进行处理。对以白底黑线为主的图片，程序直接提取墨迹区域再细化；对普通图片，使用 XDoG 风格的暗线响应生成单色线稿。路径追踪记录图中的“边”而不是把交叉点标记为只能访问一次，因此交叉和分叉不会再把整条线拆成大量错误折返。

落笔和笔画间空笔移动现在使用完全相同的参数：最大单步 2，每次移动后等待 16 ms；落笔前后各等待 16 ms，抬笔后等待 32 ms。执行计划还保证相邻两次鼠标左键按下至少间隔 600 ms，防止 VRChat 把非常短且相邻的两笔识别成双击并召唤橡皮。已经由笔画绘制或空笔移动消耗的时间会计入该间隔，只有不足 600 ms 时才补充剩余等待，不会在每一笔后固定增加 600 ms。输入发送器在实际调用 `SendInput` 前还会再次校验物理时间，因此快速暂停再恢复也不能绕过防双击间隔。统一小步长可以避免 Windows 相对鼠标加速度对大步长空笔移动和小步长落笔移动产生不同倍率。执行线程使用 Windows 高精度 Waitable Timer；这些值是 MVP 内部参数，不占用普通用户界面。

在 Windows“画图”等桌面画布中测试时，应把鼠标放在画布中央附近并保留足够活动空间。相对移动碰到屏幕边缘后，实际光标会停止，但程序的逻辑路径仍会继续，这会使后续笔画错位。

## 下载与使用

正式版本应从 GitHub Releases 下载便携 ZIP；每次 CI 构建也会生成可下载的 Windows 构建产物。

使用步骤：

1. 解压整个 ZIP，不要只单独复制 EXE。
2. 在解压目录打开 PowerShell，运行 `.\VRC-Draw.exe`。
3. 打开或拖入一张 PNG、JPEG 或 BMP 图片。
4. 分别查看“原图”“线稿”和“绘画路线”。
5. 切换到 VRChat，并把画笔移动到准备开始的位置。
6. 按一次开始/暂停快捷键开始绘制；默认是 `F8`。进度悬浮窗会自动出现。
7. 再按一次 `F8` 暂停，悬浮窗会自动消失；恢复绘制时再次出现。
8. 绘制完成、取消或出错时，悬浮窗自动消失并确保释放鼠标左键。
9. 如需保存线稿，点击“导出线稿”，文件会写入软件目录中的 `exports`。

点击左侧控制栏中的快捷键按钮，然后按下新的按键或组合键即可重新绑定；按 `Esc` 取消。开始/暂停快捷键与预留笔画数保存在 `data/settings.ini`。悬浮窗没有单独快捷键或相关设置；旧版的悬浮窗快捷键键值会被忽略，并在下次保存设置时清除。

程序不会保证图片在不同用户、距离、视角和鼠标灵敏度下具有相同尺寸。

## 便携存储

VRC-Draw 只在 EXE 所在目录中创建和管理数据：

```text
VRC-Draw/
├─ VRC-Draw.exe
├─ data/
│  ├─ settings.ini
│  ├─ imgui.ini
│  ├─ logs/
│  └─ temp/
└─ exports/
   ├─ image-lineart.png
   └─ image-lineart.svg
```

- 不写入 `%LocalAppData%`、`%AppData%` 或 `%ProgramData%`。
- 不写入 Windows 注册表。
- 不创建系统服务、计划任务或文件关联。
- 软件目录不可写时直接报错，不会回退到其他目录。
- 删除整个软件目录即可删除 VRC-Draw 主动创建的全部数据。

Windows 自身可能生成 Prefetch、安全日志等系统级记录；这些内容不由 VRC-Draw 创建或控制。

## 技术栈

| 模块 | 技术 |
|---|---|
| 语言 | C++23 |
| 构建 | CMake |
| UI | Dear ImGui |
| 渲染 | Direct3D 11 |
| 悬浮窗 | 置顶、半透明、鼠标穿透的 Win32 自绘 GDI 窗口 |
| 图片解码 | Windows Imaging Component |
| 鼠标输入 | Windows `SendInput` |
| 并发与取消 | `std::jthread`、`std::stop_token` |
| 配置 | 软件目录内的 INI 文件 |

项目不依赖 Qt、Electron 或完整 OpenCV。MinGW Release 构建会静态链接 GCC、标准库和 winpthreads 运行库，发布目录不需要携带对应 DLL。

当前仓库三张 `simple` 样例在 Release 自动测试中的执行计划估算如下（不含用户在 VRChat 中定位画笔的时间）：

| 样例 | 笔画数 | 内部执行命令（仅测试） | 预计绘制时间 |
|---|---:|---:|---:|
| sample (1) | 124 | 17839 | 155.8 秒 |
| sample (2) | 162 | 17209 | 166.3 秒 |
| sample (3) | 80 | 13606 | 116.0 秒 |

这是按统一小步长和内部等待时序计算的计划时间。当前版本优先保证桌面画布测试中的笔画相对位置，不再以差异化空笔步长换取速度；VRChat 中的实际结果仍需继续验证。

## 从 Git 拉取到第一次运行

以下命令均在 PowerShell 中执行。

### 1. 准备构建环境

- Windows 10/11 x64
- Git
- CMake 3.28 或更高版本
- 支持 C++23 的 MinGW-w64 `g++`
- Ninja
- 可访问 GitHub，以便首次配置时获取固定版本的 Dear ImGui

可以先确认命令是否已经安装：

```powershell
git --version
cmake --version
g++ --version
ninja --version
```

如果其中某条命令提示“无法识别”，需要先安装对应工具并将其加入 `PATH`。

### 2. 拉取仓库

将 `<仓库 URL>` 替换成实际 GitHub 仓库地址：

```powershell
git clone <仓库 URL>
cd VRC-Draw
```

如果已经拉取并位于项目根目录，可以从下一步开始。

### 3. 配置项目

```powershell
cmake --preset mingw-release
```

该命令会读取 `CMakePresets.json`，选择 MinGW、Ninja、C++23 和 Release 配置，并在首次执行时下载固定版本的 Dear ImGui。

### 4. 编译程序

```powershell
cmake --build --preset mingw-release
```

编译完成后，程序位于：

```text
build/mingw-release/portable/VRC-Draw/VRC-Draw.exe
```

### 5. 运行测试

```powershell
ctest --preset mingw-release
```

测试成功时会显示：

```text
100% tests passed, 0 tests failed
```

测试不会启动 UI，也不会发送绘制鼠标输入。首次运行前建议执行，但它不是启动程序的必要命令。

### 6. 启动 VRC-Draw

确保当前 PowerShell 位于项目根目录，然后运行：

```powershell
.\build\mingw-release\portable\VRC-Draw\VRC-Draw.exe
```

完整的首次运行命令如下：

```powershell
git clone <仓库 URL>
cd VRC-Draw
cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-release
.\build\mingw-release\portable\VRC-Draw\VRC-Draw.exe
```

## 后续开发时的常用命令

已经完成首次配置后，平时修改 `.cpp` 或 `.hpp` 文件通常只需要：

```powershell
cmake --build --preset mingw-release
ctest --preset mingw-release
.\build\mingw-release\portable\VRC-Draw\VRC-Draw.exe
```

## 重新生成程序图标

图标的唯一可编辑原始资源是 `assets/vrc-draw.svg`。仓库同时提交生成后的 `resources/VRC-Draw.ico`，因此普通构建和最终用户运行软件时不需要安装 Python 或 Pillow。

修改 SVG 后，开发者可以安装 Pillow 并重新生成 ICO：

```powershell
python -m pip install Pillow
cmake --preset mingw-release
cmake --build --preset mingw-release --target generate-icon
cmake --build --preset mingw-release
```

生成脚本会直接读取 SVG，分别渲染 `16`、`20`、`24`、`32`、`40`、`48`、`64`、`128` 和 `256` 像素版本，并产生两类文件：

- `resources/VRC-Draw.ico`：包含全部 9 个尺寸的复合 ICO，供 Windows 根据显示场景自动选择，并通过资源脚本编译进 EXE。
- `resources/icons/VRC-Draw-<尺寸>.ico`：9 个单尺寸 ICO，便于逐个查看和验证。

Windows 图片预览器通常只显示复合 ICO 中的一种尺寸，这不表示文件内部只有一个图像。测试程序会同时验证复合 ICO 的 9 个目录项和 9 个单尺寸文件。

如果没有修改代码，只想再次启动程序：

```powershell
.\build\mingw-release\portable\VRC-Draw\VRC-Draw.exe
```

## Visual Studio 2022 构建方式

```powershell
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build/vs2022 --config Release
ctest --test-dir build/vs2022 -C Release --output-on-failure
.\build\vs2022\portable\VRC-Draw\VRC-Draw.exe
```

## 项目结构

```text
VRC-Draw/
├─ .github/                # CI、Issue 和 PR 模板
├─ assets/                 # 图标的 SVG 原始资源
├─ docs/                   # 设计文档
├─ resources/              # 生成的 ICO 与 Windows 资源脚本
├─ simple/                 # 测试图片
├─ src/                    # 应用源代码
├─ tests/                  # 核心测试
├─ tools/                  # 开发资源生成脚本
├─ CMakeLists.txt
├─ CMakePresets.json
├─ LICENSE
└─ README.md
```

详细 MVP 设计见 [VRC-Draw MVP 设计方案](docs/VRC-Draw-MVP-设计方案.md)。

## 开发状态与路线图

- [x] C++23/CMake 工程
- [x] Dear ImGui + D3D11 UI
- [x] WIC 图片读取
- [x] 单色路径提取和预览
- [x] 原图、线稿和最终执行路线分离预览
- [x] DDA 斜线插值和统一 ExecutionPlan
- [x] PNG/SVG 线稿导出
- [x] 亮色 UI
- [x] 相对鼠标移动和左键控制
- [x] 可修改并持久化的开始/暂停快捷键
- [x] 自动显示、半透明且鼠标穿透的只读进度悬浮窗
- [x] SVG 单一图标源和多尺寸 Windows ICO
- [x] 左侧控制栏、右侧大预览、绘制进度与预计剩余时间
- [x] 便携目录存储
- [x] Windows CI
- [ ] 在更多 VRChat 绘画世界中验证参数
- [ ] 让笔画上限实际参与路径筛选
- [ ] 改进路径排序和空笔移动
- [ ] 增加更好的单色明暗表现

路线图不承诺具体发布时间。MVP 验证完成前不会加入画布标定、多颜色或复杂图像参数面板。

## 贡献

欢迎提交 Bug、文档改进和范围明确的功能建议。开始开发前请阅读 [贡献指南](.github/CONTRIBUTING.md)，较大的功能应先创建 Issue 讨论设计边界。

提交代码前至少应运行：

```powershell
cmake --build --preset mingw-release
ctest --preset mingw-release
```

## VRChat 与安全说明

VRC-Draw：

- 不注入 VRChat 进程。
- 不读取或修改游戏内存。
- 不修改 VRChat 客户端文件。
- 不绕过或干扰反作弊系统。
- 不伪造游戏网络数据。
- 不实现隐藏、反检测或伪装真人输入的功能。

但是，使用操作系统公开输入 API 不代表该行为一定符合 VRChat 的规则。外部鼠标自动化是否被允许，最终取决于使用时最新的 VRChat 服务条款、社区规则及相关政策。请仅在获得允许的场景中使用；本项目不承诺零封号风险。

发现安全问题时请按照 [安全政策](.github/SECURITY.md) 私下报告，不要直接公开可被滥用的细节。

## 许可证

VRC-Draw 使用 [MIT License](LICENSE)。第三方组件的许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

VRChat 是 VRChat Inc. 的商标。本项目与 VRChat Inc. 无隶属、认可或赞助关系。
