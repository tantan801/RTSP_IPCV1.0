# 01｜SDK 编译与 sample 移植（从零到编出可跑的 sample_venc）

> 平台：EB-Hi3516DV300-DC-182 / Hi3516CV500_SDK_V2.0.2.0
> 目标：编出一个能驱动 GC2053 出图的 `sample_venc`

---

## 1. 先搞清 SDK 的目录结构

海思 MPP 的目录层级容易踩坑，**顶层 `mpp/` 不是真正的源码目录**：

```
Hi3516CV500_SDK_V2.0.2.0/
└── smp/a7_linux/mpp/        ← 真实路径（下面所有操作都在这里）
    ├── Makefile.param       ← 全局编译参数（sensor 选型在这里）
    ├── Makefile.linux.param
    ├── cfg.mak
    ├── include/             ← MPP 头文件
    ├── ko/                  ← 内核模块
    ├── lib/                 ← 用户态库（含 libsns_*.a/.so）
    ├── sample/              ← 样例代码（venc/vio/vpss/audio/uvc…）
    ├── component/           ← isp、hifb
    └── tools/
```

**重要背景**：Hi3516DV300 与 Hi3516CV500 是同一颗 die，共用一份 SDK。所以 `ko` 文件名全带 `hi3516cv500_` 前缀，这是正常的，不是拿错 SDK。

### 1.1 MPP 的软件模型：五个硬件单元 + 一个内存池

MPP 不是一个"库"，而是一整条硬件流水线的软件封装。把这套模型搞懂，所有 sample 都能读：

```
Sensor → MIPI RX → VI → VPSS → VENC → 码流 / 显示 / 智能
                    │      │       └→ VO（送屏）
                    │      ├→ VGS（缩放/格式转换/拼接）
                    │      └→ NNIE（神经网络推理）
                    └→ ISP（3A、去马赛克、降噪、宽动态）
```

| 单元 | 职责 | 关键概念 |
|---|---|---|
| **VI**（Video Input） | 接收 MIPI/BT1120 数据，做 ISP 处理 | **dev / pipe / chn 三级模型** |
| **VPSS**（Video Process） | 缩放、裁剪、格式转换、帧率控制、OSD 叠加 | 多通道（chn0/chn1…）可各出不同分辨率 |
| **VENC**（Video Encoder） | H.264/H.265/JPEG 编码 | 通道、GOP、码率控制 |
| **VGS**（Video Graphics） | 纯硬件缩放/格式转换/拼接 | 无通道概念，一次性调用 |
| **VO**（Video Output） | 送 HDMI/BT1120/MIPI 屏 | 设备 + 图层 |
| **NNIE** | 神经网络硬件加速 | 输入必须是特定格式的 YUV |

#### ① Dev / Pipe / Chn 三级模型（VI 最典型）

```
DEV0（一个物理 MIPI 口）
  └── PIPE0（一路处理管线，含 ISP）
        └── CHN0（输出通道，可出 1080p）
        └── CHN1（输出通道，可出 720p）
```

- **DEV** = 物理接口属性 → 我们改的 `lane_id`、`img_rect` 是这一层
- **MIPI RX（PHY）** = 更底层，`lane_divide_mode` 是这一层（见第 2 篇）
- **PIPE** = 处理属性（bayer 格式、位深、WDR）→ 配错就是颜色不对
- **CHN** = 输出属性（分辨率、帧率、裁剪）

排障时先确定错在哪一层，再去改对应结构体——这是第 2 篇能快速定位的底层原因。

#### ② 绑定（bind）与用户态取帧（user mode）

VI→VPSS→VENC 用 `HI_MPI_SYS_Bind()` 连好后，帧在硬件内部直接流转，**一个字节都不经过 CPU**，所以 CPU 占用极低、延迟最小。

一旦解绑自己取帧（`HI_MPI_VPSS_GetChnFrame`），就进入 **user mode**：

| | bind 模式 | user mode |
|---|---|---|
| 帧的搬运 | 硬件 | CPU 拷贝/映射 |
| 谁是帧的主人 | 系统自动 | **你** |
| 必须做什么 | 无 | **必须 `ReleaseChnFrame` 还回去** |
| 典型场景 | 纯编码推流 | NNIE 推理、抓拍、自定义处理 |

> **忘记 Release = VB 池被慢慢抽干 = 全链丢帧**。第 5 篇那个 `VbFail` 讲的正是这个机制的另一面。

#### ③ MMZ：所有图像帧的家

`load3516dv300 -osmem 128 -total 1024` 的真实含义：

```
DDR 1GB
├─ 128MB  → Linux（-osmem 128）  ← free 看到的 MemTotal ≈ 121MB
└─ 896MB  → MMZ（媒体内存区）     ← VB 池、码流缓冲、ISP 中间缓冲都在这
```

- 图像帧**不走 malloc**，全部从 MMZ 分配
- `cat /proc/media-mem` 看 MMZ 总用量和剩余
- MMZ 和 Linux 内存池**完全隔离**：tmpfs 用的是那 121MB，不会和 VB 池抢

