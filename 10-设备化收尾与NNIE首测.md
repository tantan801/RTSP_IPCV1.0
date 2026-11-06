# 设备化收尾与 NNIE 首测

> 这一天的前半段（RTSP 移植 → 自研 server → OSD）已经各自成篇，本篇不重复。
> 只记前 9 篇**没有覆盖**的三件事：**关落流、开机固化与守护加固、NNIE 首测**，外加两个客户问题。
> 结构仍是同一套：做了什么 → 关键点 → 踩的坑 → 验收。
>
> ⚠️ **本篇 §02 的守护脚本设计已被第 11 篇推翻**：单实例锁的落盘介质、失败熔断策略、stop 的杀进程顺序都有问题，
> 最终版本见 [11-开机自愈](11-开机自愈-四个隐藏bug的排障实录.md)。本篇保留原始思路，方便对照"为什么第一版会错"。

---

## 00 今天走过的路，以及各自记在哪

| 段 | 主题 | 记录位置 |
|---|---|---|
| 一 | RTSP 移植（借大佬方案，修 3 bug，24.7fps） | 第 07 篇 |
| 二 | 自研 RTSP 服务器（~530 行，UDP/TCP 双模） | 第 08 篇 |
| 三 | OSD 三阶段（色块 → 点阵字 → 动态时间戳） | 第 09 篇 |
| 四 | 关落流 + 开机固化 + 守护加固 | **本篇 §01–02** |
| 五 | NNIE 首测（SSD 单图推理） | **本篇 §03** |

前三段的意义是「**看得见**」（出图、能远程看、画面上有字），后两段的意义是「**站得住 + 摸到下一步**」——设备化把演示版变成能自己活的东西，NNIE 首测证明这块板子真的能做 AI。

---

## 01 关落流：从"能演示"到"敢让它一直跑"

### 为什么必须关

`SAMPLE_COMM_VENC_GetVencStreamProc` 里的 `fwrite` 会把每一帧码流落盘到 `/mnt`（48MB tmpfs）。跑一段时间就满，然后反压整条链路——**这就是第 05 篇"帧率暴跌"的同一个坑换了一张脸**。

差别只在于：第 05 篇是自己主动录流做验证，这次是 sample 代码里默认就带的行为，没人记得关。

### 三文件补丁

| 文件 | 改什么 |
|---|---|
| `sample_comm.h` | 加 `#define VENC_SAVE_STREAM 0` |
| `sample_comm_venc.c` | 三处落流点（`fopen` / `SaveStream` / `fclose`）加条件编译；`pFile[i]` 置 NULL 后 `fclose` 要补 `NULL != pFile[i]` 判断 |
| `sample_venc.c` | 三个 `GetXxxMode` + **8 处**"press twice ENTER"加 `getenv("SAMPLE_VENC_AUTO")` 分支 |

第三项是为了能无人值守启动：非交互模式下直接返回默认参数（NORMALP / CBR / ROW），然后 `pause()` 常驻。

### 踩的坑一：`unsigned char` 把 EOF 截成 255

sample 原始代码：

```c
c = getchar();
if (c != EOF) goto Begin_Get;    // ARM 上 char 默认无符号！
```

`getchar()` 返回 -1 被截成 **255**，`c != EOF` 永远成立 → **goto 死循环，2.5M 行日志 45MB 直接把 tmpfs 写满**。这也是为什么要用 `SAMPLE_VENC_AUTO` 分支绕开，而不是指望 stdin 能给出 EOF。

> 这类坑的共同特征：**不报错，只是安静地做错事**。ARM 上 `char` 默认 unsigned，x86 上默认 signed——同一份代码两种行为。

### 踩的坑二：python 打补丁的事务性

补丁脚本在循环末尾才统一写盘，中途一个函数匹配失败抛异常 = **整批回滚**。结果 exit_wait 之后的 3 个 auto 补丁没落盘，`getenv` 只改了 8 处（应为 11 处），auto 模式不生效。

> 改多处时，每改一处就落盘一次，别攒到最后写。

### 踩的坑三：板子没有 `/var/log`

