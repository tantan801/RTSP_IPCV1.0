# 三阶段 Demo 学习计划（待执行）

> 2026-11-06 存档。前置任务：先完成**模型持久化**（第 12 篇遗留），再按下面顺序逐个开工。
> 每阶段产出要求：跑通 + 一篇笔记 + 代码进 `项目代码/` 归档。

## 阶段一 · snap 抓拍（预计一晚）

| 项 | 内容 |
|---|---|
| 目标 | 检测到人 → 自动抓 JPEG 存档（"AI 触发抓拍"，比红框上屏更接近产品） |
| 学习对象 | `sample/snap/sample_snap.c`（全 SDK 最精简 sample，单文件千行内） |
| 关键点 | JPEG 通道创建（VENC jpeg 模式）、抓拍时机触发（复用现有 DET 线程的 persons 结果）、`HI_MPI_VENC_GetStream` 存盘路径（存 /mnt/nnie 之外的 tmpfs 或 SD 卡，**别写 jffs2**——第 5 篇教训） |
| 验收 | 真人入镜 → 自动生成 *.jpg → PC 取回查看 |
| 注意 | 与现有 sample_venc 共存：抓拍通道挂到 chn0 或独立 VENC chn，注意 VB 池余量（JPEG 一帧 ~1MB） |

## 阶段二 · vio 精读（预计一晚，不加功能、纯吃透）

| 项 | 内容 |
|---|---|
| 目标 | 把 VI→VPSS→VENC bind 链路真正吃透；对照第 13 篇底层架构，搞清每个 bind 点的数据流向 |
| 学习对象 | `sample/vio/`（全 SDK 最精简链路示例，仅 3 个文件） |
| 关键点 | bind 模式 vs user mode（GetFrame/ReleaseFrame）差异与 CPU 占用；VPSS 组调度；与 sample_venc 大杂烩对照，找出"最小必要代码集" |
| 产出 | 一篇"链路精读"笔记（可画 bind 关系图），以及评估：现有项目哪些代码可以瘦身 |

## 阶段三 · scene_auto / uvc_app（各一晚，按需取一或都做）

| 项 | scene_auto 智能场景 | uvc_app USB 摄像头 |
|---|---|---|
| 目标 | 画质自动切换（夜景/逆光/WDR），IQ 调优的产品级替代 | 板子变免驱 USB webcam |
| 学习对象 | `sample/scene_auto/`（libsceneauto + ini 参数） | `sample/uvc_app/`（UVC gadget + configfs） |
| 关键点 | GC2053 的场景参数适配情况（readme 说主要适配 imx290/imx334，需验证）；与 PQ Tool 路线二选一 | 内核 configfs gadget 配置（接 i.MX6ULL 驱动学习线）；VENC→UVC 帧搬运 |
| 验收 | 光照变化时画质参数自动切换日志/画面 | 板子 USB 插 PC，Windows 识别为网络摄像头，能视频通话 |
| 优先级 | 要交画质时再做 | 当调剂玩，练内核侧手感 |

## 已盘点但缓办（遇到对应需求再翻）

- vdec+vo 解码回显（做远程回放时）；vgs/fisheye/dis（换广角镜头时）；hifb（接 HDMI 时）；tde（画框加速需求时）；awb_online_calibration / lsc_online_cali（配 PQ Tool 标定时）；资料盘 04.encode/05.decode、08.QT Porting（对应大佬 smarthome-master 的 Qt 播放器）
- live555：SDK 没有，资料盘有文件点播级 demo；产品化要标准 RTSP 时回头用
