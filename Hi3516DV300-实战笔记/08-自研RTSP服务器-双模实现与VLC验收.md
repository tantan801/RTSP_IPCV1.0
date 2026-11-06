# 08｜自研 RTSP 服务器（从"能写一个吗"到 VLC 出图）

> 背景：大佬版移植后 UDP 正常（24.7fps）、TCP 拉流失败（`Nonmatching transport in server reply`），
> 与其继续在别人的 800 行里打补丁，不如从零写一个**自己的**：UDP+TCP 双模、真 SPS/PPS、多客户端。
> 结果：**VLC 成功播放 `rtsp://192.168.1.20:1234/live`，画面正常**；UDP/TCP 双模验收通过，
> 10 秒拉流 281/285 帧（≈28fps），解码错误 0。
> 产物：`sample_venc_own`，md5 `68ef0ca9812ffcd0caf0bb8d656144b4`

---

## 0. 概念铺垫：这篇笔记里的每个词都是什么意思

### 0.1 RTSP 和 UDP/TCP 到底什么关系？

初学最容易混的点，一句话拆开：

```
RTSP 只是一个"应用层对话协议"，它自己不搬数据。
它规定了一套文本命令（OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN），
告诉客户端"用什么方式、到哪个地址去收视频数据"。

真正搬数据的活分两种干法（在 SETUP 时协商）：
  UDP 模式：视频数据走单独的 UDP 端口（RTP 包），信令归信令、数据归数据
  TCP 模式（interleaved）：视频数据直接"塞进"信令那条 TCP 连接里混着发
```

两种模式的数据封装对比：

| | UDP 模式 | TCP interleaved 模式 |
|---|---|---|
| 数据走哪 | 独立 UDP 端口（如 client_port 45100/45101） | 复用 RTSP 信令那条 TCP 连接 |
| 包前面加什么 | 什么都不加，就是裸 RTP 包 | **`$`（0x24）+ 1 字节通道号 + 2 字节长度**，再接 RTP 包 |
| 丢包 | 会丢（UDP 无重传），表现为花屏 | 不丢（TCP 重传），但网络差时延迟变大 |
| 适用 | 局域网、带宽够、容忍花屏 | 跨公网、要求完整 |

> **知识锚点**：抓 TCP 模式的包，开头字节是 `24 00 xx xx`（`$` + channel 0）——看到 `$` 就知道这一段是数据不是信令文本。这是区分"控制"和"数据"混流的最快指纹。

### 0.2 和 MQTT 像吗？

像了一半。用一张表说清楚：

| | MQTT | RTSP |
|---|---|---|
| 本质 | **消息分发**总线（pub/sub） | **流媒体会话**协议 |
| 状态 | 基本无状态，broker 只管转发 | **强状态机**：会话建立→协商→播放→拆除，服务器要记住每个客户端的状态 |
| 数据 | 一条条独立小消息 | 连续不断的实时流，有时序、有丢包语义 |
| 典型交互 | publish 一条温度，订阅者收到就完事 | DESCRIBE→SETUP→PLAY 一套握手完成后才开始"上菜" |
| 断开后果 | 没收到就没了（可 QoS 补） | 会话悬空，服务器要处理 TEARDOWN/超时/半途断开 |

共同点只有一个：都跑在 TCP/UDP 之上，都是应用层协议。RTSP 的"控制"角色更像 HTTP（请求-响应），而它的"数据"角色（RTP）才是流媒体的本体。

### 0.3 这台服务器里出现的并发词

| 术语 | 意思 | 在本文里的角色 |
|---|---|---|
| **accept 线程** | 专门负责"接电话"的线程，只干 accept()，接完分给干活的人 | 独立线程，主线程不阻塞在 accept 上 |
| **客户端队列** | 每个连上来的客户端一个结构体（socket、状态机、RTP 序号/时间戳） | calloc 从堆里分配，支撑多客户端 |
| **发送队列** | 编码线程和发送线程之间的缓冲带：编码快、网络慢时的蓄水池 | 见 2.1 的 segfault 大坑 |
| **BSS 段** | C 程序里"未显式初始化的全局/静态变量"所在内存区，**编译时就定死大小** | 12MB 的静态队列差点把程序撑死，见 2.1 |

### 0.4 排障工具词

