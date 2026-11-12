# Level Sequence 数据导出配置

四个预设已更新：`MRC_PT_Noisy_1SPP`、`MRC_PT_Reference_64SPP`、`MRC_PT_Reference_128SPP`、`MRC_PT_Reference_512SPP`。

在 Movie Render Queue 任务中选择对应预设，选择 Level Sequence 和地图后渲染。配置位于 `/Game/RayRepair/Cinematics/MRGConfig`。`Validation` 文件夹内的是短序列测试副本，正式导出请选择上述四个预设。

## 改动与原因

| 数据 | 原因 | 当前设置 |
|---|---|---|
| Albedo、粗糙度、法线 | 读取 GBuffer 的后处理材质挂在 Path Tracer 上；PT 不执行通常的 BasePass，因此输出黑色或固定值 | 移到独立 Geometry 分支的 Deferred Renderer |
| Depth | 内置材质输出实际距离，不能按普通 RGB 图显示；PT 多采样还会改变其深度取样 | Deferred 单空间样本、单时间样本、AA=None；保留原始浮点距离 |
| RGB | 原 EXR 使用 tone curve，且线性颜色直接显示容易误判亮度 | PT 关闭 tone curve、OCIO、降噪，输出线性 HDR；预览单独映射 |
| Motion | RG 以 0.5 为零运动基准，静态区域黄色正常；半精度损失微小运动 | 辅助通道统一 32 位浮点、无损压缩 |
| 512SPP | 原命名为 512 的预设实际设置为 128 | 空间样本数修正为 512 |

四个预设的时间采样均为 1。关闭运动模糊、景深、自动曝光适应、局部曝光、Bloom 和色差，减少逐像素数据与 RGB 的后处理错位。RGB 仍遵循相机曝光参数，不表示未经曝光缩放的物理辐亮度。PNG 输出已停用；从 EXR 生成显示预览。

## 输出与读取

新输出根目录为项目的 `Saved/DatasetLinear/{sequence_name}/{spp}spp/`，旧 `Saved/Dataset` 不会被覆盖。

```text
beauty/0000.exr
aux/base_color/geometry.0000.exr
aux/world_depth/geometry.0000.exr
aux/world_normal/geometry.0000.exr
aux/roughness/geometry.0000.exr
aux/motion_vectors/geometry.0000.exr
```

- `beauty`：线性 HDR RGB，EXR 为 half/float16。训练时读原始线性值；显示时采用统一曝光、色调映射和 sRGB 转换。不要把各帧分别自动归一化后作为训练输入。
- `base_color`：线性材质底色，显示时可做 linear→sRGB。Deferred/Substrate GBuffer 有自身存储量化，32 位文件不会增加源 GBuffer 的精度。
- `world_depth`：R 是沿相机前向的线性深度（厘米）；`depth_m=R/100`。G 是射线距离，B 是世界坐标 Z；三者不能当成彩色深度图。天空/远景需要数据集明确的有效范围掩码。本工具的 200 米截断仅用于预览，不修改 EXR。
- `world_normal`：当前材质使用 `[0,1]` 编码，`normal=RGB*2-1`，世界空间。
- `roughness`：读取 R 即可，范围通常 `[0,1]`。
- `motion_vectors`：内置材质的 `velocity_ndc=(RG-0.5)*4`；B 是 custom stencil，不是运动 Z。按引擎 current NDC−previous NDC 约定转换到左上原点像素位移：`dx=2*W*(R-0.5)`，`dy=-2*H*(G-0.5)`。使用重投影时需匹配采样方向、遮挡和首帧有效性。

渲染范围仍由序列与 Camera Cuts 决定。当前 `LS_AssassinGirl` 的序列播放区间是 0..999，但 Camera Cut 只覆盖 0..299；若需要后续帧，需扩展/补齐镜头切段并检查动画覆盖范围。本次未延长镜头或动画。

## 验证证据

2026-09-16（America/Los_Angeles）完成两次 GPU 验证：

1. `Saved/Diagnostics/render_validation_20260916_193809`：1SPP 和 128SPP 各渲染 0、1、2 帧；实际 1920×1080，36 个 EXR，55.54 秒。Albedo、粗糙度、法线恢复场景细节；三组深度逐值完全相同。
2. `Saved/Diagnostics/render_validation_20260916_195330`：重新启动编辑器后从已保存预设生成副本；640×360，同样 36 个 EXR，23.88 秒，executor success=true。最终文件命名无缺少 layer token 的提示。原始通道统计及显示预览位于该目录的 `audit/`。

这验证了短片段导出及保存后的配置有效性，未渲染完整 300 帧，也未实测 64SPP/512SPP 全序列。辅助通道之间仍可能存在少量材质/浮点差异；Deferred 单点辅助数据和 PT 抗锯齿 RGB 在轮廓、透明物体、反射中并非严格等价。

执行器没有上报失败，但引擎日志在每个任务初始化时仍出现一次 `Cannot resolve version number without a valid evaluated graph`。任务随后成功完成，文件齐全；该初始化日志问题未修改引擎处理。EXR 的通用 `unreal/layerData/rgba` 采样元数据也不能单独作为 PT SPP 的依据；以保存的预设和 `config_disk_readback.json` 为准。

## 配置维护与恢复

`capture_config.json` 集中管理资产、SPP、输出格式和验证参数，没有把本机引擎安装路径写进执行脚本。

在 Unreal 编辑器控制台执行（将 `<project>` 换成项目目录）：

```text
py "<project>/Scripts/mrg/apply_capture_config.py"
py "<project>/Scripts/mrg/validate_capture.py"
```

第一条备份并更新指定预设，同时重建节点编辑器表示；第二条只渲染独立测试副本。不要通过会在脚本返回后退出编辑器的 `-ExecutePythonScript` 启动异步 PIE 渲染。

使用含 OpenEXR、NumPy、Pillow 的 Python 生成独立显示预览：

```text
python Scripts/mrg/audit_capture.py <validation-output-directory> --depth-max-m 200
```

原始配置备份：`Saved/Diagnostics/config_repair_20260916_192904/Content/Cinematics/MRGConfig/`。后续修改前的备份也保存在各 `config_repair_*` 目录。恢复时先关闭相应资产编辑器或退出 Unreal，再将选定备份中的四个 `.uasset` 复制回 `Content/RayRepair/Cinematics/MRGConfig/`；不要覆盖其他地图或序列。

独立进程的磁盘回读：`Saved/Diagnostics/config_disk_readback.json`；完整节点文本：`Saved/Diagnostics/MRC_PT_*_final.copy`。实验记录位于 `experiment_logs/2026-09/2026-09-16_mrg-dataset-configuration-repair.md`。
