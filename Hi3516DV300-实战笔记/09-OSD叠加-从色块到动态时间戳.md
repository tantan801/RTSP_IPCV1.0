# 09｜OSD 叠加：从色块到动态时间戳（三阶段）

> 时间：2026-11-05
> 目标：在 1080p 码流左上角打上"走动的"时间水印
> 方法：拆成 A（色块）/ B（文字）/ C（动态刷新）三段，每段只引入一个新变量，独立验收
> 结果：三阶段全部通过，画面左上角 `HI3516DV300 OSD` + `2026-11-05 16:15:32` 实时走动

OSD 这个词很容易让人以为是个"画图功能"，其实它是**视频管线上的一个合成环节**。这篇按"管线位置 → 硬件资源 → 像素语义 → 三个阶段 → 踩坑"的顺序讲，代码量不大（~400 行），知识点密度不比 RTSP 那篇低。

---

## 0. 概念铺垫：先把词讲清楚

### 0.1 OSD 是什么，在管线的哪一环

OSD = On-Screen Display，画面上叠加的文字/图形。关键在于**叠加发生在编码之前**：

```
sensor → MIPI → VI → VPSS ──┬─→ VENC chn0 (1080p 主码流) ──→ RTP ──→ 播放器
                            │        ↑
                            │     【OSD 合成点】
                            └─→ VENC chn1 (子码流)

（OSD 也可以贴在 VPSS 某个通道上，见 0.5）
```

由此推出一个产品级结论：**OSD 是"烙"进码流里的**。VLC、手机 App、NVR、录像文件，任何客户端拿到的画面都自带水印，客户端不需要做任何事，也没法关掉。这是 OSD 之于"前端用 Canvas 叠一层"的本质区别——后者一录屏就没了。

### 0.2 RGN / REGION：MPP 里的"贴图层"对象

海思 MPP 把叠加层抽象成 **RGN（Region）**，五个 API 构成完整生命周期：

| API | 作用 | 类比 |
|---|---|---|
| `HI_MPI_RGN_Create` | 创建一块区域（定尺寸、像素格式、画布数） | `malloc` + 定格式 |
| `HI_MPI_RGN_SetBitMap` | 往区域里塞一张位图 | `memcpy` |
| `HI_MPI_RGN_AttachToChn` | 把区域"贴"到某个通道 | `bind` |
| `HI_MPI_RGN_DetachFromChn` | 摘下来 | `unbind` |
| `HI_MPI_RGN_Destroy` | 销毁 | `free` |

**注意顺序**：Create 只创建、不显示；Attach 之后才真正出现在画面上；Detach 之前 Destroy 会失败。这套"创建 → 挂接 → 摘除 → 销毁"是 MPP 所有模块的统一叙事（VPSS bind、VO 通道都是这个味道）。

### 0.3 OVERLAY_RGN 和 OVERLAYEX_RGN 选哪个

| 类型 | 走什么资源 | 数量 | 性能 | 适用 |
|---|---|---|---|---|
| `OVERLAY_RGN` | 芯片内部 overlay 硬件 | 少（个位数） | 最快，不占带宽 | 时间戳、少量图标 |
| `OVERLAYEX_RGN` | VGS 视频图形子系统 | 多 | 稍慢，占内存带宽 | 大量/大尺寸叠加、多路 |

本项目只需要一块 200×40 的时间戳，选 `OVERLAY_RGN`。**判断依据不是"哪个高级"，而是"你要几块、多大"**。

### 0.4 ARGB1555：1 位 alpha 怎么做半透明

像素格式 `PIXEL_FORMAT_ARGB_1555`，每像素 16 位：

```
 bit15  bit14-10  bit9-5  bit4-0
   A       R        G       B
  1位     5位      5位     5位
```

只有 1 位 alpha，理论上只能"全透明/全不透明"。**真正的半透明来自通道属性里的 FgAlpha/BgAlpha**：

| 位图像素 | A 位 | 走哪个 alpha | 效果 |
|---|---|---|---|
| `0xFFFF` | 1 | `u32FgAlpha`（前景 alpha） | 白色按 FgAlpha 混合，128 ≈ 半透明白 |
| `0x0000` | 0 | `u32BgAlpha` + `u32BgColor` | 该像素处混合背景色（黑），64 ≈ 半透明黑 |

