# 13 · 底层点亮摄像头：MIPI、Sensor 驱动与"设备树"真相

> **一句话**：海思平台的摄像头点亮不走"设备树 + 平台驱动"那套 Linux 惯例——sensor 配置是**用户态三件套**（load 脚本参数 + 用户态 sensor 库 + sample 里的 MIPI 属性），内核侧只有五个通用 ko。理解了这个架构差异，80% 的"摄像头不出图"问题都能定位到具体层。

## 00 · 结论先行

| 问题 | 答案 | 证据 |
|---|---|---|
| 摄像头在设备树（DTS）里吗？ | ❌ **不在**。DTS 只管 UART/网口/EMMC 等 Linux 标准外设；MIPI/sensor/VI 全部走 MPP 私有架构 | 板端全盘无 sensor 相关节点，配置在 load 脚本参数里 |
| GC2053 的"驱动"是什么？ | **用户态库 `libsns_gc2053.so`**（寄存器配置序列）+ 内核态 `hi_sensor_i2c.ko`（用户态 I2C 通道）| `/ko/extdrv/`、`/usr/lib/libsns_gc2053.so` |
| 我们踩的两个出图坑？ | ① `lane_id {0,2,1,3}` → `{0,2,-1,-1}`（模组只接 2 条 lane）② `LANE_DIVIDE_MODE_0` → `_1`（2+2 分路） | `sample_comm_vi.c:556 / 2486`，第 4 篇笔记 |
| 点亮链哪一层最容易出错？ | **MIPI 物理层**（lane 接线/时序），因为它横跨硬件与软件 | `/proc/umap/mipi_rx` 的 DETECT/ERROR 表 |

## 01 · 架构对照：海思 MPP vs 传统设备树（i.MX6ULL 视角）

你在正点原子 i.MX6ULL 学的是"设备树描述外设 → compatible 匹配 platform driver → probe 注册"的路数。海思 MPP 是**另一套哲学**，逐项对照：

| 维度 | i.MX6ULL（通用 Linux） | Hi3516DV300（MPP 私有） |
|---|---|---|
| 外设描述 | 设备树节点 + compatible | **无节点**。参数走 load 脚本 insmod 参数 + sample 代码里的结构体 |
| 摄像头接口驱动 | DTS 里描述 csi/sensor → 驱动 probe | 内核只提供 **hi_mipi_rx.ko**（PHY 时序收包），语义零 sensor 知识 |
| sensor 驱动 | 内核态驱动（drivers/media/i2c/xxx.c） | **用户态 so**（libsns_gc2053.so），寄存器序列跑在应用进程里 |
| I2C 访问 | 内核 i2c_client | `hi_sensor_i2c.ko` 提供用户态直读直写通道，绕开标准 i2c 子系统 |
| 引脚复用 pinmux | 设备树 pinctrl | 编译进内核/uboot 的 pinmux 表 + PERI CRG 寄存器，用户不可配 |
| 为什么这么设计 | 通用、可移植 | **快**：省去 probe 流程，APP 直接 ioctl MPP 设备；ISP 算法参数可热更（PQ 工具的前提） |

> **为什么这对你重要**：在海思平台找"摄像头设备树"是找不到的。出图链的配置入口只有三个：`S00devs` 启动参数、`load3516dv300` 脚本、sample 代码里的 `combo_dev_attr_t`。排障时按这三处查，别去翻 DTS。

## 02 · 点亮全链路（从上电到第一帧 RAW）

```
上电
 └─ rcS → S00devs:
     ./load3516dv300 -i -sensor0 gc2053 -osmem 128 -total 1024
          │
          ├─ insmod sys_config.ko chip=hi3516dv300 sensors=sns0=gc2053,sns1=NULL,g_cmos_yuv_flag=0
          │        ↑ "当前接的是哪颗 sensor"从这进内核，决定 VI/ISP 的默认时钟与接口使能
          ├─ insmod hi_osal.ko / mpi.ko（MPP 基础设施）
          ├─ insmod hi3516cv500_{sys,vi,vpss,isp,venc...}.ko（各硬件模块）
          ├─ insmod hi_mipi_rx.ko          ← MIPI 物理层收包（/dev/hi_mipi）
          └─ insmod extdrv/hi_sensor_i2c.ko ← 用户态 I2C 通道
     应用进程（sample_venc）启动后：
          ├─ HI_MPI_SYS_Init / 各模块 Init
          ├─ 读 libsns_gc2053.so → 注册 ISP_SNS_OBJ（厂商 I2C 初始化序列 + AE 参数计算回调）
          ├─ HI_MPI_MIPI_SetDevAttr(lane_id{0,2,-1,-1}, RAW10) → PHY 开始等包
          ├─ HI_MPI_SENSOR_RegCallback → HI_MPI_ISP_Run → ISP 进程线程把 RAW 推成 YUV
          └─ VI Dev/Chn 出图 → VPSS → VENC → RTSP
```

