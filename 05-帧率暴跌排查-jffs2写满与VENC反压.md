# 05｜帧率暴跌排查实战：从"写满的 jffs2"到"VENC 反压整条链路"

> 现象：30 秒录制只得到 20 帧（≈0.7fps），而画面正常、无解码错误
> 根因：码流文件写到只剩 4.6MB 的 jffs2 根分区 → fwrite 被 GC 拖到秒级阻塞 → 码流队列满 → 编码器堵死 → 全链丢帧
> 价值：这是"下游阻塞反压上游"的典型样本，方法论可复用到任何采集/编码链路

---

## 1. 现象与第一轮排除

| 观察 | 数据 |
|---|---|
| 录制 30 秒得到的文件 | 2,355,249 字节 |
| `ffmpeg -f null -` 解码 | 20 帧，**零解码错误** |
| 包结构 | 1 个 I 帧（138KB）+ 19 个 P 帧（≈12KB） |
| 抽帧看画面 | 曝光、白平衡、噪点都正常 |

结论：**码流没坏，编码器没坏，画面没坏——只是"帧数特别少"**。

这里先把两个容易跑偏的假设排掉：

1. **不是低照度长曝光**：如果 AE 把曝光拉到 1 秒以上，画面会明显过曝/拖影；实测亮度正常。
2. **不是 sensor 出帧慢**：直接在录制中看 `/proc/umap/vi`，VI PIPE 的 `IntCntPerSec = 30`、`FrameRate = 30`。

---

## 2. 先看源码：这个 sample 到底"录多久"

`sample_venc 0` → `SAMPLE_VENC_H265_H264()` → 结尾是：

```c
printf("please press twice ENTER to exit this sample\n");
getchar();
getchar();
```

**没有固定时长**，全靠人工按两次回车。所以"30 秒"是人为等待时间，而且 H.265 ES 流没有时间戳（见第 4 篇），**任何从文件侧推算时长的做法都不可靠**，必须上板看运行中的计数器。

---

## 2.5 先补一个前置概念：VB 池为什么会被抽干

后面会反复看到 `VbFail`，得先知道它到底是什么在失败。

### VB 池是什么

**VB（Video Buffer）池 = MMZ 里预先切好的一堆等大图像缓存块**。整条链路里没有"帧对象"到处 new，而是所有环节都往池子里借块、用完还回去：

```
        ┌──────────── VB 池（N 块 × 3.1MB）────────────┐
        │  空闲队列                                     │
        └───┬──────────────────────────────────▲───────┘
   Get()    │                                  │  Release()
            ▼                                  │
   VI 写入 ──▶ VPSS 读 ──▶ VENC 读 ──▶ 编码完成 ─┘
```

配置在 `SAMPLE_COMM_SYS_Init()` 里（`sample_comm_sys.c`）：

```c
stVbConf.astCommPool[0].u32BlkSize = 1920 * 1080 * 3 / 2;  // 3.11MB/块
stVbConf.astCommPool[0].u32BlkCnt  = 8;                    // 8 块
```

算一下就知道余量：8 块 × 3.1MB ≈ 25MB，只够链路里同时在飞的帧用。**块数太少 → 高帧率下立刻不够**。

### 关键：每一帧 Get 之后必须 Release

| 模式 | 谁来 Release | 会出错吗 |
|---|---|---|
| **bind 模式**（硬件直连） | 系统自动 | 不会 |
| **user mode**（自己 `GetChnFrame`） | **你的代码** | 忘了还就会出事 |

一旦有环节"借了不还"：

```
空闲块越来越少 → VI 申请不到新块 → VbFail++ → VI 直接丢帧
```

**所以 `VbFail` 上涨 ≠ "内存不够"，更常见的含义是"有人拿了不还"**。

### 怎么查

```bash
cat /proc/umap/vb
```

关注每行的 `Free` 列：**持续下降不回升 = 泄漏**（有代码没 Release）；一直贴着 0 = 池子确实太小。

对照第 3 节的实测数据：`VbFail` 从 19 涨到 103，是因为码流队列堵死导致 VENC 不再消费新帧，VPSS 也就没法归还——**反向的"不还"**。这种情况是背压，不是代码漏 Release。两者的区别就在于：**Free 是一路降到 0 并卡住，还是缓慢泄漏不回来**。

---

## 3. 上板取证：/proc/umap 三级计数器

录制进行中（程序还在跑）抓这四个：