| 术语 | 意思 |
|---|---|
| **tmpfs 叠加挂载** | 对同一目录 mount 多次 tmpfs，会形成**多层独立实例**（像盖楼），umount 一次只揭一层；每层互不相通。数据都在内存里，不占 flash |
| **lazy umount** | `umount -l`：目录还有人用时不强拆，等引用清了再摘——救援"被占死"挂载点的手段 |
| **Shmem**（/proc/meminfo） | 共享内存总量列。tmpfs 的文件全算在这里——**用它判断 tmpfs 到底吃掉了多少内存** |
| **raw_grab.py** | 自己写的 python 脚本：不借助 ffmpeg，手写 RTSP 握手并**抓取 TCP 上的原始字节**，直接看 RTP 包头 |
| **二分验证程序** | 把可疑代码逐段裁剪编译（bss2/va/vb/vc/vt/vd...），每次跑一次看崩不崩，把 segfault 的责任范围一步步砍半 |

---

## 1. 立项：为什么自己写

大佬版的三个硬伤（第 07 篇遗留问题）：

1. **TCP 模式坏了**：`Nonmatching transport in server reply`，ffmpeg 直接退出——他的代码只有 UDP 路径是活的
2. **SDP 里 SPS/PPS 是假串**（`"AAABBCCC"`），严格客户端不兼容
3. **单客户端**，且整段代码 800+ 行和 mp4v2、私有宏纠缠，改动风险高

自研目标（一次到位）：

| 目标 | 实现 |
|---|---|
| UDP + TCP 双模 | SETUP 解析客户端 transport，两种都实现 |
| 真 SPS/PPS | 服务器起来后从第一帧 I 帧提取 SPS/PPS，base64 填进 SDP |
| 多客户端 | 客户端链表 + accept 独立线程 |
| 代码可控 | ~530 行单文件 `rtsp_server.c/h`，不依赖第三方库 |

接入方式沿用第 07 篇的最小手术：`sample_comm_venc.c` 里 chn0 码流挂钩一行，改成调自己的 `rtsp_server_push_stream(&stStream)`；`RTSP_ENABLE 0` 关掉大佬版，新宏 `RTSP_SERVER_ENABLE 1` 开自研版。

---

## 2. 六连 bug：每一环都是一类教训

自研第一版跑起来 `./sample_venc_own 0` 直接 **Segmentation fault，零输出**——但这 6 个 bug 里，真正值得记的是每个 bug 背后的"排障方法"。

### 2.1 bug① Segfault 零输出：内存被 tmpfs 吃穿了（最大的一课）

**表象**：程序一行没打印就死。

**第一反应全错了**：怀疑 BSS 越界、线程栈太小、ABI 不兼容……盲改无果。

**正确的路：用 malloc 逐级探测内存水位。** 写小程序从 8MB 开始 malloc 试试：

```
malloc  8MB → 成功
malloc 12MB → 失败      ← 系统实际只剩 8~12MB 可用
```

再查 `/proc/meminfo`，Shmem 一列高得离谱（≈90MB）——**tmpfs 里的东西全算在这**。

**根因链**：
```
/mnt 上叠了 4 层 tmpfs（重复 mount 的历史遗留）
+ 每层里都躺着之前录流的 h264 文件（jffs2 写满问题的临时方案）
→ tmpfs 是内存盘，全吃 RAM（osmem 只有 128MB！）
→ 系统 Redisposable 内存剩不到 12MB
→ 自研版用静态数组当发送队列，BSS 段 12.1MB
→ 程序加载时 exec 映射 BSS 失败 → Segmentation fault，一行都没跑
```

**修复**：
1. `umount -l` 逐层卸掉叠加的 tmpfs，清掉历史录流文件
2. **发送队列从静态数组改成 calloc 堆分配**——BSS 从 12.1MB 降到 **527KB**，从根上免疫这类问题

> **三条永久教训**：
> ① `/tmp`、`/mnt` 这种目录里"躺着什么"和"占用了多少内存"是两个问题，`meminfo` 的 Shmem 列是 tmpfs 的账本；
> ② **大块全局数组是嵌入式程序的自杀式写法**，动态内存按需分配；
> ③ 排 segfault 先问"程序真的被加载进去了吗"，再问"代码哪错了"——映射失败和逻辑错误表象一模一样。

**方法论沉淀——二分验证程序**：把工程逐段裁剪成 bss2/va/vb/vc/vt/vd 等最小复现程序，每次只留一个变量，跑一次砍一半嫌疑范围。segfault 这种"零输出"问题，二分是唯一不靠运气的路。

