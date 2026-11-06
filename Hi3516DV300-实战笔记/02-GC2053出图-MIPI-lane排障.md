# 02｜GC2053 出图排障：MIPI lane 映射 + lane_divide_mode

> 现象：I2C 通信正常、打印 `GC2053 init succuss!`，但完全不收图
> 结论：本板 CSI0 物理走线是 lane 0/2，SDK 默认按 lane 0/1 配置；且 2lane 模组需要 2+2 分割

---

## 1. 数据链路先建立起来

排任何"不出图"的问题，先在心里画出这条链，再逐级取证：

```
GC2053 sensor
   │  MIPI CSI-2（2 lane：CLK + D0 + D1）
   ▼
MIPI RX（PHY）           ← 关卡1：有没有收到有效包？/proc/umap/mipi_rx
   ▼
VI DEV（mipi dev 0）     ← 关卡2：属性是否与 sensor 匹配（data type / 分辨率 / lane）
   ▼
VI PIPE（含 ISP）        ← 关卡3：ISP 是否在跑？中断频率对不对
   ▼
VI CHN → VPSS → VENC     ← 关卡4：帧率、VB 是否够
```

任何一级为 0，问题就在这一级或它的上游。

### 1.1 链路上流动的到底是什么数据（四个必懂概念）

排 MIPI 问题前，先知道"正常的数据长什么样"。

#### ① 像素格式：从 RAW 到 YUV

sensor 出来的不是图像，是**每个像素只有一种颜色的原始数据（Bayer）**——这就是为什么叫 RAW：

```
RAW Bayer（每像素 1 个颜色分量，RGGB 排列）
   │ ISP：去马赛克 + 3A + 降噪 + 色彩校正
   ▼
YUV420SP / NV12（每像素 1.5 字节）← VPSS、VENC 真正吃的格式
```

| 格式 | 每像素 | 1080p 单帧 | 出现在哪 |
|---|---|---|---|
| RAW10 | 1.25 B | 2.53 MB | sensor→MIPI→VI 内部 |
| **NV12（YUV420SP）** | **1.5 B** | **3.11 MB** | VPSS/VENC 主力格式 |
| YUV422 | 2 B | 4.15 MB | 特定场景 |
| RGB888 | 3 B | 6.22 MB | OSD、显示 |

NV12 的内存布局：`Y 平面 1920×1080` + `UV 交错平面 1920×540`，所以是 1.5 字节/像素。

> **3.1MB/帧** 这个数要记住：算 VB 池、算码率、算 DDR 带宽全靠它。30 帧缓冲 ≈ 93MB。

#### ② 位深：8bit 与 10bit 不能混

GC2053 是 **RAW10**，SDK 里的选型名 `..._10BIT` 就是它。

- 10bit 动态范围更大（暗部细节多 4 倍量化级）
- 但海思内部 VPSS/VENC 仍按 8bit 处理，多出的位被压掉
- **`mipi_attr` 里必须写 `DATA_TYPE_RAW_10BIT`**，写成 `RAW_8BIT` → PHY 按错误位宽解析 → DETECT INFO 为 0

这是"格式配错也表现为完全没图"的典型：**不是没数据，是解析不出合法包**。

#### ③ MIPI 速率估算（判断 lane 配置合不合理）

```
单 lane 速率 ≈ 宽 × 高 × 位深 × 帧率 ÷ lane 数
GC2053 1080p30 RAW10 2lane:
  1920 × 1080 × 10 × 30 ÷ 2 ≈ 311 Mbps/lane
```

MIPI 有效载荷约 80%，所以 PHY 能力需 ≥ 400Mbps/lane。这个数对不上（比如配 4lane 实际接 2lane），就会出现"PHY 在等同步码、永远等不到"——正是本篇第 4 节的 `LANE_DIVIDE_MODE_0` 问题。

#### ④ 曝光时间与帧率是绑死的

sensor 是**逐行读出**的，行时间（line time）固定：

```
曝光时间 ≤ 帧周期  → 正常帧率
曝光时间 > 帧周期  → 必须拉长帧间隔，帧率下降
```

所以"夜里帧率掉到 12fps"是**物理规律**，不是 bug——AE 为了拿到足够亮度自动延长曝光，30fps 就撑不住了。

判据：
- `dmesg | grep cmos_fps_set` 有没有 `Not support Fps` 报错
- `/proc/umap/vi` 的 `FrameRate` 是否随光照变化（白天 30、夜里 12 → 就是长曝光）
- 想强行锁帧率：关掉 AE 或设最大曝光时间上限（sensor 库里 `ae_get_max_exp` 之类）