```bash
cat /proc/umap/vi        # 采集侧
cat /proc/umap/vpss      # 处理侧
cat /proc/umap/venc      # 编码+取流侧
cat /proc/umap/mipi_rx   # 物理层
```

### 3.1 各字段含义（本次用到的）

**VI**

| 表 | 字段 | 含义 |
|---|---|---|
| VI PIPE STATUS | `IntCnt` | 累计中断数（≈累计帧数） |
| | `FrameRate` | 当前帧率 |
| | `LostFrame` / `VbFail` | 丢帧 / **VB 池申请失败**（下游没还缓存） |
| VI CHN STATUS | `FrameRate` / `LostFrame` / `VbFail` | 同上，通道级 |
| VI PROC OFFLINE IRQ | `IntCntPerSec` | 硬件中断频率（真实出帧率） |

**VPSS**

| 字段 | 含义 |
|---|---|
| `RecvPic0` | 收到多少帧 |
| `SendOk` / `FrameRate` | 各通道送出多少帧 |

**VENC（最关键）**

| 表 | 字段 | 含义 |
|---|---|---|
| VENC SEND1 | `VpssSnd` | VPSS 送进来多少帧 |
| | `Send` | 真正被编码器接收多少 |
| | **`Full`** | **输入队列满而丢弃的帧数** ← 反压的直接证据 |
| VENC CHNL INFO | `VencInt` | 编码完成帧数（编码产出） |
| VENC STREAM STATE | `BusyCnt` | 已编码但未被用户取走的帧（码流队列占用） |
| | `UserGet` | 用户（GetStream 线程）取走帧数 |
| | `Interval` | 两次取流的时间间隔（微秒）← **秒级说明 fwrite 卡住** |

### 3.2 实测数据（两次采样，间隔 18 秒）

| 计数 | t+15s | t+33s | 增量 | 解读 |
|---|---|---|---|---|
| VI `IntCntPerSec` | 30 | 30 | — | sensor 一直在 30fps 出图 ✅ |
| VPSS `RecvPic0` | 280 | — | — | 收到了 |
| VENC `VpssSnd` | 252 | 779 | +527 | VPSS 送了 527 帧 |
| VENC `Send` | 214 | 322 | +108 | 编码器只收下 108 帧 |
| **VENC `Full`** | **38** | **457** | **+419** | **419 帧被输入队列丢弃** ❌ |
| VENC `VencInt` | 208 | 316 | +108 | 编码产出 108 帧（≈6fps） |
| STREAM `BusyCnt` | 200 | 191 | — | 码流队列**堆满**（200/200） |
| STREAM `UserGet` | 12 | 129 | +117 | 取流线程 18 秒只取走 117 帧（6.5fps） |
| STREAM `Interval` | 1,650,000µs | 8,966µs | — | **1.65 秒/次**的取流间隔 |
| VI CHN `VbFail` | 19 | 103 | +84 | 下游不还缓存，VI 也丢了 103 帧 |

---

## 4. 把反压链条画出来

```
fwrite() 写 jffs2 被 GC 阻塞（最长 1.65s / 5.6s）
        ▲
        │ 8. 取流线程（select→GetStream→fwrite→Release）节流到 6.5fps
        │
VENC STREAM STATE: BusyCnt=200 满（已编码但没人取）
        ▲
        │ 7. 码流环形缓冲写满 → 编码器无法输出新的帧
        │
VENC 编码器停摆（VencInt 只剩 6fps）
        ▲
        │ 6. 输入队列（VENC PIC QUEUE）堆满
        │
VENC SEND1: Full=457（新帧直接丢）
        ▲
        │ 5. VPSS 送进来的帧被丢
        │
VI CHN VbFail=103（下游不还 VB，VI 申请不到新缓存 → 丢帧）
        ▲
        │ 4. VB 池耗尽
        │
GC2053 仍然老老实实 30fps 出图（冤枉）
```

**核心认知：编码链路是"背压（back-pressure）"系统**。你看到的"帧率低"往往不在采集端，而在最下游——**写文件、网络发送、上层消费跟不上，会把整条链反向压死**。

---

## 5. 定位到存储：为什么是 jffs2

```bash
$ mount | grep ' / '
/dev/root on / type jffs2 (rw,relatime)

$ df -h /
Filesystem   Size  Used Available Use% Mounted on
/dev/root   27.0M 22.4M      4.6M  83% /          ← 只剩 4.6MB

$ df -h /tmp
Filesystem   Size  Used Available Use% Mounted on
/dev/root   27.0M 22.4M      4.6M  83% /          ← /tmp 不是 tmpfs，也是 jffs2！
```

