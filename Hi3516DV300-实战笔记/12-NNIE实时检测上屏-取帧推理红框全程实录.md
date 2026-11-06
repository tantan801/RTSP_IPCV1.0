# NNIE 实时检测上屏：取帧 → 推理 → 红框 → 水印修复（S2 全程实录）

> 目标：把 SSD 人形检测挂进 `sample_venc`，**VLC 画面里看到跟人动的红框**，同时水印、帧率一样都不能坏。
> 分三段施工：S2-1 取帧、S2-2 推理、S2-4 红框上屏（中间还埋了一个水印遮挡 bug）。
> 结构照旧：现象 → 取证 → 根因 → 修法 → 验收。

---

## 00 结论先行

| 阶段 | 做了什么 | 关键坑 | 验收结果 |
|---|---|---|---|
| S2-1 取帧 | VPSS 开第 3 路 300×300，检测线程 `GetChnFrame` | **VB 块虚拟地址是内核地址，用户态直接读必段错误** | 取帧稳定，主链路 6s ≥180 帧不掉 |
| S2-2 推理 | NV21→BGR planar → NNIE SSD → 解析 person 框 | static 函数要拷、tmpBuf 不能自己算、tmpfs 别塞两份 | 真人入镜 `persons=1` 置信度 0.67~0.84 |
| S2-4 红框 | 全屏 ARGB1555 overlay 挂 VENC，推理后重绘 | **本平台 overlay 不是标准 alpha 合成：透明像素会替换下层** | 红框跟人动，4px 空心框 |
| 水印修复 | 画框层盖住时间水印 | layer 大者在上层 → 水印层号必须高于画框层 | 像素级验证水印恢复显示 |
| 收尾 | 修 stop→start 失败 rc=255 | **根因不在代码，在脚本**：`kill -9` 强杀跳过全部清理 | 3 轮 stop→start 全通，退出 rc=0（§07） |

最终状态：**水印时间在走 + 红框跟人 + 主链路 6s 212 帧 + 单次推理 ~257ms 不掉帧**，
并且进程退出可被脚本反复 stop→start（§07 修复）。

---

## 01 S2-1：给检测器开一条专用取帧通道

### 1.1 改动面（先读源码核定，再动手）

| 位置 | 原样 | 改后 | 为什么 |
|---|---|---|---|
| `sample_venc.c:1488` | `SIZE_S stSize[2]` | `stSize[3]` | VPSS_MAX_PHY_CHN_NUM=3，chn2 是**最后一路**，数组不扩会读到栈垃圾 |
| `sample_venc.c:1506` | `abChnEnable = {1,1,0}` | `{1,1,1}` | 使能 chn2 |
| chn2 属性 | — | 300×300 | SSD 模型输入尺寸 |
| VB 池 | 2 个池 | 加第 3 池（300×300 NV21 × 4），`u32MaxPoolCnt = 3` | 检测图走公共池，尺寸与通道属性一致，不压缩 |

> 坑伏笔：配置写 300×300，海思按 16 对齐后**实际输出的帧是 320×320**（stride 也随之对齐）。后面所有尺寸计算都要用 `stFrame.stVFrame.u32Width/u32Stride` 里的实际值，不要用配置值。

### 1.2 最大坑：`VB_REMAP_MODE_NONE` 下读虚拟地址 = 段错误

现象：检测线程一启动进程直接死，退出码 **rc=139**（SIGSEGV），一行错误信息都没有。

当时的排查绕了三圈——怀疑 MMZ 残留、怀疑块太小、怀疑池数量——全是**连锁后果**，不是根因。根因一条就够：

```
系统配置 VB_REMAP_MODE_NONE 时，
stFrame.stVFrame.u64VirAddr 里给的是【内核虚拟地址】(形如 0xca800000)，
用户态进程拿它当指针一解引用，页表里没有映射 → SIGSEGV。
```

修法：**不信 u64VirAddr，拿物理地址自己映射**：