---

## 2. 故障现场（修复前）

程序不报错，但：

| 检查点 | 现象 |
|---|---|
| `dmesg` | `GC2053 init succuss!`（**这个不能信，见第 5 节**） |
| `/proc/umap/mipi_rx` → DETECT INFO | 全 0（VC0~VC3 的 width/height 都是 0） |
| `MIPI PHY DATA INFO` | `MipiData 0x00`（正常应为非 0） |
| VI PIPE STATUS | 无中断增长，ISP IntCnt = 0 |
| VENC | 刷屏 `get venc stream time out` / `QueryStatus 0xa0088010` |

**DETECT INFO 全 0 是关键判据**：说明 PHY 根本没收到能被解析的 MIPI 数据包，问题在 sensor→PHY 这一段，而不是 ISP 或编码。

---

## 3. 根因一：lane_id 与实际走线不匹配

`smp/a7_linux/mpp/sample/common/sample_comm_vi.c`，GC2053 的 combo dev 属性（**第 556 行**）：

```c
combo_dev_attr_t MIPI_2lane_CHN0_SENSOR_GC2053_10BIT_2M_NOWDR_ATTR = {
    .devno = 0,
    .input_mode = INPUT_MODE_MIPI,
    .data_rate = MIPI_DATA_RATE_X1,
    .img_rect = {0, 0, 1920, 1080},
    .mipi_attr = {
        DATA_TYPE_RAW_10BIT,
        HI_MIPI_WDR_MODE_NONE,
        {0, 1, -1, -1}    // ← SDK 默认：用 PHY 的 lane0/lane1
    }
};
```

**`lane_id` 数组的语义**：它是「MIPI 数据 lane 序号 → PHY 物理 lane 编号」的映射表，`-1` 表示不用。

本板（EB-Hi3516DV300-DC-182）的 CSI0 走线是 **lane 0 和 lane 2**（lane 1/3 分给了第二路 CSI），所以必须写：

```c
{0, 2, -1, -1}
```

厂商提供的 `mpp_0727` 包里这一行本来就是 `{0, 2, -1, -1}`，而 `{0,1,...}` 那行是**被注释掉的**——说明这个差异是厂商针对板子改过的，原厂 SDK 没跟。

---

## 4. 根因二：lane_divide_mode 必须是 2+2

同一个文件里 `SAMPLE_COMM_VI_GetMipiLaneDivideMode()`：

```c
switch (enSnsType) {
    case SONY_IMX327_2L_MIPI_2M_30FPS_12BIT:
    case SONY_IMX327_2L_MIPI_2M_30FPS_12BIT_WDR2TO1:
        lane_divide_mode = LANE_DIVIDE_MODE_1;   // 2+2
        break;
    default:
        lane_divide_mode = LANE_DIVIDE_MODE_0;   // ← 4 lane 全给一个 dev，错
        break;
}
```

**概念**：MIPI RX 有 4 条物理 lane，`lane_divide_mode` 决定怎么切分给两个（或多个）MIPI dev：

| 模式 | 含义 |
|---|---|
| `LANE_DIVIDE_MODE_0` | 4 lane 全部给 dev0（单路 4lane） |
| `LANE_DIVIDE_MODE_1` | **2 + 2**：dev0 用 lane0/1，dev1 用 lane2/3 |
| `LANE_DIVIDE_MODE_2` | 1 + 1 + 1 + 1 等其它切法 |

GC2053 是 **2lane** 模组，如果按 MODE_0（4lane）配置，PHY 会等 4 条 lane 的同步码，永远等不到，于是 DETECT INFO 全 0。

改法：把 else 分支也改成 `LANE_DIVIDE_MODE_1`（等于对 2lane sensor 固定 2+2，与厂商版一致）。

> 两个改动**必须同时存在**，缺任何一个都不出图——这是本次排查最耗时的点。

---

## 5. 附带结论：厂商的 "init succuss" 不可作为识别证据

**实验**：把 sensor 模组整个拔掉，程序照样打印

```
=== Galaxycore GC2053_1080P_30FPS_10BIT_LINE_Init_OK!===
GC2053 init succuss!
```

说明 `libsns_gc2053.so` **不做芯片 ID 校验**（或校验失败也打印成功）。

可靠判据：