这就是 OSD 里做"衬板"的原理：**文字像素写 0xFFFF，其余写 0x0000，衬板色设成黑色**，于是一块半透明黑底就出来了——不需要真的画一个矩形，也不需要逐像素算 alpha。

> alpha 取值范围在头文件注释里写了：**`u32FgAlpha` / `u32BgAlpha` 都是 [0, 128]**，不是常见的 [0,255]。填 255 会被驱动截断或报参数错误。

### 0.5 贴 VENC 还是贴 VPSS

| 挂接点 | 影响范围 | 适合 |
|---|---|---|
| `HI_ID_VENC` chn0 | 只有这一路码流 | 只用主码流打水印（本项目） |
| `HI_ID_VPSS` grp/chn | 该通道往下游的所有编码通道（主+子共享） | 想所有码流都带水印 |

**关键区别**：贴 VPSS 是"在源头画一次"，下游所有编码通道都吃到；贴 VENC 是"在出口画"，只有那一路有。

结论：要"双码流都带时间戳"就贴 VPSS；只要主码流就贴 VENC（改动面更小、不影响子码流带宽）。本项目贴 VENC chn0。

### 0.6 u32CanvasNum：画布数决定会不会撕裂

`u32CanvasNum` 是 Create 时指定的"画布数量"：

- **=1**：只有一张画布。编码器正在读它合成，你的线程同时往里写 → 读写竞争 → 可能出现**半张旧文字半张新文字**的撕裂
- **=2**：驱动维护两张画布，编码器读 A 时你可以写 B，写完提交再交换 → 读写永远不撞

这和 GUI 的 double buffering、摄像头的 ping-pong buffer 是同一个思想，只是规模小得多。

### 0.7 点阵字体：为什么不用 freetype

| 方案 | 依赖 | 体积 | 适用 |
|---|---|---|---|
| FreeType + 矢量字体 | 要带库 + 字体文件（几 MB） | 大 | 多字号、中文、任意字体 |
| 内嵌点阵字库 | 无依赖 | 95 字符 × 16 字节 = **1520 字节** | 固定字号的 ASCII（OSD 典型需求） |

8×16 点阵的存储约定：**每个字符 16 个字节，一字节一行，字节的 MSB 对应最左边的像素**。

```
'A' 的 16 行：
0x00  ← 空行
0x00
0x10  ← 0001 0000 → 第 4 个像素亮
0x38  ← 0011 1000
0x6C
0x6C
0xC6
0xC6  ← 1100 0110
0xFE  ← 1111 1110（横杠）
0xC6
0xC6
0xC6
0xC6
0x00
0x00
0x00
```

MSB-first 还是 LSB-first 是**必须自己定死并全链一致**的约定，写反了字就左右镜像。

### 0.8 板子上的时间：RTC、NTP、TZ

时间戳离不开"时间从哪来"，这里有三件事：

1. **RTC（硬件时钟）**：本板没有电池/没有 RTC，每次上电系统时间都是 `1970-01-01 00:00`
2. **NTP**：有网络就能对时（`ntpd` / `ntpdate` / busybox 的 `ntpd -q`），但要板子能出网
3. **TZ（时区）**：`localtime_r()` 靠 `TZ` 环境变量决定偏移。**busybox 系统一般不带 `/usr/share/zoneinfo`**，`TZ=Asia/Shanghai` 会失效，要用 POSIX 格式：

```sh
export TZ=CST-8      # 表示 UTC+8，不依赖任何时区文件
```

`CST-8` 的含义是"名为 CST 的时区，比 UTC 早 8 小时"（POSIX 里符号是反的，别被绕晕）。

---

## 1. 阶段 A：色块（验证链路和 alpha）

### 1.1 改动点

新建 `sample/venc/osd_test.c`，在 `sample_venc.c` 的 `SAMPLE_VENC_H265_H264()` 里、`SAMPLE_COMM_VENC_StartGetStream()` 成功**之后**调用：

```c
if (HI_SUCCESS != osd_test_start(VencChn[0]))
{
    SAMPLE_PRT("OSD test start failed, continue without OSD\n");
}
```

**为什么必须在 VENC 通道起来之后**：`AttachToChn` 的目标是"通道 ID"，通道还没创建时 Attach 直接返回失败。这个顺序错了会得到一个很干净的错误码，不会崩，所以容易被忽略。

