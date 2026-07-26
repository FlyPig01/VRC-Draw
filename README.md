# VRC-Draw

<p align="center">
  <img alt="VRC-Draw 图标" src="assets/vrc-draw.svg" width="128" height="128">
</p>

<p align="center">
  <img alt="C++ 23" src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus">
  <img alt="Windows" src="https://img.shields.io/badge/platform-Windows-0078D4?logo=windows11">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
  <img alt="Status" src="https://img.shields.io/badge/status-MVP-orange">
  <img alt="Version" src="https://img.shields.io/badge/version-v0.2.0-blue">
</p>

VRC-Draw 是一个轻量级 Windows 桌面工具：它把用户导入的图片转换成单色笔画路径，再通过鼠标移动和鼠标左键控制 VRChat 画笔或桌面绘图程序进行近似绘制。

当前版本为 **0.2.0**。

项目的目标是“让图案可以被大致辨认”，不是在 VRChat 中进行像素级复刻。由于 VRChat 的画笔、视角和输入方式本身存在误差，绘制结果出现位置、尺寸、比例和细节差异属于正常现象。

> [!IMPORTANT]
> 当前项目处于 MVP 阶段。核心输入链路已经实现，但仍需要在不同 VRChat 绘画世界中继续验证速度和线条连续性。

> [!WARNING]
> 开发版本不会要求当前前台程序必须是 VRChat。按下开始快捷键后，程序会通过一段抬笔移动自动判断目标是“中心锁定”还是“桌面光标”，再选择相对或绝对坐标输入；前台程序改变时会释放左键并自动暂停。

## 功能

- 使用 C++23 开发。
- 导入或拖放 PNG、JPEG、BMP 图片。
- 线稿使用保留中间覆盖值的抗锯齿灰度墨迹和独立二值拓扑图；绘画路线按局部类型选择单中心线或紧凑填充轮廓。
- 原图、单色线稿和最终绘画路线三个独立预览页面。
- 绘画路线直接显示最终会落笔绘制的鼠标路径，不显示笔画之间的空笔移动。
- 将完整线稿同时导出为抗锯齿 PNG 和亚像素闭合路径 SVG。
- 绘制大小可以在 `0.30×～3.00×` 调整，默认 `1.00×` 保持原有尺寸。
- 自动判断目标程序的鼠标是否被锁定在中心，不维护程序名单。
- 中心锁定目标使用相对移动，微软画图等桌面目标使用精确绝对坐标。
- 使用 Windows `SendInput` 发送鼠标移动及左键输入。
- 控制鼠标左键按下和释放。
- 开始/暂停快捷键可以修改并保存，默认值为 `F8`。
- 开始绘制时自动显示置顶悬浮窗，暂停、完成、取消或出错时自动隐藏。
- 悬浮窗只显示绘制进度和预计剩余时间，使用黑色半透明背景、白色文字，无标题、图标和按钮。
- 悬浮窗支持鼠标穿透，不使用 Win32 原生控件，不注入 VRChat，也不会夺取目标窗口焦点。
- 右侧大预览区上方显示绘制进度与预计剩余时间，不向普通用户显示内部鼠标命令数。
- 左侧控制栏集中放置文件操作、绘制大小、预留滑块和快捷键设置；栏宽可拖动调整，说明文字会随可用宽度自动换行。
- 开发阶段不限制前台程序，便于使用本地测试画布调试。
- Dear ImGui + Direct3D 11 轻量界面。
- 纯便携存储，不写入 AppData 或注册表。

“笔画上限”滑块目前是预留 UI，可以拖动并保存数值，但暂时不会影响路径生成结果。

## 工作原理

