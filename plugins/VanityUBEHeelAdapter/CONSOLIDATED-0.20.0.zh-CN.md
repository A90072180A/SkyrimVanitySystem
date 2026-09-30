# UBE Adapter 0.20.0：移除袜面隐藏，工具与核心合并整理

## 当前行为

- 已从运行控制器移除袜面隐藏、显隐租约与 SetAppCulled 调用。Agata、Glass、裸脚和其他鞋均不被 Adapter 隐藏袜面。
- 旧 enableStockingFootOcclusion、coverage 字段仍可读取，以免旧配置失效；即使为 true/opaque-closed 也不再触发隐藏。
- 保留 0.18.0 高度库/共享组、手工优先、源指纹校验、超残差警告、正向确认裸脚 NoHeel=1/Heel=0，以及装备节点重建后的重应用。
- 保留 0.19.1 Glass 独立 VHA_GlassFootFit；高度仍为 NoHeel=0/Heel=1.1，准确源文件、体重0、女性及脚部形变的原有限制不变。
- 可在用户 settings 中设置 enableGlassFootFit=false 停用局部贴合而继续高度控制。该功能需要与原 0.19.1 私人资源匹配；公开 CI 包不包含个人 NIF/TRI。无资源时只使用高度功能，不假装贴合已生效。
- 旧双形状 CPB 资源仍可匹配并同时接受高度；本版控制器不修改任何节点可见性。不重新分区、不扩大原型适用范围。
- Glass 局部贴合并未新增游戏视觉验收。CI 是编译/测试证据，不是用户画面验证。

## 一个工具目录

从 MO2 运行本安装目录中的 `tools/vha_offline.py gui`。所有依赖模块都在同一个 tools 目录；不要继续用旧目录中的入口脚本。

### 路径记忆

自动记住 Data、MO2 profile、mods、报告输出、SKSE/Plugins 和用户覆盖 JSON 路径。最后三种写入路径按 profile 分开保存。切换 profile 会清空或恢复该 profile 的路径，而非沿用另一份配置的写入路径。

Windows 默认记忆文件：`%LOCALAPPDATA%\VanityUBEHeelAdapter\tools-paths.json`。这是本机工具设置，不写游戏存档，不扫描 mods，不上传。

可展开写入路径或点击“记住路径”；平时修改后自动保存，正常关闭也保存。损坏/新格式设置不阻止启动，也不自动覆盖原设置；明确点击“记住路径”才重建。命令行显式参数优先；CLI scan/apply/export-library **不使用** GUI 记忆。

### 直接编辑真实手工覆盖

扫描窗口的“直接编辑手工覆盖 JSON”无需扫描和计算报告。可以选择已有配对，改 NoHeel/Heel/模式/备注；也可以新增、忽略、删除后恢复自动，或在“原始 JSON”页编辑 settings/items/pairs。

1. 选择已有行或“新建配对”。
2. 修改字段，点击“应用此条到编辑区”。这一步尚不写文件。
3. 点击“保存手工覆盖 JSON”。原始 JSON 页可以直接编辑后保存。
4. 游戏内 `set VHA_Reload to 1`，关闭控制台让任务执行。

顶部显示实际编辑文件；活动游戏回执指出另一个文件时，拒绝保存到错误位置。保留未编辑条目及来源绑定信息；修改带 approval 的配对身份须另建条目或明确编辑 JSON，不能无声沿用旧身份的来源审批。

保存前校验字段、稳定ID、重复项、数量、NoHeel 0–1、Heel 0–heelMax、单方向分支、有限数值和 JSON 重复键。自动 `.json.bak` 备份和原子替换；发现另一个编辑器已修改原文件则拒绝覆盖，请重新读取后合并。保存、游戏接受配置、实际画面生效是三个不同状态。

独立启动：`python height_editor.py --plugins "...\SKSE\Plugins" gui`。旧观察装备/候选界面仍可用 `inventory-gui`。

## 合并与回退

必须完全退出游戏再替换 DLL。保留用户 `.user.json`、已有主配置、height-library、offline 报告；本次不要求重扫重算，也不默认发放空用户配置。

如果原数据在将要停用的旧补丁模组内，先复制这些数据到新模组或独立输出位置，保留备份；否则停用旧模组会同时隐藏其数据。MO2 Overwrite 中已有数据无需重复复制。

上一份 0.19.1 覆盖包应停用，避免其旧 DLL 赢得冲突；最终确认 0.20.0 DLL 为获胜文件。停用新模组并恢复旧模组即回退。不要热卸载DLL。

SVS build13-render-fix DLL 在另一个包中；本 Adapter 不含 SkyrimVanitySystem.dll，不修改渲染和用户外观集合。