### 1.2 关键字段

```c
/* Create：定尺寸、格式、画布数 */
RGN_ATTR_S stRgnAttr;
stRgnAttr.enType                            = OVERLAY_RGN;
stRgnAttr.unAttr.stOverlay.enPixelFmt       = PIXEL_FORMAT_ARGB_1555;
stRgnAttr.unAttr.stOverlay.stSize.u32Width  = 200;   /* 建议 2 对齐 */
stRgnAttr.unAttr.stOverlay.stSize.u32Height = 40;
stRgnAttr.unAttr.stOverlay.u32BgColor       = 0x000000;  /* 衬板色：黑 */
stRgnAttr.unAttr.stOverlay.u32CanvasNum     = 2;

/* Attach：定位置、透明度、图层 */
RGN_CHN_ATTR_S stChnAttr;
stChnAttr.bShow                              = HI_TRUE;
stChnAttr.enType                             = OVERLAY_RGN;
stChnAttr.unChnAttr.stOverlayChn.stPoint.s32X = 64;   /* 坐标也要 2 对齐 */
stChnAttr.unChnAttr.stOverlayChn.stPoint.s32Y = 64;
stChnAttr.unChnAttr.stOverlayChn.u32FgAlpha   = 128;  /* [0,128] */
stChnAttr.unChnAttr.stOverlayChn.u32BgAlpha   = 64;
stChnAttr.unChnAttr.stOverlayChn.u32Layer     = 0;    /* [0,7] */

stChn.enModId  = HI_ID_VENC;   /* 注意是 HI_ID_VENC，不是 MOD_ID_VENC */
stChn.s32DevId = 0;
stChn.s32ChnId = s32VencChn;
```

**枚举名坑**：`MPP_CHN_S.enModId` 的类型是 `MOD_ID_E`，但枚举值叫 `HI_ID_VENC`（不是 `MOD_ID_VENC`）。头文件里 grep 一下比记忆靠谱。

### 1.3 验收：故意做一半透明

色块测试时把画布**左半填 `0xFFFF`、右半填 `0x0000`**：

| 观察结果 | 说明 |
|---|---|
| 左半不透明白 + 右半透明（能看到画面） | alpha 语义正确，`SetBitMap` 生效 |
| 整块全白 | 位图没提交（漏了 `SetBitMap`）或全填了 0xFFFF |
| 整块看不见 | 全填 0x0000（全透明），或 Attach 失败 |
| 位置偏 / 尺寸不对 | 坐标或尺寸没 2 对齐 |

实测画面：左上角 (64,64) 只有一个 **200×50 里左半边约 100×50 的白块**，右半边完全透明——一次就把"链路通不通 + alpha 对不对"两件事同时验证了。

> 这种"一个实验验证两个假设"的设计，比"先全填白看看有没有块，再改 alpha 看看透不透"省一轮板测。

---

## 2. 阶段 B：点阵文字

### 2.1 不手敲点阵，用工具生成

手敲 95 个字符 × 16 行 = 1520 个十六进制数，错一位字形就歪，而且肉眼查不出来。做法：**在 PC/VM 上用 Pillow 渲染真实字体，二值化后导出 C 数组**。

```python
from PIL import Image, ImageDraw, ImageFont

W, H = 8, 16
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 13)

rows = []
for code in range(32, 127):                 # ASCII 可打印字符
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), chr(code), font=font)
    x = (W - (bbox[2]-bbox[0]))//2 - bbox[0]     # 水平居中
    y = (H - (bbox[3]-bbox[1]))//2 - bbox[1]     # 垂直居中
    d.text((x, y), chr(code), fill=255, font=font)
    px = img.load()
    for yy in range(H):
        b = 0
        for xx in range(W):
            if px[xx, yy] > 110:                 # 阈值 110，二值化
                b |= (1 << (7 - xx))             # MSB = 最左
        rows.append(b)
```

三个关键参数的选择理由：

| 参数 | 取值 | 为什么 |
|---|---|---|
| 字号 | 13px（画在 8×16 格子里） | 太大截顶、太小糊成一团；13px 正好填满 16 行 |
| 字体 | DejaVuSansMono**-Bold** | 等宽保证对齐；Bold 在小尺寸下笔划不断 |
| 阈值 | 110 / 255 | 低于此算背景。太低笔划变粗粘连，太高笔划断裂 |

