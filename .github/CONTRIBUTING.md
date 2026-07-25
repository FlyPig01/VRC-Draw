# 贡献指南

感谢参与 VRC-Draw。项目当前处于 MVP 阶段，优先级依次是输入安全、绘制链路稳定、界面简单和便携轻量。

## 开始之前

- Bug 可以直接使用仓库的 Bug 模板提交。
- 小型文档和明确的 Bug 修复可以直接提交 Pull Request。
- 较大的功能、算法替换或依赖变更应先创建 Issue 讨论。
- 请不要提交用户私人图片、VRChat 账号信息或私人世界信息。

以下功能不属于当前范围：

- 注入或修改 VRChat 客户端。
- 读取、扫描或修改游戏内存。
- 绕过或干扰反作弊系统。
- 隐藏自动化、模拟真人输入或规避检测。
- 后台无人值守绘制。
- 多颜色、画布标定和复杂专业参数面板。

## 本地构建

```powershell
cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-release
```

也可以使用 Visual Studio 2022，具体命令见根目录 README。

## 代码要求

- 使用标准 C++23，避免依赖编译器私有扩展。
- 保持模块边界清晰，不把图片处理、UI 和输入控制混在一起。
- 新的用户数据必须保存在 EXE 所在目录中。
- 软件目录不可写时必须报错，不得回退到 AppData 或注册表。
- 鼠标按下状态必须具有可靠的异常释放路径。
- 绘制线程不得使用忙循环。
- 不要加入用于伪装人工输入的随机抖动或随机延迟。
- 新增依赖前必须说明体积、许可证和必要性。

## Commit 与 Pull Request

建议 Commit 保持单一目的，标题直接描述结果，例如：

```text
fix: release mouse button when VRChat loses focus
feat: add PNG drag-and-drop support
docs: clarify portable storage behavior
```

Pull Request 应包含：

- 问题背景和实现结果。
- 主要设计取舍。
- 构建与测试结果。
- UI 变更截图（如适用）。
- 对便携存储、输入安全和二进制体积的影响。

## 许可证

提交代码即表示你同意将贡献以项目的 MIT License 发布，并确认你有权提交相关内容。