**关键理解**："sensor 驱动"被拆成三段——
1. **内核 ko**（hi_mipi_rx、hi_sensor_i2c）：只管物理层和 I2C 通路，不认识 GC2053；
2. **用户态 so**（libsns_gc2053.so）：sensor 知识全在这（上电时序、I2C 初始化表 250+ 寄存器、AE 曝光换算公式）；
3. **sample 代码**（sample_comm_vi.c）：声明"这颗 sensor 怎么接"（lane、分辨率、RAW 位宽）。

三层必须互相咬合，任何一层错位 = 无图。

## 03 · MIPI RX 深入：lane_id 与 LANE_DIVIDE

### 3.1 lane_id 语义（官方 FAQ 原文）

> 数组的**索引号是 SENSOR 的 lane 编号，值是 MIPI RX 的 lane 编号**；sensor 没用到的 lane 配 `-1`。

```c
// sample_comm_vi.c:556（我们改过的地方）
.mipi_attr = {
    DATA_TYPE_RAW_10BIT,          // GC2053 输出 10bit RAW
    HI_MIPI_WDR_MODE_NONE,        // 非宽动态
    {0, 2, -1, -1}                // sensor lane0→MIPI lane0, lane1→MIPI lane2, lane2/3 未接
}
```

SDK 原版是 `{0, 2, 1, 3}`（4-lane sensor 的标准接法）。GC2053 模组**硬件上只焊了 2 条数据 lane**（走线在 D0 和 D2），所以 1、3 必须写 -1。配 4-lane 会怎样？PHY 一直在等 lane1/lane3 的时钟同步，永远凑不齐一帧 → DETECT 表里 0×0。

### 3.2 LANE_DIVIDE：4-lane PHY 怎么拆

Hi3516DV300 的 MIPI RX 物理上是 4×4-lane 的池子，`lane_divide_mode` 决定怎么分：

| 模式 | 划分 | 适用 |
|---|---|---|
| `LANE_DIVIDE_MODE_0` | 4+4（每 dev 占满 4 lane） | 4-lane sensor 双摄 |
| `LANE_DIVIDE_MODE_1` | **2+2** | 2-lane sensor 双摄（GC2053 就是） |

sample_comm_vi.c:2486 原版只在 imx327_2l 分支给 MODE_1，其它 sensor 走 MODE_0——我们的 else 分支也改成 MODE_1，否则 dev0 会按"4+4"占住前 4 lane，lane0/lane2 收不到独立时钟。这就是第 4 篇笔记里"改两处才出图"的第二处。

### 3.3 怎么验证（板子正在跑时的实测）

```sh
cat /proc/umap/mipi_rx
```

我们当前的输出（全文照录关键段）：

```
-----MIPI LANE DIVIDE MODE-----
  MODE         LANE DIVIDE
     1                 2+2          ← MODE_1 生效

-----MIPI DEV ATTR-----
   Devno  WorkMode  DataRate  DataType  WDRMode  ImgX ImgY ImgW ImgH
       0      MIPI       X1     RAW10      None     0    0   1920  1080

-----MIPI LANE INFO-----
   Devno        LaneID
       0      0,  2, -1, -1        ← lane 配置生效

-----MIPI DETECT INFO-----          ← 有数据的硬证据
 Devno VC   width  height
     0  0    1920    1080           ← VC0 正常收帧；VC1-3 全 0（未用）
     0  1       0       0

-----PHY CIL ERR INT INFO-----      ← 全 0 = 物理层干净
   Clk2TmOut ClkTmOut Lane0-3TmOut ... 全 0

-----MIPI ERROR INT INFO 1/2/3----- ← CRC/ECC/帧序 全 0
```

**判读口诀**：DETECT 有分辨率 = 收到包；ERROR 表全 0 = 链路干净；DETECT 全 0 = 先查 lane_id 和 MODE，再查 sensor 供电/时钟（`PhyData` 那行有值说明 PHY 活着）。

## 04 · Sensor 驱动剖析（GC2053 三件套）