```text
导入图片
   ↓
WIC 解码，最长边1536且最多240万像素
   ↓
PS式多尺度响应＋暗线脊/RGB单边界
   ↓
自动色阶＋方向约束防粘连
   ├─→ 同源灰度覆盖线稿 ─→ 抗锯齿预览、PNG、亚像素路径SVG
   ↓
二值拓扑线稿
   ├─→ 临时证据：置信度、切线、三尺度支持、原暗线/RGB边界/补线来源
   ↓
墨迹内距离场＋局部区域分类：细线、粗笔触、紧凑填充、交叉区
   ↓
交叉边界端口配对＋正向射线门禁＋墨迹内测地线＋多证据毛刺判定
   ↓
阶段二验证路线＋无重复边的欧拉迹分解、结构边界、闭环起点和空笔顺序优化
   ↓
区域跨度／硬锚点＋保守直线/贝塞尔拟合
   ↓
当前绘制缩放下的整数门禁，不合格跨度局部回退阶段二折线
   ↓
ExecutionPlan：用户缩放、方向对称的曲率采样、整数鼠标增量、抬落笔等待
   ↓
抬笔行为探测：自动选择中心锁定相对输入或桌面绝对坐标输入
   ↓
同一 ExecutionPlan 同时用于路线预览和实际绘制
```

VRC-Draw 不识别或框选 VRChat 画布，也不计算三维坐标、摄像机角度或透视关系。软件只负责播放图片生成的鼠标路径；中心锁定程序消费相对移动量，普通桌面程序消费以开始位置为原点的绝对桌面坐标。

图片默认以最长边1536、最多240万像素处理；`1.00×`时最终鼠标绘制范围独立限制为最长边约768，用户可以用绘制大小滑块在此基础上调整到 `0.30×～3.00×`。缩放只重建鼠标执行计划，不重新提取或删减线稿。黑白或低彩度原生线稿结合原始灰度、全局阈值和局部亮度差；彩色图片结合PS式三尺度响应、局部暗脊与经过非极大值抑制的RGB单边界。预览和PNG使用灰度覆盖图保留抗锯齿边缘，SVG从同一覆盖图提取亚像素闭合轮廓；路径分析使用同源二值拓扑图。程序不再执行可能粘合近邻线条的全图闭运算，只允许有方向和弱响应证据的有限补线。路线阶段不再使用全图统一粗线半径或把交叉分支拉向最小二乘公共点，而是根据局部线宽、形状和端口数分类，并只接受正向有效射线交点或完全位于交叉墨迹内的测地线。

路径优化只合并精确共享端点的阶段二验证图边。每条原始线段仍且只绘制一次，不按距离添加连接线，也不通过重复描线减少笔画；`sourceEdgeEnds` 记录阶段二图边结束位置，独立的 `routeMetadata` 保存区域跨度、交叉端口和闭环接缝，后续矢量跨度再增加受保护尖角。转换到鼠标坐标后没有任何移动的点状笔画会保持独立落笔。优化结果必须同时满足逻辑线段多重集和最终落笔鼠标移动线段完全相同，并且预计时间不增加；任一检查失败都会自动回退到优化前路径。当前14张质量样本的笔画数从30,808降为13,589，预计总绘制时间从315.61分钟降为149.61分钟，组件覆盖率保持100%，路线越出规范墨迹比例为0。

阶段三会在硬锚点之间保守拟合直线或三次贝塞尔。浮点曲线必须通过双向距离、墨迹支持、控制柄、包围盒和自交门禁；应用当前绘制大小后，还会重新验证整数端点、偏差、折返和墨迹支持。任一检查失败只回退该跨度保存的阶段二折线。路线预览和真实输入始终消费同一个最终 `ExecutionPlan`，不会显示一条实际不执行的“漂亮曲线”。