| 想确认什么 | 看什么 |
|---|---|
| sensor 是否真的在 I2C 上 | I2C 读失败会打 `wait idle abort! RIS: 0x611` 之类错误 |
| sensor 是否真的在出数据 | `/proc/umap/mipi_rx` 的 DETECT INFO width/height 非 0 |
| ISP 是否真的在跑 | `/proc/umap/vi` 的 `IntCntPerSec` ≈ 30 |

---

## 6. 辅助：换厂商的 sensor 库

板端 `/usr/lib/libsns_gc2053.so` 有两个版本：

| 版本 | 大小 | md5 |
|---|---|---|
| SDK 原厂 | 67,640 | `3d12d9ea…` |
| 厂商 2021-02-06 | 67,640 | `a41b4217f0a8a04d23345bd9e42544b9` |

**大小完全一样但 md5 不同**——所以"文件存在且大小对"不能证明版本对。替换前先 `md5sum` 记录，备份成 `.bak`。

### 6.1 为什么换个 .so 就能改变出图行为

这就是第 1 篇说的"sensor 驱动是用户态库"的实际后果：

```
sys_config.ko（内核）
   │ 拿到字符串 "gc2053"
   ▼
ISP 用户态进程  ──dlopen("libsns_gc2053.so")──▶  函数表
                                                ├─ gc2053_init()
                                                ├─ gc2053_start()
                                                ├─ gc2053_set_fps()
                                                └─ gc2053_get_max_exp()
```

- **换 `.so` = 换整套 sensor 时序和 AE 策略**，不用重编内核、不用重烧固件
- 这也是为什么厂商给了 2021 版库就能解决某些问题：寄存器序列、lane 配置、AE 曲线都在库里
- 风险在于**它是个黑盒**：`init succuss` 不代表真成功（第 5 节已证），只能靠 `/proc` 反证

动态库的三条验证手法：

```bash
file libsns_gc2053.so          # 必须是 ELF 32-bit ARM，EABI5，hard-float
md5sum libsns_gc2053.so        # 版本唯一可靠判据
strings libsns_gc2053.so | grep -i "2021\|version"
```

> ABI 必须匹配：同为 `arm-himix200-linux` 工具链产物、同一套 glibc 符号。若库是软浮点或 uclibc 编的，`dlopen` 会失败或一调用就崩。

---

## 7. 用"对照实验"快速划分软硬件责任

这是本次最高效的一步：

```
厂商提供的独立 demo：03.Seneor/GC2053旧/sample_vio_mipi0（静态链接厂商库）
→ 一跑就出 DETECT INFO 1920x1080
```

结论链条：
1. 硬件（模组 + 排线 + CSI 口 + 供电 + MCLK）**全部正常**
2. 问题一定在**软件配置**（SDK 的 lane/lane_divide 与厂商 demo 不同）

于是把排查范围从"硬件 8 种可能"压缩到"两行配置"，几分钟定位。

**方法论**：手上有厂商 demo 时，先跑它。跑通 = 硬件无罪，跑不通 = 硬件/底层问题，避免在错误的方向上折腾排线。

---

## 8. 修复后的验证

```bash
# 1. MIPI 属性
cat /proc/umap/mipi_rx
```

期望输出：

```
-----MIPI LANE DIVIDE MODE-----
  MODE         LANE DIVIDE
     1                 2+2            ← MODE_1 生效

-----MIPI DEV ATTR-------------
   Devno  WorkMode  DataRate   DataType  WDRMode  ImgX ImgY ImgW ImgH
       0      MIPI        X1      RAW10     None     0    0 1920 1080

-----MIPI LANE INFO------------
   Devno                  LaneID
       0         0,  2, -1, -1        ← lane 0/2 生效

-----MIPI DETECT INFO---------
     0  0     1920    1080           ← 真的在收图了
```

```bash
# 2. 出帧
cat /proc/umap/vi | sed -n '/VI PIPE STATUS/,+3p'
# IntCnt 持续增长、FrameRate=30、LostFrame 很小
```

```bash
# 3. 出码流
ls -l stream_chn0.h265   # 录 10 秒应有明显增长
```

---

## 9. 排查清单

- [ ] 先跑厂商独立 demo，确认硬件无罪
- [ ] `mipi_rx` DETECT INFO 是否非 0（判断 PHY 有无收到包）
- [ ] `lane_id` 是否与本板物理走线一致（问原理图，别猜）
- [ ] `lane_divide_mode` 是否与 lane 数匹配（2lane → 2+2）
- [ ] sensor 库 md5 是否真是要用的那一版
- [ ] 改 `common/*.c` 后是否 `rm -f ../common/*.o` 并用 md5 验证