**生成后一定要出预览图**（把数组再反向画回一张 PNG 放大看），因为"数组生成成功"和"字形正确"是两件事。

### 2.2 渲染与衬板

```c
static HI_VOID osd_draw_char(HI_U16 *pBmp, HI_U32 x, HI_U32 y, char ch, HI_U16 color)
{
    HI_U32 row, bit;
    if (ch < 32 || ch > 126) ch = '?';          /* 越界兜底，别读到数组外 */
    for (row = 0; row < 16; row++) {
        HI_U8  b     = s_font8x16[ch - 32][row];
        HI_U16 *pLine = pBmp + (y + row) * OSD_W;   /* 行偏移按 OSD_W 走 */
        for (bit = 0; bit < 8; bit++) {
            if (b & (0x80 >> bit)) {
                pLine[x + bit] = color;             /* 只写"亮"的像素 */
            }
        }
    }
}
```

**只写亮的像素、不写暗的像素**——暗像素保持 `memset` 后的 `0x0000`，自动走 BgAlpha 变成半透明衬板。这是"衬板零成本"的关键：不需要画矩形，只需要不清屏。

两行文字：

```
第 1 行 (4,4)：HI3516DV300 OSD      ← 固定标题
第 2 行 (4,20)：2026-11-05 15:53    ← 阶段 C 改成动态
```

19 个字符 × 8px = 152px，200 宽刚好装下。**区域宽度要按最长字符串算**，超了会被 `osd_draw_text` 里的边界检查截断（宁可截断也不能越界写内存）。

---

## 3. 阶段 C：动态时间戳

### 3.1 三件事加进去

```c
/* 1) 刷新线程：每秒重绘 + 提交 */
static HI_VOID *osd_refresh_proc(HI_VOID *pArg)
{
    char   szTime[32];
    while (s_bThreadRun) {
        osd_format_time(szTime, sizeof(szTime));   /* localtime_r 取系统时间 */
        osd_paint(szTime);                          /* 重绘整块画布 */
        osd_flush();                                /* SetBitMap 提交 */
        osd_wait_next_second();                     /* 睡到下一个整秒边界 */
    }
    return HI_NULL;
}

/* 2) 对齐整秒的睡眠 */
static HI_VOID osd_wait_next_second(HI_VOID)
{
    struct timeval tv;
    gettimeofday(&tv, HI_NULL);
    usleep(1000000 - tv.tv_usec);       /* 差多少补多少，不固定睡 1s */
}

/* 3) 收尾顺序：先停线程，再拆资源 */
if (HI_TRUE == s_bThreadStarted) {
    s_bThreadRun = HI_FALSE;
    pthread_join(s_thread, HI_NULL);    /* 等线程真的退出 */
    s_bThreadStarted = HI_FALSE;
}
/* ...之后才 Detach / Destroy / free */
```

### 3.2 为什么是"睡到整秒"而不是 `sleep(1)`

`sleep(1)` 的实际睡眠是 `1s + 调度误差`，误差按毫秒累积。跑十几分钟后，刷新时刻会逐渐偏离整秒边界，出现**某秒没刷（跳秒）或者同一秒刷两次**。肉眼表现就是秒数"卡一下然后跳两个"。

`usleep(1000000 - tv_usec)` 每次都重新对齐到下一个整秒，误差不累积。这是所有"秒表类"刷新的通用写法（时钟 App、状态栏时间都一样）。

### 3.3 线程栈为什么要显式设 64KB

`pthread_create` 默认栈通常是 8MB（虚拟地址空间）。这块板子有前科：**12MB 的静态 BSS 会直接导致 exec 失败**（第 08 篇的 segfault 就是内存紧张导致映射失败）。刷新线程只调用 `snprintf` 和 `localtime_r`，64KB 绰绰有余：

```c
pthread_attr_t stAttr;
pthread_attr_init(&stAttr);
pthread_attr_setstacksize(&stAttr, 64 * 1024);
pthread_create(&s_thread, &stAttr, osd_refresh_proc, HI_NULL);
pthread_attr_destroy(&stAttr);
```

> 经验：嵌入式设备上**任何线程都应该显式设栈大小**。默认 8MB × N 个线程，在 128MB 内存的板子上是很容易踩空的账。