所有逻辑鼠标移动的最大单步都限制为6，每次移动后等待16 ms；落笔路线会按线段两端曲率中更保守的要求自动降到2～5，直线段使用最多6。正向和反向绘制同一线段会严格使用互为反序的整数采样点。中心锁定模式直接发送这些相对增量；桌面模式累计增量后转换为虚拟桌面的绝对坐标，并禁止系统合并移动事件，因此不受Windows鼠标速度和“提高指针精确度”影响。落笔前后各等待16 ms，抬笔后等待32 ms。执行计划还保证相邻两次鼠标左键按下至少间隔600 ms，防止VRChat把非常短且相邻的两笔识别成双击并召唤橡皮。已经由笔画绘制或空笔移动消耗的时间会计入该间隔，只有不足600 ms时才补充剩余等待。输入发送器在实际调用 `SendInput` 前还会再次校验物理时间，因此快速暂停再恢复也不能绕过防双击间隔。执行线程使用Windows高精度Waitable Timer，并按不超过16 ms的切片持续更新绘制进度和剩余时间。

开始绘制时，程序在左键抬起状态下优先复用第一段空笔移动观察光标：光标被拉回原位时继续相对输入；光标稳定停在新桌面位置时恢复起点并以绝对坐标从头执行。探测量根据Windows指针速度自动取 `8～96`，默认设置约10；第一段空笔太短时才使用同等幅度的可逆探测。微软画图等桌面画布仍需由用户准备足够大的可见画布并把鼠标放在期望图像中心；如果完整路径会超出虚拟桌面，程序会拒绝开始而不会裁剪图像。

## 下载与使用

正式版本应从 GitHub Releases 下载 `VRC-Draw-v0.2.0-win64.zip` 便携包及同名 `.sha256` 校验文件；每次 CI 构建也会生成带版本号的 Windows 构建产物。

下载后可以在 PowerShell 中校验文件完整性，并与 `.sha256` 文件中的哈希值比较：

```powershell
Get-FileHash .\VRC-Draw-v0.2.0-win64.zip -Algorithm SHA256
Get-Content .\VRC-Draw-v0.2.0-win64.zip.sha256
```

使用步骤：

1. 解压整个 ZIP，不要只单独复制 EXE。
2. 在解压目录打开 PowerShell，运行 `.\VRC-Draw.exe`。
3. 打开或拖入一张 PNG、JPEG 或 BMP 图片。
4. 分别查看“原图”“线稿”和“绘画路线”。
5. 在左侧调整绘制大小；`1.00×`为原有尺寸。
6. 切换到 VRChat 或桌面绘图程序，并把画笔移动到准备开始的位置。桌面程序应把鼠标放在期望图像中心。
7. 按一次开始/暂停快捷键开始绘制；默认是 `F8`。程序自动判断输入方式，进度悬浮窗会自动出现。
8. 再按一次 `F8` 暂停，悬浮窗会自动消失；回到同一目标程序后再按一次即可恢复。
9. 绘制完成、取消、前台程序改变或出错时，程序确保释放鼠标左键。
10. 如需保存线稿，点击“导出线稿”，文件会写入软件目录中的 `exports`。

点击左侧控制栏中的快捷键按钮，然后按下新的按键或组合键即可重新绑定；按 `Esc` 取消。绘制大小、左侧控制栏宽度、开始/暂停快捷键与预留笔画数保存在 `data/settings.ini`。悬浮窗没有单独快捷键或相关设置；旧版配置没有新增设置项时会使用默认值。

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
| 鼠标输入 | Windows `SendInput`，运行时自动选择相对移动或虚拟桌面绝对坐标 |
| 并发与取消 | `std::jthread`、`std::stop_token` |
| 配置 | 软件目录内的 INI 文件 |

项目不依赖 Qt、Electron 或完整 OpenCV。MinGW Release 构建会静态链接 GCC、标准库和 winpthreads 运行库，发布目录不需要携带对应 DLL。

当前仓库会把 `sample` 目录中的全部 PNG 作为线稿质量回归样例。测试除验证非空线稿和墨迹比例外，还检查灰度中间覆盖、近邻平行线不粘连、路线不越出拓扑墨迹、全部拓扑组件被覆盖，以及SVG使用闭合 `<path>` 而非逐像素矩形。

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

