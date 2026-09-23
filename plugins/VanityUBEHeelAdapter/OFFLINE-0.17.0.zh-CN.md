# VHA 离线高度扫描器与控制台重载接续说明

## 先分清两个交付物

**离线扫描工具现在就能运行，兼容你现有的 Adapter 0.16.1。它不含、不替换 DLL。** 工具本身是 Python 源码，包含中文窗口，只需 Python 3.10+（窗口需要 tkinter）。

**控制台重载和离线工具已整合进 0.17.0 源码，交由原开发分支的 GitHub Actions 测试、编译和打包。** 是否已经构建通过，以对应提交的 Actions 结果及包内 `BUILD.json` 为准；源码提交本身不代表 DLL 已构建成功。使用控制台命令必须同时安装新 DLL 和配套命令 ESP；保留已有基础配置、用户配置及 Glass=1.1。

因此，不要在 0.16.1 上直接输入尚未安装的 `VHA_Reload`，也不要把单独的命令 ESP 当成修复插件。离线工具则可以先独立使用。

## 这次日志告诉了我们什么

当前日志只有一次插件启动，随后在 14:23:01.837 接受了配置 revision=3；14:23:02.126 提交了 CPB+Glass 的 `NoHeel=0、Heel=0.9`。状态文件也记录这些值。说明至少这次配置读取和 API 提交发生过，不能说整个文件读取一直失败。

但 `submitted=true` 不是画面验收。你没有看到预期脚形变化，问题仍然成立；可能在局部几何更新或后续覆盖环节。新手动入口会同时强制重读和多次局部重应用，而不是把同一个文件检查再执行一次。仍需最终画面验证，不承诺“命令一加就必然解决全部原因”。

裸脚已由你确认正常，本轮不修改裸脚判据。保留 Glass 的手工 `NoHeel=0、Heel=1.1`。贴合度、收紧、修形继续暂停。

## 一、无需进游戏的扫描流程

### 1. 在 MO2 中添加程序

把离线工具压缩包解压到普通工具目录，例如：

```text
D:\MO2\tools\VHAOffline\
  tools\vha_offline.py
  tools\height_editor.py
  tools\offline\...
```

这不是一个必须安装进游戏 Data 的模组。`offline` 子目录必须保留，不能只复制 `vha_offline.py`。

MO2 的“编辑可执行程序”中新增：

```text
名称：VHA 离线高度扫描
程序：你已安装的 Python 的 python.exe（建议 64 位）
参数："D:\MO2\tools\VHAOffline\tools\vha_offline.py" gui
起始目录：D:\SteamLibrary\steamapps\common\Skyrim Special Edition
```

通过 **MO2 的运行按钮**启动，不要只在资源管理器双击脚本。这样程序读取游戏 Data 时才能看见 MO2 当前虚拟文件系统中的最终资源。先关闭 Skyrim 即可，扫描不需要启动它。

### 2. 选择输入

窗口中选择：

```text
游戏 Data 目录：
D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data

MO2 当前 profile 目录：
D:\MO2\profiles\<正在使用的配置名称>
```

profile 中应有 `plugins.txt` 和通常存在的 `loadorder.txt`。工具只用启用列表，不把整个 `mods` 文件夹中的所有 ESP 当成已加载插件。不要选错 profile。

**源模型体重**是游戏角色的 0～100 体重，不是公斤。你此前快照为 0，这轮可先保持 0。扫描默认读取现有 BodySlide 生成的 NIF 基准，不额外套任何预设；不会读取当前 OBody、骨骼动画或脚跟抬升。可选的 BodySlide XML 预设是命令行高级功能，已按该预设 Build 的模型不要再叠加一遍。

平脚参考默认仍是你已经验证过的：

```text
[AFxII] Converse AS.esp|0000080A
```

扫描后下拉框也可选确切的 `armor::addon`。这里假设该鞋为平脚、丝袜 `NoHeel=1` 为平脚端点，不是因为文件名含 Converse 就自动证明了这些事实。

### 3. 扫描、计算、复核

依次点击：

**“1. 扫描启用装备” → 选择丝袜 → “2. 计算高度配对”**。

选择丝袜可以按 Ctrl 多选；不选则计算所有可测丝袜。为控制第一次扫描规模，可先只选 CPB。最多计算 20000 个配对，超出时要求缩小丝袜选择，而不是无提示截断。