精简 rootfs 里这个目录不存在，首启疯狂循环报错。日志全部改到 `/mnt`（tmpfs）——反正掉电即丢，也不消耗 jffs2。

### 验收

`/mnt` 不再增长（16KB / 0%），编码帧率无退化，OSD 时间戳正常。

---

## 02 开机固化与守护加固

### 两个 init 脚本

```
/etc/init.d/S90telnet   telnetd 开机自启（重启后仍能远程进）
/etc/init.d/S99rtsp     等网 → 幂等挂 tmpfs → SAMPLE_VENC_AUTO=1 → ntpd → 3s 守护循环
```

`S90telnet` 编号在 `S99rtsp` 之前，这是有意的：**万一 S99 里的东西把进程搞挂，telnetd 已经起来了，还能远程救回来**。

`S99` 里的 tmpfs 挂载必须是**幂等**的：

```sh
grep -q " /mnt " /proc/mounts || mount -t tmpfs -o size=48m none /mnt
```

重复 mount 会叠层吃掉内存——第 08 篇那个段错误的根因就是这个，一次挂了三四层，Shmem 到了 90MB。

### 晚上暴露的两个真 bug（这才是这一段的价值）

**① 时间水印一直是错的**

```
date    → Thu Jan  1 00:09:33 UTC 1970
route -n → 没有 default 路由
```

两层原因叠加：

1. `S80network` 只写了 `ifconfig eth0 ... up`，**没配默认网关** → 板子出不了外网 → `ntpd -q` 永远失败
2. 就算网关通了，**busybox 的 `ntpd -q` 不带 `-n` 会先 daemonize**，`-q` 还没对时就退出了

修：S99rtsp 里补 `route add default gw`，NTP 改 `ntpd -n -q -p 203.107.6.88`。现在板子时间是准的。

> 也就是说：**下午验收"通过"的那个动态时间戳，走是走了，走的是 1970 年。**
> 这提醒一件事：验收标准定得不够硬。"秒数在动"证明了线程和双缓冲没问题，但没证明"时间是对的"。以后 OSD 的验收要加一条——和 PC 对表。

**② 守护脚本会自我打架**

为了跑 NNIE 反复停/起服务，暴露出旧脚本**没有单实例保护**：多次 `start` 留下 3 个 watcher，各自拉起 `sample_venc` 抢 VI，抢不到的那个每帧刷错误日志，**19MB → 34MB，把 48MB 的 tmpfs 直接撑爆**（`No space left on device`）。

根因很隐蔽：

```sh
echo $$ > $PIDFILE    # 看起来对，实际是父进程 pid
```

`start_daemon &` 是在子 shell 里跑的，**ash 的子 shell 里 `$$` 拿到的是父进程 pid**，pid 文件永远指向错误的进程，`kill -0` 检查形同虚设。

改法：

| 机制 | 做法 |
|---|---|
| 单实例 | `mkdir /mnt/rtsp.lock`（mkdir 是原子的）+ `echo $! > lock/watcher.pid`（`$!` 才是刚起的后台进程真 pid） |
| 日志保护 | 超 2MB 自动 `: > $LOG` 截断 |
| 熔断 | 连续 5 次非零退出就停机，不再无限重启刷屏 |
| 对时 | 启动时补网关 + `ntpd -n -q` |

验收：二次 `start` 明确回 `already running`；`killall` 后 3 秒自动拉起；`VencInt` 6 秒 213 帧（30fps 正常）。

> 这一段的教训不是"要写健壮脚本"，而是：**运维动作本身也会造成事故**。为了做 A 反复起停服务，反而把存储撑爆了。所以任何会自动重启的东西，都必须先有单实例锁和日志上限。

---

## 03 NNIE 首测：先证明硬件能算，再谈工程

### 思路：先跑最小可验证单元

不直接写实时链路，先做离线单图推理。原因：如果连一张图都算不出来，后面所有工程活都是白费。这一步把"N 种可能"压缩成"1 个方向"。

### 第一步：盘库存（不凭印象）