### 3.4 板子时间：从 1970 到 2026

第一次跑起来画面显示 `1970-01-01 08:00:xx`——不是代码 bug，是板子没有 RTC。部署流程要加对时：

```sh
# 板子（telnet）：设系统时间 = 北京时间 - 8 小时（因为系统时区当 UTC 用）
date -u -s "2026-11-05 08:14:00"

# 验证：用 POSIX 时区看东八区时间
TZ=CST-8 date
# Thu Nov  5 16:14:01 CST 2026   ← 对上了

# 启动程序时带上 TZ，否则程序里 localtime 出来的是 UTC
TZ=CST-8 ./sample_venc_osd 0 < in > osd.log 2>&1 &
```

这个问题的产品级答案只有两条路：

1. **加 RTC 芯片 + 纽扣电池**（成本几毛钱，但断电后仍会漂）
2. **开机 NTP 对时**（要能出网；内网要有 NTP 服务器）

`date -s` 手工对时只能在调试阶段用——**重启就没了**。

---

## 4. 六个坑：三类，各有指纹

### 4.1 环境类

**坑① tmpfs 又被写满（老坑重演）**

sample 自带"落流到文件"功能，`stream_chn0.h264` / `stream_chn1.h264` 各写 23MB，48MB 的 tmpfs 几分钟就 100%。表现：`df -h /mnt` 显示 `100%`，新文件写不进。

真相：**RTSP 推流不经过这个文件**（走 VENC 取流 → 内存队列 → socket），所以盘满不影响拉流，但录流/日志全废。

处理：跑之前清、跑之后清。**长期方案是关掉落流**（把 `SAMPLE_COMM_VENC_GetVencStreamProc` 里的 `fwrite` 分支关掉）。

**坑② tftp 下载后是 `-rw-------`，忘了 `chmod +x`**

```
./sample_venc_osd 0
sh: ./sample_venc_osd: Permission denied
```

tftp 客户端不带权限位。下载完必须 `chmod +x`。这个坑本次踩了两遍。

**坑③ 板子无 RTC，时间是 1970**

见 3.4。判据很明确：画面时间戳显示 1970 就是没对时，不是格式化代码写错。

### 4.2 Shell 类

**坑④ `cd /mnt && ./app &` 的 `&` 把整条链后台化了**

```sh
cd /mnt && ./sample_venc_osd 0 < in > osd.log 2>&1 &
```

`&` 作用在**整个 `cd ... && ...` 列表**上，所以 `cd` 是在子 shell 里执行的，主 shell 的工作目录没变 → 程序找不到 → `127 not found`。

正确写法：`cd` 单独一条。

```sh
cd /mnt
./sample_venc_osd 0 < in > osd.log 2>&1 &
```

**坑⑤ 交互式菜单：后台跑会 SIGTTIN 停住**

`sample_venc 0` 会打印码控模式菜单并等键盘输入。直接 `&` 后台化，进程一读 tty 就收到 **SIGTTIN 被暂停**（`ps` 里状态 `T`），VENC 通道根本没建起来。

两种解法：

```sh
# 方案 A：fifo 喂输入（推荐，可脚本化）
mkfifo in
(echo c; sleep 3600) > in &          # c = CBR 码控；sleep 保持写端不关
./sample_venc_osd 0 < in > osd.log 2>&1 &

# 方案 B：前台跑，会话不能断
./sample_venc_osd 0        # telnet 会话一关 = SIGHUP，进程死
```

fifo 方案的两个细节：`echo c` 只写一次不够时（菜单有多处输入点），可以多写几次；`sleep 3600` 是为了让 fifo **始终有写端**，否则程序读到 EOF 会反复重试。

### 4.3 可观测性类

**坑⑥ printf 重定向到文件是全缓冲，日志"看不到"**

```sh
grep OSD osd.log        # 什么都不输出，但画面上 OSD 明明正常
```

原因：stdout 重定向到文件时是**全缓冲（4KB）**，`printf` 的内容还在用户态缓冲区里没落盘。程序不退出、输出量又小，就永远看不到。

三个解法：

1. 在关键 printf 后加 `fflush(stdout)`
2. 用 `setvbuf(stdout, NULL, _IONBF, 0)` 关掉缓冲
3. **最实用：别靠日志判断 OSD，直接看画面**（拉一帧比 grep 一万行靠谱）