两个致命点：

1. **根分区 27MB，已用 83%**
2. **`/tmp` 不是内存盘**，和 `/` 是同一个 jffs2 分区

### jffs2 的写放大与 GC

jffs2 是**日志结构（log-structured）**文件系统：

- 写入不是原地覆盖，而是往空闲块追加
- 空间不够时启动 **GC**：扫描、擦除、搬移有效数据
- **分区越满，GC 越频繁，写延迟越不可预测**（秒级尖峰很常见）

两路码流 4Mbps×2 ≈ 1MB/s，而 SPINOR 的实际持续写入能力只有几百 KB/s——一开始靠 page cache 顶着，写满后进入"擦-搬-写"循环，`fwrite` 直接阻塞几秒。

> **dd 测试会骗人**：`dd if=/dev/zero of=/root/t bs=64k count=64` 报 2.9MB/s，那是写进 page cache 的速度，不是 flash。要在**容量压力**下测（或加 `sync`、`conv=fdatasync`）才有意义。

---

## 6. 验证实验：换到 tmpfs，一切归位

```bash
mount -t tmpfs -o size=48m none /mnt
cd /mnt
( printf '0\nc\n'; sleep 20; printf '\n\n' ) | /root/sample_venc 0 > /tmp/vl.log 2>&1
```

录制中再抓 `venc`：

| 指标 | jffs2 上 | **tmpfs 上** |
|---|---|---|
| VENC `Full` | 457 | **0** |
| `VencInt` 速率 | ≈6 fps | **240 帧 / 8 秒 = 30.0 fps** |
| `BusyCnt` | 200（满） | **0** |
| `Interval` | 1,650,000 µs | **33,301 µs（≈33ms，正好 30fps）** |

文件侧验证：

```
20 秒录制 → 594 帧 → 29.7 fps（统计上等于 30）
文件大小 10,115,098 字节 / 20 秒 ≈ 4Mbps（CBR 达标）
I 帧 20 个，GOP=30 ✓
```

**结论：整条采集/编码链路一直是好的，问题 100% 在"往快满的 jffs2 写文件"。**

---

## 7. 工程规范（这次最重要的收获）

| 场景 | 正确做法 |
|---|---|
| 录流测试 | 挂 tmpfs（`mount -t tmpfs -o size=48m none /mnt`）再 `cd` 进去跑 |
| 需要保留录像 | 写 SD 卡 / NFS / U 盘，**永远不写系统分区** |
| 产品形态（监控） | 直接网络推流（RTSP/RTP），**不落地**——从根上没有这个问题 |
| 必须落盘的场景 | 监控分区剩余空间，写满前滚动删除；或用循环写+定时清理 |

### 顺带的运维动作

```bash
# 清理历史码流，把分区腾出来
rm -f /root/stream_chn*.h26*
df -h /        # 27.0M 16.8M 10.2M 62% /   ← 恢复到 62%
```

---

## 8. 排查方法论提炼（可复用）

1. **先证伪最简单的假设**：码流坏不坏（解码错误）、sensor 出帧快不快（VI 计数器）——两个 5 分钟就能排除
2. **不要在文件侧推断运行时行为**：ES 无时间戳，`ffprobe` 的 fps 是假的
3. **一定要看运行中的 /proc**：`/proc/umap/{vi,vpss,venc,mipi_rx}` 是海思平台的黑匣子
4. **两级采样看增量**：单次快照只能看状态，两次快照的差值才是速率
5. **顺着 Full / BusyCnt / VbFail 三个"丢弃点"找堵点**：它们分别对应"输入队列满"、"输出队列满"、"缓存池空"，是反压的三处指纹
6. **怀疑存储时先看 `df` 和 `mount`**：`/tmp` 未必是内存盘，别想当然

---

## 9. 速查：帧率异常的判据表

| 现象 | 最可能原因 |
|---|---|
| VI `IntCntPerSec` < 期望 | sensor 配置/fps 设置（cmos_fps_set） |
| VI `IntCntPerSec` 正常但 VI CHN `VbFail` 高 | VB 池不够或下游不还缓存（反压） |
| VENC `Full` 持续增长 | 编码器被堵（输出侧问题） |
| VENC `BusyCnt` 满 | **取流/写文件/发送太慢** |
| `Interval` 出现秒级尖峰 | 阻塞在 I/O（flash、网络、锁） |
| `VencInt` 高但 `UserGet` 低 | 取流线程本身慢或没被调度 |