| 项 | 实际情况 |
|---|---|
| 示例 | `svp/nnie/`：**0 号是 `VI→VPSS→NNIE→VGS→VO` 实时链路**，5 号是 SSD 读文件 |
| 模型 | 22 个 `.wk`：SSD 26MB、YOLOv3 61MB、FasterRCNN 132MB、Segnet、MNIST…… |
| 人脸模型 | **一个都没有**（`find -iname "*face*"` 全空） |
| 转换工具 | `nnie_mapper` / RuyiStudio **不在 SDK 里**，要另找 HiSVP_PC 工具包 |
| 板端库 | `libnnie.so`、`libsvpruntime.so` 已就位，`hi3516cv500_nnie.ko` 已加载 |
| 内存 | MemTotal 118MB（空闲 92MB）；MMZ zone 896MB |

**最关键的一条**：0 号示例已经实现了"摄像头实时取帧 → 推理 → 画框 → 输出"，只是最后输出到 VO（本地 HDMI）。要做的是把这个输出换成 VENC 送进已有 RTSP，而不是从零写。

### 第二步：编译，撞上自己埋的雷

公共文件 `sample_comm_venc.c` 里有第 08 篇加的 RTSP 推流钩子，nnie 示例链接时报：

```
undefined reference to `rtsp_server_push_stream'
```

不改任何逻辑的修法——把宏改成可外部覆盖：

```c
/* sample_comm.h */
#ifndef RTSP_SERVER_ENABLE
#define RTSP_SERVER_ENABLE 1
#endif
```

```makefile
# svp/nnie/Makefile 末尾
CFLAGS += -DRTSP_SERVER_ENABLE=0
```

> 这条是通用经验：**给公共文件加钩子，等于给所有链接它的程序加依赖**。要么用弱符号，要么用可覆盖宏，别写死。

另外 `make clean` 不清 `../common/*.o`，共享文件改了必须手动 `rm -f ../common/*.o`，否则编出来的还是旧目标文件——第 02 篇排 MIPI 时在这上面浪费过大量时间。

### 第三步：板测

先停 S99rtsp（**MPP 的 VI/VPSS 是进程独占资源**，两个进程抢必有一个挂），再跑 5 号示例：

```
[Info]:Svp mpi init ok!
[Info]:Ssd Load model!
[Info]:Ssd parameter initialization!
[Info]:Ssd start!
[Info]:Ssd result:
==== The 7th class box info ====   179  43 273  86  0.997070
==== The 12th class box info ====   47 114 128 275  0.997314
[Info]:Svp mpi exit ok!
EXIT=0
```

输入 `dog_bike_car_300x300.bgr`，第 7 类 car、第 12 类 dog，和文件名对得上。

### 这次验证证明了什么、没证明什么

**证明了**：NPU 硬件工作、`libnnie.so` / `libsvpruntime.so` 正常、ko 已加载、26MB 模型能加载、后处理（置信度 + 坐标解析 + NMS）正确、内存够用。也就是**整条横贯链路健康**。

**没证明**（别高估）：

- ❌ 实时链路（VI → VPSS → NNIE）一次没跑
- ❌ 画框回灌（VGS → VENC → RTSP）没做
- ❌ 和 `sample_venc` 共存（资源进程独占，只能先停服务）
- ❌ 人形检测：跑的是通用 20 类 SSD，第 15 类才是 person，不是专用人形模型

### 部署位置

模型 26MB、二进制 3.8MB 全部放 `/mnt/nnie/`（tmpfs）——根分区只剩 4.9MB，**放根分区必然写满**。md5 已逐项核对。

> 代价：tmpfs 一断电就清空，下次要用得重新 tftp 传。源文件留在 VM `/srv/tftp/nnie/`。

---

## 04 旁支：两个客户问题

### RV1126B 的 alsa 音量存不下来

`alsactl store` 报 Invalid argument。根因：codec 的 `Power Amplifier` 控件是 `SOC_ENUM_EXT`，get/put 回调开头 `if (!rd->pa_ctl) return -EINVAL`；而 `pa_ctl` 来自设备树的 `pa-ctl-gpios`，**客户板 dts 里没配** → 恒为 NULL。

`alsactl store` 要遍历全部控件，踩到这一个就整体失败。官方 evb dtsi 都配了，客户板缺。

两个方案：A（不改内核）开机脚本 `amixer cset` 恢复音量；B 补 dts 或改内核回调。

### BOX2000（RK3588）root 登不上

客户设了 root 密码仍然 Access denied。**拆 `rootfs.img` 用 7z 直接开 ext4 实锤**：`/etc/shadow` 里是 `root:*`（无密码且锁定），"出厂默认 123456"的说法是错的；`sshd_config` 默认 `PermitRootLogin prohibit-password`，所以设了密码也不许 SSH 用密码登 root。

> 技巧：7z 可以直接从 `rockdev/rootfs.img` 提取任意文件验证出厂状态，不用烧机。

---

## 05 这一天踩的坑，按类型归一下

只归本篇涉及的（07/08/09 的坑各在原文里）：

| 类型 | 具体 | 通用教训 |
|---|---|---|
| **存储** | 落流写 tmpfs、日志无限增长、tmpfs 叠加挂载 | 任何"持续写"的地方都要有上限；挂载要幂等 |
| **构建** | `make clean` 不清 `../common/*.o`；公共文件加钩子污染其他 sample | 改公共文件前先想"谁还会链接它" |
| **脚本** | `$$` 在子 shell 是父 pid；`unsigned char` 吃掉 EOF；补丁脚本事务性 | ash 这些坑会安静地失效，不报错 |
| **认知** | 验收标准定得不够硬（"秒数在动"≠"时间对"）；"成功打印"≠成功 | 验收要量化到"对"，不只"在动" |

---

## 06 当前状态与下一步

| 项 | 值 |
|---|---|
| RTSP + OSD | ✅ 可用，VLC 出图，时间戳正确（NTP 已修） |
| 开机自启 + 自愈 | ✅ S90telnet + S99rtsp，单实例锁 + 日志上限 + 失败熔断 |
| 关落流 | ✅ `VENC_SAVE_STREAM 0`，`/mnt` 不再增长 |
| NNIE 离线单图 | ✅ SSD 出两框（0.997 / 0.997） |
| NNIE 实时链路 | ❌ 未开始（S2） |
| 根分区 | jffs2 剩 4.9MB，**别再往里放东西** |

### S2：把 NNIE 挂进现有进程

**为什么必须合并进同一进程**：`VI` / `VPSS` 是进程独占资源，第二个进程 `HI_MPI_VI_EnableDev` 必然失败。只能作为线程长在 `sample_venc` 里，共享同一套上下文——好处是零拷贝，帧指针直接递过去。

改动面（已读源码核实，不是推测）：

| # | 改哪 | 内容 |
|---|---|---|
| ① | `sample_venc.c:1488` | `SIZE_S stSize[2]` → `stSize[3]`（不扩会读到栈垃圾） |
| ② | `sample_venc.c:1506` | `abChnEnable = {1,1,0}` → `{1,1,1}` |
| ③ | 新增 chn2 尺寸 | 300×300（= SSD 输入尺寸），不绑 VENC，由推理线程主动 `GetChnFrame` |
| ④ | 新文件 `nnie_detect.c/h` | init / start / thread / stop，约 400 行 |
| ⑤ | 全屏 RGN | 1920×1080 ARGB1555 双缓冲约 8.3MB，**只重绘框线像素，绝不 memset 整块** |
| ⑥ | Makefile | 补链接 NNIE 库 |
| ⑦ | `main` | 两行挂钩（start 在起流前，stop 在 UnBind 前） |

**约束**：`VPSS_MAX_PHY_CHN_NUM = 3`，chn2 是最后一路物理通道，**没有第 4 路**。

**开工前要先解决**：VPSS 出 YUV420SP，SSD 模型吃 RGB planar（测试图 300×300×3 = 270000 字节印证），中间要做色彩空间转换。0 号示例肯定做了这一步，**先去读它怎么做，不自己发明**。

**验收四条**：

1. 终端有实时检测日志（类别 + 置信度 + 坐标）
2. VLC 有框，且框跟着目标动
3. 时间戳还在走（证明两个 RGN 没打架）
4. `VencInt` 6 秒仍 ≥ 180 帧（**30fps 不掉**，这条最容易被忽略，也是这个方案最大的风险）