记住这三个概念（三级模型 / 绑定 / MMZ），sample 里 90% 的 API 都是在它们上面打转。

---

## 2. sensor 驱动是用户态库，不是内核模块

这是从别的平台（RV1126/i.MX）转过来最容易误判的一点：

| 常见误解 | 实际情况 |
|---|---|
| sensor 驱动是 `.ko` | `ko/` 目录里**没有任何 sensor 的 ko** |
| 需要写 sensor 驱动 | sensor 驱动是用户态 `.a/.so`：`lib/libsns_gc2053.a`、板端 `/usr/lib/libsns_gc2053.so` |
| 内核里配 sensor | `load3516dv300` 只把**名字字符串**传给 `sys_config.ko`，由它决定 ISP 去 dlopen 哪个 libsns |

验证方法：

```bash
# 1. sys_config.ko 里是否认这个名字
grep -a -o "gc2053" mpp/ko/sys_config.ko

# 2. 库里是否有实现
ls -l mpp/lib/libsns_gc2053.a
```

`load3516dv300` 的 `-h` 帮助文本是**过期的**（只列 imx327/imx335/os05a/imx307），实测传 `gc2053` 是可以的：

```bash
./load3516dv300 -a -sensor0 gc2053 -osmem 128 -total 1024
# dmesg 应出现：sensor0: gc2053
```

> 附带现象：sensor1 默认是 NULL，会打 `parse_sensor_bus_type SNS:[NULL] is not supported`，**无害**，不用管。

---

## 3. 配置 sensor 类型

`mpp/sample/Makefile.param` 第 42 行：

```makefile
# 原值
SENSOR0_TYPE ?= SONY_IMX327_MIPI_2M_30FPS_12BIT
# 改成
SENSOR0_TYPE ?= GALAXYCORE_GC2053_MIPI_2M_30FPS_10BIT
```

`sensor1_TYPE` 保持默认不动。

改完**必须** `make clean`，因为头文件里的宏会参与编译。

---

## 4. 三个编译坑（每个都真实踩过）

### 坑 1：不能用 `make -C`，必须 cd 进目录

```
$ make -C venc
venc/Makefile:3: /path/mpp/smp_linux.mak: No such file or directory
```

原因：`venc/Makefile` 里是

```makefile
include $(PWD)/../$(ARM_ARCH)_$(OSTYPE).mak
```

`$(PWD)` 是 **make 的环境变量**，不会因为 `-C venc` 而改变，所以它去 `mpp/smp_linux.mak` 找，而真实文件在 `mpp/sample/smp_linux.mak`。

正确做法：

```bash
cd mpp/sample/venc && make
```

### 坑 2：`make clean` 不清 common 的 .o

海思 sample 的 clean 只清当前目录。改了 `sample/common/*.c`（比如 VI 的 MIPI 配置）却只 `make clean && make`，链接的仍是**旧的 common .o**，二进制根本不会变。

```bash
cd mpp/sample/venc
rm -f ../common/*.o     # 关键
make clean && make
```

### 坑 3：改完一定要用 md5 验证二进制真的变了

```bash
md5sum sample_venc
# 改之前：03cecd53...
# 改之后：e9ab6935...   ← 变了才说明编进去了
```

本次事故：连续两次编译 md5 都是 `03cecd53`，等于白改。**"编过了" ≠ "改生效了"**。

### 附：sed 改源码前必须核对真实行号

```bash
# 先看
grep -n "combo_dev_attr_t MIPI_2lane_CHN0_SENSOR_GC2053" sample_comm_vi.c
sed -n '544,575p' sample_comm_vi.c
# 再改，行号差一行就会静默改错（本次 555 应为 556）
sed -i '556s/{0, 1, -1, -1}/{0, 2, -1, -1}/' sample_comm_vi.c
```

---

## 5. 产物与基线

| 项 | 值 |
|---|---|
| 二进制 | `sample_venc`，**3,432,068 字节** |
| md5 | `e9ab6935…` |
| 编译时宏 | `-DSENSOR0_TYPE=GALAXYCORE_GC2053_MIPI_2M_30FPS_10BIT` |
| 部署 | 通过 tftp 传到板端 `/root/sample_venc` |

传输（板子主动拉）：

```bash
# PC/VM 起 tftp 服务，板端执行
tftp -g -l sample_venc -r sample_venc 192.168.1.103
chmod +x sample_venc
```

---

## 6. 编译检查清单

- [ ] SDK 路径是 `smp/a7_linux/mpp`，不是顶层 `mpp`
- [ ] `Makefile.param` 的 `SENSOR0_TYPE` 已改
- [ ] 在 `sample/venc` 目录里 make（不是 `make -C`）
- [ ] 改过 `common/*.c` 就 `rm -f ../common/*.o`
- [ ] 用 `md5sum` 确认二进制变化
- [ ] `strings sample_venc | grep GC2053` 能搜到 sensor 名