```c
/* VB 池给出的 virAddr 是内核地址,用户态不可直接访问,
   必须用物理地址自己映射一次 */
u32MapSize = u32Stride * u32H * 3 / 2;                    /* NV21: Y+VU */
pMapped = HI_MPI_SYS_Mmap(stFrame.stVFrame.u64PhyAddr[0], u32MapSize);
...
pu8Y = (HI_U8 *)pMapped;                                  /* 从这里读 Y 平面 */
...
HI_MPI_SYS_Munmap(pMapped, u32MapSize);
HI_MPI_VPSS_ReleaseChnFrame(DET_VPSS_GRP, DET_VPSS_CHN, &stFrame);
```

**判据以后固定下来**：段错误 + rc=139 + 地址形如 `0xca…`，第一反应查"是不是内核地址"，不要先去翻内存池账本。

### 1.3 检测线程骨架

```c
while (s_bDetectRun) {
    GetChnFrame(grp, chn, &stFrame, 200ms);   /* 取 VPSS chn2 的帧 */
    Mmap(物理地址);                            /* 映射到用户态 */
    nnie_detect_run(pu8Y, w, h, stride);      /* 每 6 帧推理一次 ≈5fps */
    Munmap;
    ReleaseChnFrame;                          /* 必须还块,否则 VB 池被抽干 */
}
```

节流策略：主链路 30fps，检测**每 6 帧做一次推理**（约 5fps），既省 NPU 时间也省 CPU 的颜色转换时间。

### 1.4 验收

```
[DET] #6 320x320 stride=320 ...      ← 取帧稳定
/proc/umap/venc: 6s ≥180 帧          ← 主链路不掉帧
/proc/umap/vb: 各池 MinFree ≥5       ← 公共池没被检测侧抽干
```

---

## 02 S2-2：把 SSD 推理接上

### 2.1 SDK demo 的代码哪些能用、哪些必须拷

`svp/nnie` 的 sample 拆得很散，链接前先分清：

| 函数 | 来源 | 原因 |
|---|---|---|
| `Forward` / `SoftwareInit` | sample_nnie.c 里是 **static**，链接不到 → **拷进 nnie_detect.c** | static 只在本文件可见 |
| `Ssd_GetResult` / `Ssd_GetResultTmpBuf` / `LoadModel` / `ParamInit` | 公共库（sample_nnie_software 等）直接链 | 本来就是对外符号 |

### 2.2 数据通路：VPSS 出 NV21，SSD 吃 BGR planar

```
VPSS chn2 (320×320 NV21)
   ↓ CPU 最近邻缩放 + BT.601 limited YUV→BGR
BGR planar 300×300×3 = 270000 B（正好对上模型输入账本）
   ↓ 填进 src blob（海思要求 16 对齐 → stride=304）
FlushCache → HI_MPI_NNIE_Forward(seg0, bInstant=TRUE)
   ↓
Ssd_GetResult → 按 score>0.5 过滤 → 只留 class15(person)
```

两个易错点：

- **`Ssd_GetResultTmpBuf` 别自己算**：第一次按三块 tmpBuf 自定义分块，推理出乱码结果。核实 SDK 源码后发现**函数内部已按模型参数填好 size**，自己再算一遍反而错位。删掉自定义逻辑，回归原版调用。
- **bInstant=TRUE**：单段网络且要立刻拿结果，不加这个会等不到输出。

### 2.3 三个部署坑

| 坑 | 现象 | 修法 |
|---|---|---|
| tmpfs 塞两份模型 | 27MB 模型传两份把 48MB tmpfs 塞满，tftp 写失败 | 模型只存 `/mnt/nnie/` 一份 |
| telnet 传大文件截断 | 默认 6s 超时只传出 1.7MB | 传模型这类大文件用**长超时专用通道**（~400s） |
| Makefile wildcard | `$(wildcard *.c)` 把**没有 main 的** nnie_detect.c 也当可执行 target 链接，报 undefined | TARGET 改**固定列表** `sample_venc rtsp_server osd_test`，SVP 源码走 `SMP_SRCS` 附加，库补 `libive/libmd/libnnie.a` |

### 2.4 降级设计

模型加载失败（比如重启后 tmpfs 里的模型没了）不致命：

```c
if (HI_SUCCESS == nnie_detect_init(HI_NULL))
    printf("[DET] NNIE model ready\n");
else
    printf("[DET] NNIE init failed, fallback to luma-only\n");
    /* 降级: 只统计画面亮度,证明取帧链路还活着 */
```