工具先处理插件覆盖关系与 ARMO→ARMA，再读取实际女性第三人称模型（不是库存模型）。所有启用 UBE 女性装备部件都可以进入能力检查，避免仅凭英文名称或 slot38 漏掉连体丝袜。每件仍必须存在实际 `NoHeel` 差分，才成为可测丝袜。无此能力的 TooHotForYou 一类会被列出，但不伪造滑块。

鞋子优先使用独立 `Feet` 网格。平脚姿／高跟姿分类来自原生脚部几何拟合，**不是厘米，也不是鞋底外壳的真实跟高**。没有独立脚、不同拓扑、变换不兼容、多条难以区分的 UBE ARMA、资源缺失等情况会写明原因，不凭文件名猜数值。

计算结果中的重要状态：

| 状态 | 含义 |
|---|---|
| `within-mathematical-limits` | 通过当前数值门槛，仍需视觉确认 |
| `review-residual` | 有数值，但误差超过 0.15，默认不批量应用 |
| `range-saturated` | 最优值落到限制外，不当作已解决 |
| `manual-adjusted` | 你明确修改过该建议；原始计算残差不等于修改后的残差 |
| `NoHeel-unavailable` | 没有可用的丝袜脚型差分 |
| `unsupported-topology` / `multiple-eligible-shapes-needs-review` 等 | 当前结构不支持或有歧义 |

Glass、Agata 在历史样本的 C++/Python 对照中分别约为 `Heel=1.10677` 和 `0.491227`，残差仍高于默认门槛。新离线工具直接读你当前 NIF 基准，未必得到逐位相同的值；这些历史数字没有硬编码进扫描器。

### 4. 一键应用与手工标记

选好建议后点击 **“3. 一键应用所选”** 并确认。也可先点“选择误差门槛内建议”。对于 `review-residual`，只有明确勾选“允许本人复核过的超误差建议”才允许保存；这不会更改全局误差门槛。

“一键应用”实际做的是：**把所选数值合并为固定的手工配对设置**。不是自动穿装备，不会改动 SVS 套装，不会写 ESP/NIF/TRI，也不冒充 OBody 上下文绑定的自动标定。

它会保留已有手工／忽略配对，包括你验证过的 Glass=1.1；已有参考配置也不会被扫描值取代。写入前检查源模型、TRI、插件和启用列表是否变化。用户文件已被其他程序修改则停止，不抢写；写入采用备份和临时替换。

“手工调整一条建议”只编辑候选报告，之后仍需应用。它不会强行覆盖已有手工值；要修改已经保存的 Glass 数值，继续使用随包的 `height_editor.py` 或直接编辑实际生效的用户文件。

“手动标记所选装备”可写丝袜／鞋／忽略／恢复自动分类。标记影响 Adapter 的用户分类，不会给无滑块模型补出滑块。对子部件作丝袜标记时记录选中的 ARMA。

### 5. 文件保存到哪里

默认输出是虚拟 Data 下：

```text
SKSE\Plugins\VanityUBEHeelAdapter\offline\
  offline-catalog.json        装备清单、能力、拒绝原因、模型来源
  offline-candidates.json     原始和受限滑块建议、残差、参考条件、源文件摘要
  offline-apply-receipt.json  本次应用／保留了哪些配对、实际用户文件位置
```

用户配置是：

```text
SKSE\Plugins\VanityUBEHeelAdapter.user.json
```

在你的环境中，通常落到 `D:\MO2\overwrite\SKSE\Plugins\...`。已有文件也可能由 MO2 路由回所属模组；以保存回执中的实际文件路径和 MO2 最终覆盖关系为准，**不要另复制一个互相竞争的用户文件**。不确定时检查 `offline-apply-receipt.json`。

退出旧 DLL 游戏后修改文件，下一次启动即可读取；这是启动读取，不叫热重载。

### 支持范围

本轮是可运行的离线工具首版，不保证任意 Skyrim 网格都能生成数值。支持当前受限的 SSE skinned BSTriShape、单分区／对应顶点布局、packed BODYTRI；支持完整相同拓扑和受限、唯一、保序的整组件删除。重排顶点、减面、任意骨骼／坐标系转换暂不自动猜测。