### 2.2 bug② telnet 会话关闭杀子进程（SIGHUP）

长测试跑一半，启动它的 telnet 会话一关，板上的 sample 跟着死——**SIGHUP 是 shell 会话关闭时给前台/子进程发的"清算"信号**。教训：长跑测试的启动会话必须保持；以后要固化就用 `&` + nohup 语义或 init 脚本。

### 2.3 bug③ DESCRIBE 响应被 64 字节截断

SDP 响应头 + sprop-parameter-sets 早就超过 64B，`buf[64]` 直接截断，ffmpeg 收到不完整的头解析失败退出。**修法**：`buf[512]`。

> 教训：**协议文本响应的缓冲区要按"最长可能"给，不能按"现在看着够"给**。RTSP 是文本协议，截断不会报错，只会让对端"莫名其妙退出"。

### 2.4 bug④ VENC pack 的两个隐藏结构（码流内容损坏）

现象：dump 下来的裸流 4.7MB 只解出 4 帧。两个根因：

1. **`u32Offset` 没处理**：pack 的有效数据是 `[pu8Addr+u32Offset, pu8Addr+u32Len)`，直接从 pu8Addr 拷会把无效前缀混进去
2. **大帧多 pack 且后续 pack 无起始码**：一帧 100KB+ 会被 VENC 拆成多个 pack，后面的 pack 不带 00 00 00 01 开头——按"起始码切 NALU"的朴素逻辑会把它们整段丢弃

**修法：整帧聚合**——push_stream 收到流先遍历所有 pack、按 offset/len 拼出完整一帧，再进 NALU 切分。

> 知识点：海思 VENC 一次回调给你的 `VENC_STREAM` 包含若干 `VENC_PACK`，**pack ≠ NALU ≠ 帧**，三者边界必须自己理清楚，不能假设。

### 2.5 bug⑤ FU-A 分片载荷多带了一个 NALU 头（最隐蔽）

现象链：dump 裸流完美 → 落地文件解得开 → 但接收端全花 → 问题只可能在 **RTP 打包层**。

**取证手段是这节的精华**：不信任任何中间层，用 raw_grab.py 手写 RTSP 握手，把 TCP interleaved 通道的**原始字节**抓下来，肉眼对 RTP 包头：

```
预期：7c 85 | b8 ...          （FU indicator + FU header + 载荷）
实际：7c 85 65 b8 ...         ← 多了一个 0x65（原 NALU 头！）
```

**根因**：FU-A 分片时 `memcpy(fu+2, nalu+sent, n)` 没跳过 1 字节 NALU 头。规范要求：**分片后的载荷不含原 NALU 头**——NALU 类型信息已经搬进 FU header 了，再带一份就是"序号错位"，每个分片整体后移 1 字节，解码器解出来的全是乱码。

```c
/* 修复 */
memcpy(fu + 2, nalu + sent + 1, n);   /* 跳过 1 字节 NALU 头 */
int total = nalu_len - 1;             /* 总长同步减一 */
```

> 教训：**"文件能解但网络上不能解"= 问题在打包层，与编码无关**。这个二分判断把 6 个嫌疑方向一下压到 1 个。

### 2.6 bug⑥ 调试 dump 把发送线程堵死

第一版加调试代码往 /tmp 写 dump 文件——忘了 /tmp 是 jffs2！写盘毫秒变秒，发送线程被 fwrite 阻塞，队列瞬间 full，**600 帧直接丢**。而队列 full 的掉帧又被误判成 bug①的残留。

**修法**：dump 用 `RTSP_DUMP_DEBUG` 宏开关，**默认关闭**；打开时也只写 tmpfs。

> 教训：第 05 篇的"jffs2 写满压垮链路"换了个马甲（调试日志）再犯一次。**嵌入式板上任何落盘行为都要先问：这个分区是 flash 还是内存？**

---

## 3. 六个 bug 的分类学（以后遇到直接对号入座）

| # | bug | 表象 | 根因类别 | 一句话教训 |
|---|---|---|---|---|
| ① | segfault 零输出 | 一行不打印就死 | **环境**（tmpfs 吃内存）+ 架构（BSS 过大） | 先测内存水位，再查代码 |
| ② | 进程莫名消失 | 跑一半死 | 会话生命周期 | 长测试守住启动会话 |
| ③ | 客户端连接即断 | ffmpeg 秒退 | 协议实现（缓冲区过小） | 文本协议缓冲按最长给 |
| ④ | 码流损坏 | 落盘解不开 | 厂商 API 语义（pack 结构） | pack≠NALU≠帧，边界自己理 |
| ⑤ | 画面全花但文件正常 | 文件好、网络坏 | 协议规范（FU-A 载荷） | 抓原始字节，不信中间层 |
| ⑥ | 队列 full 掉帧 | 批量丢帧 | 资源阻塞（flash 写） | 落盘先问分区类型 |