> tmpfs 里的模型**重启即丢**。设备化部署时要考虑自动恢复（见 §06 遗留）。

### 2.5 验收

```
[DET] #156 infer 293ms persons=1 (0.84,101,39,215,262)   ← 人走进画面
[DET] #186 infer 254ms persons=0                          ← 人离开
主链路 6s 212 帧                                          ← 不掉帧
```

置信度 0.67~0.84，框坐标稳定——**NPU 实时链路第一次在自家进程里跑通**。

---

## 03 S2-4：红框画到 VLC 画面上

### 3.1 方案：全屏 overlay + 推理后重绘

不是给 VENC 再开一路，而是**复用 OSD 的 RGN 机制**：

```
RGN handle=1, 画布 canvas=2（时间水印是 handle=0/canvas=1）
1920×1080 ARGB1555 全屏画布, layer=1
   ↓ 绑定 VENC chn0
每次推理结束 → 重绘画布: 全透明底 + 红色空心矩形(4px)
   ↓ 坐标反算
x_1080p = x_300 × 1920/300 , y_1080p = y_300 × 1080/300
```

ARGB1555 纯红 = `0xFC00`（A=1、R=11111、G/B=00000）；画布其余像素 A=0，配合 `u32FgAlpha=128 / u32BgAlpha=0`。

### 3.2 平台特性：这不是标准 alpha 合成（本篇最重要的硬件认知）

标准 alpha blending 里，透明像素应该"露出"下层。**本平台 VENC 多 overlay 的实际行为**：

1. **layer 值大的在上层**（小号在下）
2. 上层的**透明像素不是混合，而是替换**下层内容

也就是说：上层的"透明"不是 0% 覆盖，而是"这格归我，涂成透明色"。谁 layer 大，谁的画布就整个生效——红框层会把下层水印的对应位置抹掉。

### 3.3 由此翻车的 bug：水印消失

现象：红框正常，**时间水印不见了**。

根因一行：水印 RGN 的 `u32Layer = 0`，画框 RGN 的 `u32Layer = 1` → 画框层整体压在画面最上，水印像素被"透明"替换。

修法：`osd_test.c:324` 把水印层号抬高：

```c
u32Layer 0 → 2      /* 水印(2) 在画框(1) 之上 */
```

### 3.4 像素级验收：没有 PIL 怎么验水印

VLC 截图是压缩画面，肉眼确认不硬。写了个**纯标准库 PNG 解码器**（zlib inflate + 反 filter，几十行），直接读 VLC 抓的帧，检查水印区域 (64,64)-(264,104) 的暗像素数量：

```
修复前: dark 像素 ≈ 0     → 水印确实被盖掉
修复后: dark 像素 = 1269  → 水印恢复
```

> 教训：**视觉验证也要有数字判据**。没有图像库就手写解码器，几十行换一个可信的 pass/fail。

### 3.5 终验（四条全绿）

| 验收项 | 实测 |
|---|---|
| VLC 画面有框且跟人动 | ✅ 人走框走 |
| 时间水印在走 | ✅ 抓帧里看到 14:38:14 |
| 主链路帧率 | ✅ 6s 212 帧（≥180 达标） |
| 推理耗时 | ✅ ~257ms，VENC 无 Full、无 BusyCnt 增长 |

---

## 04 全链路数据流（S2 完成后的样子）

```
GC2053 → MIPI → VI → VPSS ─┬─ chn0 1080p ──→ VENC ──→ RTSP → VLC
                           │                    ↑      ↑
                           │              RGN overlay  RGN overlay
                           │              layer1 红框   layer2 水印
                           ├─ chn1 码流(备用)
                           └─ chn2 320×320 → DET线程(每6帧) → mmap
                                              → CPU转BGR → NNIE SSD
                                              → 框坐标 → 重绘红框
```

主链路一行代码没动逻辑——检测是**旁路**：从 VPSS 借帧、还帧，只在 VENC 的 overlay 上画东西。这是"往在线链路加功能"的标准姿势：**加旁路，不改干路**。

---

## 05 方法论

### 5.1 段错误先查地址空间，再查资源