---

## 5. 六坑分类学：4 个不是 OSD 的错

| 坑 | 类别 | 指纹 | 以后怎么防 |
|---|---|---|---|
| tmpfs 写满 | 存储/环境 | `df` 100% | 跑之前先 `df -h` |
| tftp 无执行权限 | 运维 | `Permission denied` | 下载脚本里固定跟 `chmod +x` |
| 无 RTC → 1970 | 硬件/环境 | 时间戳年份 1970 | 部署流程加对时 + NTP |
| `cd && xxx &` | Shell 语义 | `127 not found` | `cd` 单独一条命令 |
| SIGTTIN 暂停 | 进程/终端 | `ps` 状态 `T`、VENC 通道空 | fifo 喂输入 |
| printf 全缓冲 | 可观测性 | 日志空但功能正常 | 看画面 / 关缓冲 |

**只有"Attach 顺序、alpha 语义、MSB 位序"这三个才是真正属于 OSD 的知识点**，其余都是环境问题。这个比例在嵌入式里非常典型——**排障时间 80% 花在环境和运维上，不是业务代码**。

判据：出问题时先问"是环境问题还是代码问题"，用 `/proc/umap/region`、`df -h`、`ps`、`date` 四把尺子先量一遍，能省掉一半弯路。

---

## 6. 验收数据

### 6.1 三阶段判据

| 阶段 | 判据 | 实测 |
|---|---|---|
| A 色块 | (64,64) 处出现 200×宽 色块，左半不透明白、右半透明 | ✅ 符合 |
| B 文字 | 两行白字清晰可读，衬板半透明（能隐约看到背景） | ✅ `HI3516DV300 OSD` / `2026-11-05 15:53` |
| C 动态 | 间隔 N 秒抓两帧，秒数差 = 实际经过时间 | ✅ 24 → 32（8 秒） |

### 6.2 阶段 C 的量化验证

除了肉眼看图，做了一次**像素级差分**（比"我觉得在动"更硬）：

```python
from PIL import Image, ImageChops
a = Image.open('f_a.jpg').convert('L')
b = Image.open('f_b.jpg').convert('L')
ra = a.crop((64, 64, 264, 104))       # 只比 OSD 区域
rb = b.crop((64, 64, 264, 104))
px = list(ImageChops.difference(ra, rb).getdata())
print(sum(1 for p in px if p > 40))   # → 49
```

200×40 = 8000 像素里只有 **49 个像素变化**，正好是"秒位两个数字笔划"的量级。如果整块 8000 都在变，说明画布在被整体重写/撕裂；如果 0 变化，说明刷新没生效。**这个数字本身就是一个判据**。

### 6.3 回归

| 项 | 结果 |
|---|---|
| 帧率 | 抓流 91 帧 / 3 秒 ≈ 30fps，未退化 |
| RTSP 拉流 | UDP 82 帧 / 2.96 秒，无 missed、无 error |
| 码流格式 | H.264 Baseline 1920×1080 30fps，3.7Mbps |
| 解码错误 | 0（ffmpeg 无报错） |

**OSD 走硬件合成，理论上不占 CPU、不掉帧**。如果加了 OSD 掉帧，说明你在 CPU 上做了逐像素 alpha 混合（那就是软件 OSD 了），应该检查是不是根本没用 RGN。

---

## 7. 产物与回滚

### 7.1 文件清单

| 位置 | 文件 | 说明 |
|---|---|---|
| VM `sample/venc/` | `osd_test.c` | 阶段 A→C 的最终版（~400 行，含 1520 字节点阵数组） |
| VM `sample/venc/` | `sample_venc.c` | 打补丁：extern 声明 + `StartGetStream` 后调用 |
| VM `sample/venc/` | `sample_venc.c.bak_osd` | 打补丁前原始备份 |
| 本地 `work/` | `osd_test.c` / `osd_test_stageB.c` | 阶段 C / 阶段 B 源码留档 |
| 本地 `work/` | `genfont.py` / `font_array.txt` | 点阵生成脚本与数据 |
| VM `/srv/tftp/` | `sample_venc_osd` | md5 `9211c11e17839ca03b6293db3d06c6fe` |
| 板 `/mnt/` | `sample_venc_osd` | 同上 md5 |