松散资源优先；程序包含 BSA104/105读取和 zlib/LZ4解压。默认索引启用插件的同名关联 BSA及 ` - Textures.bsa`。**仅由 INI 加载、没有关联插件名的其他 BSA，需通过 `--archive-list` 明确提供按低→高优先级排列的 Data 内文件名列表**。资源仅在未列出的 BSA 中时会报缺失，不静默捏造几何。

当前按 `!UBE\` 女性模型路径识别 UBE；自定义路径、男性模型以及种族继承链的完整游戏选择规则不在自动验证范围。`--race` 可用于明确 RACE 的直接／additional-race 过滤，但不模拟完整游戏加载器。

## 二、控制台命令：新 DLL 构建后再测试

GitHub Actions 的 Windows 构建任务会编译新 DLL，并生成只包含一个全局变量的小型 ESL 标记插件：

```text
VanityUBEHeelAdapter.Commands.esp
```

新 DLL 和该 ESP 必须一起安装，ESP 必须启用。安装包应叠加在现有 0.16.1 后面，不替换你工作的 SVS 本体、现有配置、BodySlide输出；首次加载新 DLL 需要重启游戏一次。

之后命令为：

```text
set VHA_Reload to 1
```

它显式请求读取当前实际配置；校验通过后，即使配置内容没变化，也清理本插件的局部控制去重记录，重新寻找当前目标，并安排三次间隔开的局部高度提交。非法内容保留最后有效设置。变量处理后归零。

查看状态：

```text
set VHA_Reload to 2
```

`configuration-status.json` 新增 `manualReload` 状态，区分请求、等待主线程、拒绝、接受并安排重应用。仍不把 API 提交当作画面证明。

输入后关闭控制台并让游戏恢复运行；暂停／后台挂起时，不要求主线程任务立刻执行。

**这两个命令不是现有 0.16.1 的功能。必须安装 0.17.0 构建产物，不能只安装源码或单独的命令 ESP。** 以该提交的 Actions 结果和包内 `BUILD.json` 确认完整构建；游戏内命令与画面仍需下面的验收。

## 三、本次最小测试安排

### 现在就能做：离线扫描验收

保持 Skyrim 关闭，通过 MO2 启动离线窗口，选择正确 Data/profile；先只计算 CPB。检查清单能找到 Converse、Glass、Agata，且其他不支持的条目有原因。确认数值报告与来源实际写出。

保留原有 Glass=1.1。选择一个此前没有手工规则的配对，例如 CPB+Agata，先复核数值；若是 `review-residual`，明确勾选复核允许后应用。查看回执确认它写入了哪份用户文件、Glass被保留。已有 Agata 手工值也会保留，这时改测另一条没有覆盖的配对。

随后正常启动你现有 0.16.1，显示该搭配，确认启动读取后的变化；顺带检查 Glass=1.1和裸脚平脚没有退化。不需要重建 BodySlide、不需要大量几何导出。

### Windows 新 DLL 构建后：一次控制台验收

保持 CPB+Glass 可见，修改实际用户文件的 Heel=0.9，输入 `set VHA_Reload to 1`，关闭控制台等待几秒。检查请求回执、配置修订号、实际脚形变化；再改回1.1并执行同一命令。重复执行一次内容未变化的命令，也应有新 requestId与重应用记录。最后脱鞋，检查NoHeel1/Heel0；再穿回Glass，恢复1.1。

失败时发日志、`configuration-status.json`、`runtime-state.json`、`height-events.json`；离线问题发三个 `offline-*.json` 和命令窗口报错。无需再重做整个采样流程。

## 已完成的验证

Linux本地现有13套C++核心／文件线程测试经ASan/UBSan通过；新增强制请求覆盖同内容请求、重交付、应答、无效JSON、恢复。Python覆盖插件覆盖、停用／删除、截断资源、BSA压缩、边界校验、源文件变化、保留手工项及合成端到端扫描应用。

Tk窗口在Linux/Xvfb中实际执行了扫描→计算→应用、非法NoHeel拒绝、手工调整及标记。历史真实几何回放中，Python与现有C++核心的关键值差异小于1e-5，接受／拒绝结论一致；不是逐位相等。

以上不是 Windows DLL 编译，也不是MO2注入或Skyrim画面验证。该工具没有在这里访问你的 D: 盘、没有扫描完整本机模组库。