rc=139 那次绕了三圈查内存池，真相是"内核地址不可解引用"。**错误类型本身已经缩小了范围**：SIGSEGV = 地址错了，不是"资源少了"。

### 5.2 SDK demo 源码是第一手资料

三次踩坑都是"凭理解写"输给了"照源码抄"：static 函数链接不到、tmpBuf 内部已填 size、ARGB1555 alpha 语义。**海思 sample 不是示例，是规范**——它就是这个平台的唯一标准答案。

### 5.3 往在线链路加功能 = 加旁路

检测线程全程只做三件事：借帧、算、画 overlay。主链路（VI→VPSS chn0→VENC→RTSP）的代码逻辑零改动，帧率账本（6s ≥180）从头到尾当作回归测试跑。**有基线指标的功能升级，才敢说"没退化"**。

### 5.4 部署自动化要幂等

整条链（改源码 → 编译 → tftp → 板端替换 → 重启 → 验收）全部脚本化，且重复执行安全（补丁先探测锚点是否已存在）。一天内十几轮"改-编-传-测"，靠的就是这个。

### 5.5 脚本的"杀法"决定代码的清理是否生效

`stop` 里一个 `-9` 和 `TERM` 的差别，等于"清理代码执行"与"清理代码不存在"。出这类问题时**先看谁在杀进程、用什么信号杀**，再去看退出路径写了什么——§06 那次就是先怀疑代码、白找半天，真相在脚本第 82 行。

---

## 06 收尾修复：stop→start 失败 rc=255（原判断被推翻）

### 6.1 现象

S2 期间每次部署都要 `stop` → 换二进制 → `start`。多次出现：stop 之后 start **永久失败**，日志无限刷：

```
[VB] p0=4181760 x10 | p1=866304 x10 | p2=153600 x4 | maxpool=3
[SAMPLE_COMM_SYS_Init]-373: HI_MPI_VB_SetConf failed!
[SAMPLE_VENC_SYS_Init]-1244: SAMPLE_COMM_SYS_GetPicSize failed!
[SAMPLE_VENC_H265_H264]-1703: Init VI err for 0xffffffff!
program exit abnormally!
```

`HI_MPI_VB_SetConf` 失败 = **VB 公共池还留在内核模块里**，新进程没法重新配置。只有 `reboot` 能恢复。

### 6.2 原来的判断（错的）

> "SIGTERM 退出路径没调 `HI_MPI_VB_Exit`，退出时要补 MPP 反初始化。"

这个判断错得很彻底。读源码核实：`SAMPLE_VENC_HandleSig()` 里明明有完整清理链：

```c
void SAMPLE_VENC_HandleSig(HI_S32 signo)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    if (SIGINT == signo || SIGTERM == signo)
    {
        SAMPLE_COMM_VENC_StopSendQpmapFrame();
        SAMPLE_COMM_VENC_StopGetStream();
        SAMPLE_COMM_All_ISP_Stop();
        SAMPLE_COMM_SYS_Exit();          /* 内含 HI_MPI_SYS_Exit + HI_MPI_VB_Exit */
        printf("program termination abnormally!\n");
    }
    exit(-1);
}
```

而 `SAMPLE_COMM_SYS_Exit()` 的实现里确实有 `HI_MPI_VB_Exit()`。**代码没毛病，是它根本没被调用。**

### 6.3 取证：旧进程死前一个字都没留

稳定复现后翻旧进程的日志：**没有 `termination abnormally`、没有 `[DET] thread exit`**——任何清理路径的痕迹都没有。同时 `ps` 里进程确实已经消失。

这个组合只有一种解释：**进程是被 SIGKILL 杀的**。SIGKILL 不可捕获、不可阻塞，进程没有任何机会执行 handler。

回头查 `S99rtsp`：第 11 篇为了"杀干净"，把 `clean_all` 全改成了 `kill -9`：

```sh
killall -9 -q sample_venc
sleep 1
killall -9 -q sample_venc
```

> **加固加固，把功能也一起加固没了。** 第 11 篇解决的是"stop 杀不掉老 watcher"，改成 `-9` 确实杀得干净，代价是**跳过了所有清理**。两个目标冲突时，选了粗暴的那个。

