# 06｜RTSP 推流方案选型与移植路线

> 目标：让板子把 VENC 的实时码流推成 RTSP，VLC/ffplay/Qt 客户端能直接播
> 结论：SDK 完全没有 RTSP 能力；资料包带 live555（仅文件轮播）；最优解是移植大佬手写的轻量 RTSP 服务器

---

## 1. SDK 里有什么？（已全盘搜过）

| 查法 | 结果 |
|---|---|
| `find . -iname '*live555*' -o -iname '*rtsp*'` | **0 个文件** |
| `mpp/component/` | 只有 `hifb`、`isp` |
| `mpp/sample/` | 20 个目录：vio/venc/vdec/vo/vgs/vpss/audio/uvc/snap/fisheye/tde… **无 RTSP** |
| 源码 `grep -ri rtsp` | 仅 `hi_comm_isp.h` 几处注释 |

**海思 2.0.2.0 原厂 SDK 不含任何 RTSP/流媒体协议栈**。IPC 产品通常由方案商自己加（live555 或自研）。

---

## 2. RTSP 到底是什么（30 秒原理）

RTSP 是"带外控制协议"，真正的数据走 RTP：

```
客户端 ──OPTIONS──▶ 服务器     询问支持哪些方法
客户端 ──DESCRIBE─▶ 服务器     要 SDP（描述媒体：编码、时钟、参数集）
客户端 ──SETUP────▶ 服务器     协商传输方式（UDP/TCP，客户端 RTP 端口）
客户端 ──PLAY─────▶ 服务器     开始推流
                ◀──RTP/UDP──   码流（H.264 打包成 RTP 包）
客户端 ──TEARDOWN─▶ 服务器     结束
```

H.264 over RTP 的关键约定：

| 项 | 值 |
|---|---|
| payload type | 96（动态） |
| clock rate | 90000 Hz |
| 打包模式 | `packetization-mode=1`（支持 FU-A 分片） |
| 单包上限 | 通常 1400 字节（以太网 MTU 1500 − IP/UDP 头） |
| 大帧传输 | **FU-A 分片**：一个 NALU 拆成多包，首包带 Start 标志，末包带 End 标志 |
| 参数集 | SPS/PPS 通过 SDP 的 `sprop-parameter-sets` 或在码流里带 |

> 1080p 一帧十几 KB，必然要分片——这是 RTSP 服务器代码里最核心的一段逻辑。

### 2.1 全景：RTSP 只是选项之一

选定方案前，先把整个流媒体协议家族的谱系摆在桌上（做产品时这一页值钱）：

| 方案 | 传输方式 | 典型延迟 | 主要用途 | 实现代价 |
|---|---|---|---|---|
| **RTSP / RTP** | UDP（或 TCP 隧道） | 200–500ms | 局域网预览、NVR、VLC/ffplay | **低**（手写约 900 行） |
| **GB28181** | SIP 信令 + RTP/PS | 300ms–1s | **国标平台接入**、政府/公安项目 | 高（SIP + PS 封装） |
| **ONVIF** | SOAP/HTTP 信令 + RTSP 媒体 | — | 设备发现、能力协商、跨品牌兼容 | 中 |
| **HTTP-FLV** | HTTP 长连接 + FLV 封装 | 1–3s | 网页无插件预览 | 中 |
| **HLS** | HTTP + TS 切片 | 5–20s | 点播、大并发分发 | 低（延迟高） |
| **WebRTC** | SRTP/UDP（DTLS 加密） | **<300ms** | 浏览器实时、双向对讲 | 高（ICE/DTLS/SRTP） |
| **SRT** | UDP + ARQ 重传 | 200ms–1s | 弱网、公网长距离传输 | 中（需 libsrt） |

**一句话选型**：

- 自己看 / 毕设 / 大佬这个项目 → **RTSP**（最快见效）
- 卖给客户、要接平台 → **GB28181**（安防行业真正的门槛）
- 要网页里看 → **HTTP-FLV**（简单）或 **WebRTC**（要求低延迟时）
- 跨品牌对接、进别人平台 → **ONVIF**

### 2.2 顺带搞清 RTCP 是干嘛的

RTP 只负责"送"，RTCP 负责"反馈"：

| 协议 | 作用 |
|---|---|
| RTP | 装码流，带序号（判断丢包）和时间戳（判断抖动） |
| **RTCP** | 接收方周期性发报告：丢包率、抖动、往返时延 |

RTSP 的 SETUP 里客户端会协商**两个端口**（`client_port=5000-5001`，偶数 RTP、奇数 RTCP）。如果你的服务器只发 RTP 从不理 RTCP 端口，严格一点的客户端（部分 NVR、GStreamer）会认为流异常而断开——**这就是"VLC 能播、别的播放器不能播"的常见原因之一**。