规律一目了然：**6 个 bug 里 4 个不是"RTSP 写错了"，而是环境/厂商 API/资源管理的坑**。自研一个协议服务器，代码本身反而是最不难的部分。

---

## 4. 验收：双模式 + 真播放器

### 4.1 验证链

```bash
# 板子（telnet）：
cd /mnt && ./sample_venc_own 0      # sample 起来，RTSP server 监听 1234

# PC/VLC：打开网络串流
rtsp://192.168.1.20:1234/live

# VM/ffmpeg 两种模式分别拉流数帧：
ffmpeg -rtsp_transport udp -i rtsp://192.168.1.20:1234/live -t 10 -c:v copy udp.h264
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.20:1234/live -t 10 -c:v copy tcp.h264
ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 udp.h264
```

### 4.2 结果

| 指标 | UDP | TCP |
|---|---|---|
| 10 秒帧数 | **281（28.1fps）** | **285（28.5fps）** |
| 解码错误/花屏 | 0 | 0 |
| VLC 实时播放 | ✅ 画面正常 | — |
| SDP 真 SPS/PPS | ✅ | ✅ |

帧数口径说明：裸流没有时间戳，ffprobe 数帧数 ÷ 拉流时长（板端 PLAY→TEARDOWN 实际间隔）才是真实 fps；28/30 的残差在拉流起止的边界帧内，链路本身 30fps 满帧无丢包（第 05 篇已证）。

**里程碑**：一个从零手写、无第三方库的 RTSP 服务器，UDP/TCP 双模 + 真 SPS/PPS + 多客户端，在标准播放器 VLC 上验收通过。

---

## 5. 产物与回滚

```
板子   : /mnt/sample_venc_own           md5 68ef0ca9812ffcd0caf0bb8d656144b4
VM     : /srv/tftp/sample_venc_own      （同 md5）
源码   : sample/venc/rtsp_server.c / rtsp_server.h   ← 自研主体（~530 行）
       : sample/common/sample_comm.h      RTSP_ENABLE=0（大佬版关）、RTSP_SERVER_ENABLE=1
       : sample/common/sample_comm_venc.c 挂钩 if(0==i){ rtsp_server_push_stream(&stStream); }
       : sample/venc/sample_venc.c        RtspServer_init() → rtsp_server_start()
拉流   : VLC/ffplay/ffmpeg → rtsp://192.168.1.20:1234/live
前置   : mount -t tmpfs -o size=48m none /mnt && cd /mnt   （重启后必做！）
回退   : RTSP_SERVER_ENABLE 0 重编，即回到无 RTSP 的纯落盘版
```

## 6. 判据速查（本次新增）

| 症状 | 指纹 | 根因 |
|---|---|---|
| 程序一行不打印就 segfault | malloc 探测水位低 + Shmem 异常高 | tmpfs 吃穿内存，BSS 映射失败 |
| 文件能解、网络全花 | raw 抓包看 RTP 头 | 打包层 bug（如 FU-A 多带头） |
| 客户端秒断无报错 | 响应头不完整 | 文本缓冲区截断 |
| 队列 full 大量丢帧 | 发送线程卡在写文件 | 调试 dump 写了 flash 分区 |
| TCP 拉流 Nonmatching transport | server 回的 transport 对不上 | 只实现了 UDP 路径 |

## 7. 遗留与下一步

| # | 事项 | 说明 |
|---|---|---|
| 1 | RTP 时间戳仍写死 30fps | 改成 `u64PTS` 差值动态算（与第 07 篇同一遗留） |
| 2 | TCP 模式 VLC 复验 | VLC 默认 UDP 协商，设置里开"RTP over RTSP"即强制 TCP |
| 3 | SDP Content-Base 乱码 | 大佬版遗留，自研版已顺带修掉 |
| 4 | **OSD 叠加** | 下一步主线：REGION 在 VPSS/VENC 通道上打时间戳水印 |
| 5 | 板子重启后 tmpfs 方案失效 | /mnt 要重新 mount tmpfs，长期可固化成启动脚本 |