### 6.4 修法（三处）

**① `clean_all`：先礼后兵**

```sh
# 先 TERM 让 HandleSig 走完 VB_Exit；直接 -9 跳过清理，VB 池残留内核
killall -TERM sample_venc 2>/dev/null
i=0
while [ $i -lt 10 ] ; do
    pgrep -x sample_venc >/dev/null 2>&1 || break
    sleep 1 ; i=$((i+1))
done
killall -9 -q sample_venc 2>/dev/null      # 兜底：10 秒还不死才强杀
```

SIGTERM 可捕获 → handler 有机会跑完 → `VB_Exit` 执行 → 池干净。`-9` 降级为"10 秒超时兜底"，不再是首选。

**② `HandleSig` 的退出码：`exit(-1)` → `exit(0)`**

`-1` 在 shell 里是 **255**，而 255 正好是"VB 池残留"的特征码。优雅退出记成失败，会让守护进程的失败计数误判。改 `exit(0)`，**把 rc=255 保留为 VB 残留的专属信号**。

**③ 守护循环：连续 10 次 rc=255 自动重启**

万一将来又被别的方式强杀（`kill -9` 手工、OOM killer），池残留恢复不了。与其无限重试，不如自愈：

```sh
if [ $fail -ge 10 ] && [ $rc -eq 255 ] ; then
    blog "daemon: 10x rc=255, VB pool stuck, reboot to recover"
    sleep 2 ; reboot -f ; exit 0
fi
```

### 6.5 验证

| 验证项 | 结果 |
|---|---|
| 3 轮 `stop`→`start` | ✅ 全部成功，日志 `NO_ERRORS`，554 立即 LISTEN |
| 优雅退出证据 | ✅ 单独 TERM 实验：watch 记录 `exited rc=0`，daemon 自动拉起新进程 |
| 模型恢复后推理 | ✅ `infer 282ms` 正常 |

副产物踩坑：`cp` 覆盖 `/usr/bin/sample_venc` 后忘了 `chmod +x`（tftp 下载件权限是 644），启动报 `Permission denied` rc=126。

### 6.6 残留的小谜团（不影响功能）

`HandleSig` 里的 `termination abnormally` 打印始终没落盘。推测是信号上下文里 stdio 的 flush 行为不保证（handler 不是异步信号安全的）。功能证据是硬的：rc=0 + VB 池干净 + 后续 start 成功。

> 教训：**信号处理器里只该置标志位，真正的清理放主线程做**。厂商标配的这套 `exit()` in handler 能跑通是运气。

---

## 07 剩余待办

| 项 | 现状 | 方案 |
|---|---|---|
| 模型开机丢失 | `/mnt/nnie` 是 tmpfs，重启即丢，目前靠降级 luma-only 活着 | 模型挪 jffs2 外的持久分区，或 S99rtsp 开机自动 tftp 回灌 |
| 检测框标签（可选） | 只有框没有 "person 0.84" 文字 | 复用 OSD 点阵字体画在框角 |

---

## 附：本次涉及的文件

| 文件 | 改动 |
|---|---|
| `sample/venc/sample_venc.c` | stSize[3]、abChnEnable{1,1,1}、VB 第 3 池、DetectProc 重写（mmap 方案）、DRAW 三处挂钩 |
| `sample/venc/nnie_detect.c/h` | 新建：模型加载、NV21→BGR、Forward、Ssd_GetResult、DRAW 重绘 |
| `sample/venc/osd_test.c:324` | 水印 `u32Layer 0 → 2` |
| `sample/venc/Makefile` | TARGET 固定列表 + SVP 源/库 |
| 板端 `/etc/init.d/S99rtsp` | `clean_all` 改"TERM 优先 + -9 兜底"，守护循环加 rc=255 自愈重启 |
| 板端 `/mnt/nnie/inst_ssd_cycle.wk` | 模型（tmpfs，重启丢失，需 tftp 回灌） |

配套脚本：`patch_s21/s22/s24.py`（源码补丁）、`deploy_s22/s24.py`（编译+部署+验收）、`png_check.py`（纯 stdlib PNG 像素验证）、`venc_fps.py`（帧率账本）。