---

## 3. 方案 A：资料包自带的 live555 demo

`03.开发板软件开发资料/01.Demo Code/10.live555/` 里有全套：

| 文件 | 说明 |
|---|---|
| `live555-2020-02.tar.gz` | 源码（14.9MB），带 `config.hi3516` 交叉编译配置 |
| `libssl-himix200.tar.gz` | 交叉编译的 OpenSSL（`libssl.so.1.1`/`libcrypto.so.1.1`） |
| `live555MediaServer` | **已交叉编译好的 ARM 二进制**（776KB，ELF32 ARM） |
| `stream_chn1.264` / `audio_chn0.aac` | 测试码流 |
| `readme.txt` | 官方步骤 |

官方步骤（readme 摘要）：

```bash
tar xzf live555-2020-02.tar.gz && tar xzf libssl-himix200.tar.gz
cd live2021
# 把 _install/lib 下的 .a/.so 拷到工具链 target/usr/lib
#   /opt/hisi-linux/x86-arm/arm-himix200-linux/target/usr/lib
./genMakefile hi3516
make
# mediaServer 目录下得到 live555MediaServer，拷到板子
./live555MediaServer
# VLC 打开 rtsp://<板子IP>/stream_chn1.264
```

### 它的局限（关键）

`live555MediaServer` 是 **文件点播服务器**：把 `.264` 文件按 RTP 循环推给客户端。它**不接 VENC**，所以：

- ✅ 可以用来验证：网络通、端口通、播放器能解、板子 ABI 兼容
- ❌ **不能做实时监控**（客户端看到的是历史录像在循环）

依赖注意：该二进制依赖 `libssl.so.1.1` + `libcrypto.so.1.1`，板端 `/usr/lib` 没有，需要从 tar 包拷过去。

---

## 4. 方案 B：大佬的手写 RTSP 服务器（推荐）

`smarthome-master/hisi3516/venc/sample_venc.c`（3050 行）里，他**直接集成了一套轻量 RTSP 服务器**：纯 socket + pthread，**零第三方依赖**。

### 4.1 代码结构

| 行号 | 函数/对象 | 作用 |
|---|---|---|
| 35–70 | `#if RTSP_ENABLE` 头文件段 | 开关宏控制编译 |
| 69–284 | `common/sample_comm.h` 里的定义 | `RTP_FIXED_HEADER`、`NALU_HEADER`、`FU_INDICATOR`、`FU_HEADER`、`RTSP_CLIENT`、`MAX_RTSP_CLIENT=1` |
| 92 | `RTPbuf_head` 链表 | 待发送包队列 |
| 245 | `OptionAnswer()` | OPTIONS |
| 269 | `DescribeAnswer()` | DESCRIBE，返回 SDP（`m=video RTP/AVP 96` + `H264/90000`） |
| 403 | `SetupAnswer()` | SETUP，解析客户端 `client_port`，分配本地 RTP/RTCP 端口 |
| 452 | `PlayAnswer()` | PLAY，创建 UDP socket 连到客户端端口 |
| 487/511 | `PauseAnswer()` / `TeardownAnswer()` | 暂停/结束 |
| 535 | `RtspClientMsg()` | 每客户端会话线程，解析请求分发 |
| 628 | `RtspServerListen()` | 监听 554，accept 新客户端 |
| 724 | `VENC_Sent()` | **核心：RTP 打包（含 FU-A 分片）+ sendto** |
| 873 | `SAMPLE_COMM_VENC_Sentjin()` | 同步把 `VENC_STREAM_S` 直接发送 |
| 902 | `saveStream()` | 把 `VENC_STREAM_S` 拷进队列（异步，避免阻塞编码） |
| 949 | `RtspServer_init()` | 起 Listen 线程 + 发送线程 |
| 981 | `vdRTPSendThread()` | 从队列取包 → `VENC_Sent` 发送 |
| 2999 | `RtspServer_init();` | main 里启动 |

### 4.2 数据流

```
HI_MPI_VENC_GetStream()
        │  VENC_STREAM_S（多 pack）
        ▼
saveStream(&stStream)          ← 拷进 RTPbuf_head 链表（malloc 每包）
        ▼
vdRTPSendThread()  usleep(5000) 循环取
        ▼
VENC_Sent(buf,len)             ← 拆 NALU、FU-A 分片、加 RTP 头
        ▼
sendto(udpfd) → 客户端
```

### 4.3 移植清单（逐个文件）