VM 上源码：`mpp/component/isp/user/sensor/hi3516cv500/galaxycore_gc2053/`

| 文件 | 职责 | 关键内容 |
|---|---|---|
| `gc2053_sensor_ctl.c` | **I2C 寄存器序列** | `sensor_init()` 里 250+ 行 `sensor_write_register(0x0xxx, val)`——上电、PLL、MIPI lane 数、分辨率/帧率、mirror/flip 全靠它 |
| `gc2053_cmos.c` | **ISP 对接层** | `cmos_get_isp_default()`（3A 初始参数）、`cmos_set_ae_default()`（曝光/增益换算）、AWB 增益计算——AE 算法据此把亮度目标换算成 I2C 寄存器值 |
| `Makefile` | 编译成 `libsns_gc2053.so` | 应用启动时 dlopen 或静态链接进来 |

**运行机制**：`gc2053_cmos.c` 导出 `ISP_SNS_OBJ_S`（一套函数指针），`SAMPLE_COMM_ISP_Init` 把它注册给 ISP 库。ISP 每帧从硬件取 AE 统计 → 调 cmos 的换算函数得到新曝光 → 经 `hi_sensor_i2c.ko` 写 sensor 寄存器。**整条 3A 闭环跑在用户态**，这就是 PQ 工具能在线调参的架构前提（第 12 篇结尾的伏笔在这里闭合）。

**版本坑**：板端 `/usr/lib/libsns_gc2053.so` 已换成**厂商 2021 版**（.bak 存了 SDK 原版 2019）。不同批次 sensor 的初始化表可能不同——出图花屏/偏色先换 sensor 库版本试，这比调 IQ 便宜得多。

## 05 · 底层排障手册（按层查，从下往上）

| 症状 | 断层 | 查什么 |
|---|---|---|
| mipi_rx DETECT 全 0 | **物理层** | lane_id 配置？LANE_DIVIDE？sensor 供电/24M 时钟？换 sensor 库版本 |
| DETECT 有分辨率，VI 无图 | **接口层** | RAW 位宽/分辨率与 sensor 实际输出不符；`sample_comm_vi.c` 的 dev attr 用错模板 |
| VI 有帧但画面绿屏/花屏 | **数据层** | 位宽不对（RAW10 vs RAW12）；sensor 库初始化表与模组批次不匹配 |
| 出图但曝光乱跳/偏色 | **算法层** | cmos 换算与 IQ 不配（GC2053 无 IQ 文件，用默认参数，见第 5 篇） |
| load3516dv300 报错 | **ko 层** | sys_config 的 sensors= 参数拼错；osmem/total 内存划分与内核冲突 |

**核心工具就两个**：`/proc/umap/mipi_rx`（物理层）+ `/proc/umap/vi`（接口层）。断电到出图整条链 10 秒内跑完，逐层 proc 看一遍，断层立现。

## 06 · 遗留与下一步

- **双摄**：MODE_1 是 2+2，CSI0/CSI1 两个座子是否各占一组 2-lane，换口实测 mipi_rx 的 DETECT 即知（不用改任何代码，改 devno 就行）
- **换 sensor**：改三处——load 脚本 `sns0=` 参数、sample_comm_vi.c 里选对应的 dev attr 模板、/usr/lib 里放对应 libsns_xxx.so
- **IQ 精调**：走第 12 篇结尾的 PQ 工具链路线（资料盘已备齐）

## 07 · 概念速查

| 词 | 一句话解释 |
|---|---|
| **combo_dev_attr_t** | MIPI 接口描述结构体：lane_id、RAW 位宽、分辨率、WDR 模式全在这一个结构体里 |
| **lane_id[n]** | 索引=sensor lane，值=MIPI lane，-1=未接（FAQ 1.8.1） |
| **LANE_DIVIDE_MODE** | 4×4-lane PHY 的拆分方式：MODE_0=4+4，MODE_1=2+2 |
| **RAW10** | 10bit Bayer 原始数据，GC2053 的输出格式 |
| **ISP_SNS_OBJ_S** | sensor 库导出的函数指针集合，ISP 库与具体 sensor 的解耦接口 |
| **hi_sensor_i2c.ko** | 用户态 I2C 直通通道，让 so 里的寄存器序列能直达 sensor |
| **sys_config.ko** | 内核"全局参数"ko，sensor 型号从这里进内核 |
| **/proc/umap/mipi_rx** | MIPI 物理层黑匣子：DETECT（有没有包）、ERROR 表（包干不干净） |