### 7.2 回滚

```sh
# 回到"没有 OSD"的状态
cd ~/hisi/Hi3516CV500_SDK_V2.0.2.0/smp/a7_linux/mpp/sample/venc
cp sample_venc.c.bak_osd sample_venc.c
rm -f osd_test.c osd_test.o sample_venc
rm -f ../common/*.o && make
```

注意：**只要 `sample_venc.c` 里还留着 `osd_test_start()` 调用，就必须留着 `osd_test.c`**，否则链接会 undefined reference。

### 7.3 关闭落流（建议做，防止写满 tmpfs）

`sample/common/sample_comm_venc.c` 的 `SAMPLE_COMM_VENC_GetVencStreamProc` 里有 `fwrite` 分支，用宏关掉：

```c
#define VENC_SAVE_STREAM  0      /* 0 = 不落流，只推 RTSP */
```

或者在启动拍板的脚本里把 `/mnt` 换成更小的 tmpfs（不够就写失败，比写满 jffs2 安全）。

---

## 8. 判据速查表（本次新增）

| 现象 | 第一嫌疑 | 取证命令 |
|---|---|---|
| Attach 返回非 0 | 通道没起来 / 枚举用错 | 确认 `StartGetStream` 之后调用；grep `HI_ID_VENC` |
| 返回全 0 但画面无色块 | 没 `SetBitMap`，或全 0（透明） | 看代码有没有 `osd_flush()`；左半填 0xFFFF 试 |
| 色块/文字位置偏 | 坐标或尺寸没 2 对齐 | 改 64/200 这种偶数值 |
| 文字左右镜像 | MSB/LSB 位序反了 | 检查 `b & (0x80 >> bit)` |
| 文字糊/断裂 | 字号或阈值不合适 | 改字号 12/13/14 重出预览图 |
| 时间戳显示 1970 | 板子没 RTC、没对时 | `TZ=CST-8 date` |
| 时间戳显示 UTC（差 8 小时） | 启动没带 TZ | `TZ=CST-8 ./app` |
| 秒数跳秒/卡顿 | 用 `sleep(1)` 累积漂移 | 改 `usleep` 对齐整秒 |
| 画面撕裂（半旧半新） | 单画布边读边写 | `u32CanvasNum = 2` |
| 日志空但功能正常 | stdout 全缓冲 | `fflush` 或直接看画面 |
| 进程状态 `T`、VENC 没通道 | SIGTTIN（等交互输入） | fifo 喂输入 |
| `Permission denied` | tftp 下载没执行权限 | `chmod +x` |

---

## 9. 遗留与下一步

### 9.1 当前状态

OSD 三阶段全部完成，"时间水印"这个产品功能已经有完整雏形。但**离"设备"还差三件事**：

1. **开机自启**：现在全靠 telnet 手敲 `date -s` + `mkfifo` + 启动，重启就回到 1970
2. **落流未关**：tmpfs 48MB 几分钟写满
3. **NTP 未接**：时间靠手工对，漂移没人管

### 9.2 下一步候选

| 方向 | 工作量 | 价值 |
|---|---|---|
| **开机自启固化**（S00devs 里加对时 + tmpfs + 启 sample） | 小 | 从"实验"变"设备" |
| **NNIE 人形/人脸检测** | 大 | 监控项目的核心价值 |
| **MP4 录像**（存 SD/NFS，别写 jffs2） | 中 | 需 mp4v2 + 循环覆盖策略 |
| **i.MX6ULL 内核章节** | 中 | 另一条学习线 |

按"小步可验收"原则，建议先做**开机自启固化**——它把前面 9 篇的所有成果一次性变成"重新上电就能跑"的东西。

---

## 附：本篇最该记住的三句话

1. **OSD 在编码前合成**，所以它是码流的一部分，客户端关不掉——这是它和"前端叠层"的本质区别。
2. **1 位 alpha 也能做半透明**，靠的是 FgAlpha/BgAlpha 两个通道参数配合位图的 A 位，`0xFFFF` 走前景、`0x0000` 走背景色。
3. **静态 OSD 和动态 OSD 是两个问题**：前者考"链路通不通"，后者考"并发和时间"。分阶段验证，别一次写完再 debug。