| 文件 | 要做的改动 |
|---|---|
| `common/sample_comm.h` | 拷他 69–284 行的 RTP/RTSP 结构体与宏 |
| `venc/sample_venc.c` | 拷 RTSP 段：35–70（头文件）、92（队列）、245–990（全部函数）；main 里加 `RtspServer_init()` |
| `common/sample_comm_venc.c` | 在 `SAMPLE_COMM_VENC_GetVencStreamProc()` 里 `HI_MPI_VENC_GetStream` 成功后加一行 `saveStream(&stStream);` |
| `venc/sample_venc.c` | 编码类型改成 `PAYLOAD_TYPE_E enPayLoad[2] = {PT_H264, PT_H264};`（他的 RTSP 只支持 H.264） |
| 编译参数 | 加 `-DRTSP_ENABLE=1`（他的 Makefile.param 没带过来，宏不定义则整段 RTSP 代码等于不存在） |

> **重要坑**：他的 `saveStream()` / `Sentjin()` 在源码里**只有定义、没有调用**（他本地 SDK 里接了，仓库没带）。这行挂钩必须我们补，否则 RTSP 服务器起来了但一个包也发不出去。

### 4.4 他代码里的两个缺陷（要顺手修）

**缺陷 1：发送线程限速 ~200 包/秒**

```c
void* vdRTPSendThread(void* p) {
    while (1) {
        ...
        usleep(5000);      // 5ms → 最多 200 包/秒
    }
}
```

1080p 4Mbps ≈ 350 包/秒（1400 字节/包），200 包/秒会**积压→延迟越来越大**。

改法：`usleep(1000)` 或改成条件变量/队列非空时不睡。

**缺陷 2：SDP 里 SPS/PPS 是假串**

```c
pTemp2 += sprintf(pTemp2, "a=fmtp:96 packetization-mode=1; sprop-parameter-sets=%s\r\n", "AAABBCCC");
```

`AAABBCCC` 是占位符，不是真实 SPS/PPS。VLC 能靠码流里内嵌的 SPS/PPS 硬扛过去，但严格客户端（部分 IPC/NVR 软件、GStreamer）会失败。

改法两种：
- 简单：从 VENC 第一帧的 I 帧里提取 SPS(7)/PPS(8)，base64 编码后填进去
- 偷懒：客户端只支持 VLC 就先不管

### 4.5 另一个限制

`MAX_RTSP_CLIENT = 1`：**只能一个客户端**。多客户端需要改成客户端数组 + 广播（每包发给所有 RTSP_SENDING 的客户端）。

---

## 5. 方案对比

| 维度 | A: live555MediaServer | B: 手写 RTSP（大佬版） |
|---|---|---|
| 实时性 | ❌ 文件轮播 | ✅ 实时 |
| 依赖 | libssl/libcrypto | 无 |
| 集成成本 | 拷二进制即可 | 拷 ~900 行代码 + 1 行挂钩 |
| 协议完整度 | 高（标准实现） | 够用（OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN） |
| 扩展性 | 好（支持 RTSP over TCP、多客户端） | 弱（H264 only、单客户端） |
| 适合场景 | 快速验证通路 | 本项目主线（学习+可控） |

**建议：先用 A 花 10 分钟验证网络/播放器链路，再用 B 做实时流。**

---

## 6. 验证计划

```bash
# 板端（写在 tmpfs 里，别写 jffs2）
mount -t tmpfs -o size=48m none /mnt && cd /mnt
/root/sample_venc 0        # 起 RTSP 服务

# PC/VM
ffplay -rtsp_transport udp rtsp://192.168.1.20:554/stream_chn1.h264
# 或 VLC：媒体 → 打开网络串流
```

判据：
- 首帧时间 < 2 秒
- 画面连续无花屏（花屏多为 FU-A 分片序号/标志位写错）
- 延迟可接受（UDP 无重传，本身不保证顺序，局域网一般 < 300ms）
- `ss -u` 能看到 RTP 包在发

---

## 7. 检查清单

- [ ] 确认编译带了 `-DRTSP_ENABLE=1`（否则代码被预处理器整段丢弃，且**没有任何报错**）
- [ ] 确认 `saveStream(&stStream)` 已挂进 GetVencStreamProc
- [ ] 编码类型必须是 H.264（RTSP 段只处理 NALU，不认 H.265 的 2 字节头）
- [ ] 发送线程 `usleep` 改小，否则高码率积压
- [ ] 码流写盘走 tmpfs，别重演第 5 篇的坑
- [ ] 单客户端限制要提前知道（MAX_RTSP_CLIENT=1）