```powershell
git clone https://github.com/FlyPig01/VRC-Draw.git
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

如果需要生成 `sample` 目录全部 PNG 的灰度线稿、拓扑图、路线对比、PNG/SVG导出和指标文件，可以运行：

```powershell
$env:VRC_DRAW_QUALITY_OUTPUT = "build/quality-output"
.\build\mingw-release\vrcdraw_tests.exe
Remove-Item Env:VRC_DRAW_QUALITY_OUTPUT
```

对比图会写入 `build/quality-output`，该目录位于已忽略的 `build` 下，可以随整个构建目录删除。

### 阶段三路径兼容回退

0.2.0默认启用经过实绘验收的直线/贝塞尔拟合和缩放后局部回退。若特定绘图程序出现兼容问题，可以临时恢复阶段二折线：

1. 启动并正常退出一次程序，让软件目录生成 `data/settings.ini`。
2. 用文本编辑器把 `vector_path_enabled=1` 改为 `vector_path_enabled=0`。
3. 重新启动并导入图片。此时“绘画路线”预览和实际鼠标输入共同恢复阶段二折线。
4. 改回 `1` 并重新启动，即可重新启用阶段三路径。

该高级兼容开关只写在软件目录中，没有新增普通用户界面控件。

### 6. 启动 VRC-Draw

确保当前 PowerShell 位于项目根目录，然后运行：

```powershell
.\build\mingw-release\portable\VRC-Draw\VRC-Draw.exe
```

完整的首次运行命令如下：

```powershell
git clone https://github.com/FlyPig01/VRC-Draw.git
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
├─ sample/                 # 测试图片
├─ src/                    # 应用源代码
├─ tests/                  # 核心测试
├─ tools/                  # 开发资源生成脚本
├─ CMakeLists.txt
├─ CMakePresets.json
├─ LICENSE
└─ README.md
```

详细文档：

- [VRC-Draw MVP 设计方案](docs/VRC-Draw-MVP-设计方案.md)
- [分辨率验证与路径质量改进方案](docs/VRC-Draw-分辨率验证与路径质量改进方案.md)
- [无损绘制时间优化方案与实施结果](docs/VRC-Draw-无损绘制时间优化方案.md)
- [绘制缩放与自动鼠标适配方案](docs/VRC-Draw-绘制缩放与自动鼠标适配方案.md)
- [线稿与真实绘制路径质量改进方案（阶段三已完成）](docs/VRC-Draw-真实绘制路径矢量拟合方案.md)

## 开发状态与路线图

- [x] C++23/CMake 工程
- [x] Dear ImGui + D3D11 UI
- [x] WIC 图片读取
- [x] 单色路径提取和预览
- [x] 原图、线稿和最终执行路线分离预览
- [x] DDA 斜线插值和统一 ExecutionPlan
- [x] 抗锯齿灰度线稿与独立拓扑掩码
- [x] 抗锯齿PNG与亚像素闭合路径SVG导出
- [x] 亮色 UI
- [x] 相对鼠标移动和左键控制
- [x] `0.30×～3.00×`绘制大小及便携设置持久化
- [x] 自动判断中心锁定/桌面光标并适配微软画图
- [x] 可修改并持久化的开始/暂停快捷键
- [x] 自动显示、半透明且鼠标穿透的只读进度悬浮窗
- [x] SVG 单一图标源和多尺寸 Windows ICO
- [x] 左侧控制栏、右侧大预览、绘制进度与预计剩余时间
- [x] 无重复描线的共享端点笔画合并和零差异质量门禁
- [x] 闭环起点、路径方向和空笔顺序优化
- [x] 便携目录存储
- [x] Windows CI
- [ ] 在更多 VRChat 绘画世界中验证参数
- [ ] 让笔画上限实际参与路径筛选
- [x] 修复真实绘画路径的局部毛刺、交叉拓扑和粗区方框误判
- [x] 对验证后的路径进行直线/贝塞尔拟合、缩放后整数门禁和逐跨度回退
- [x] 完成阶段三真实绘制验收并默认启用，保留阶段二兼容回退开关
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
