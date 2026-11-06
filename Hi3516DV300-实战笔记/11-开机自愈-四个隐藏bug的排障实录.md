# 开机自愈：从"重启后服务不启动"挖出的四个隐藏 bug

> 目标只有一个：**打完电话，VLC 就能看到画面**。
> 结果网络、时钟、进程、启动脚本各埋了一个 bug，链条是「一个掩盖一个」，必须按层剥。
> 结构照旧：现象 → 取证 → 根因 → 修法 → 验收。

---

## 00 结论先行

| 层 | 现象 | 根因 | 一句话修法 |
|---|---|---|---|
| 网络 | ping 不通，但 `arp -a` 有 MAC | 板子 `/24`，网关 3.1 在网段外，回包发不出去 | 掩码改 `/16` 再补网关 |
| 时钟 | `date` 停在 1970 | 无网关 → NTP 出不去；无电池 RTC | `ntpd -q` + 候选网关探测 |
| 时钟 | 每次重启快 8 小时 | `hwclock -w` 写的是**本地时间**，内核按 UTC 读 | `hwclock -w -u` |
| 进程 | 画面卡、日志刷 `0xa0088012` | 两个 `sample_venc` 抢同一 VENC/554 | 清干净 + 重写 stop |
| 启动 | 重启后服务**永不启动** | 锁目录落在 jffs2，跨重启残留 → 判定 "already running" | 锁必须在 tmpfs |
| 启动 | 掩码被改回 `/24` | 备份 `S80network.bak_gw` 放在 `/etc/init.d/`，rcS 会执行它 | 该目录不留 .bak |

四个 bug 里有两个是**我自己造成的**（.bak 命名、锁的位置），另外两个是原始设计缺陷（stop 不彻底、hwclock 语义）。记这篇的目的不是"又修好了"，而是**这四类错以后不要再犯**。

---

## 01 第一层：网络——"收到了但回不去"

### 现场

```
PC   192.168.5.88/16   网关 192.168.3.1
板子 192.168.1.20/24   （S80network 里写死的 255.255.255.0）
```

三条命令就能定性：

```sh
ping -n 2 192.168.1.20        # 100% 丢包
arp -a | grep 192.168.1.20    # 00-c0-97-72-49-3a  ← 有 MAC
ifconfig eth0                 # RX 5671 / TX 10
```

### 判据表（这张表值钱）

| 现象组合 | 结论 |
|---|---|
| `arp -a` 有 MAC | **二层通**，网线/网口/PHY 都没问题 |
| ping 超时 | **三层不通**，问题在 IP、掩码、路由 |
| RX 涨、TX 不涨 | 板子**收到了**，但**发不回去**——典型"回包不通" |

### 为什么回不去

内核转发回包时查路由表：
1. 目的 `192.168.5.88` 在 `192.168.1.0/24` 里吗？**不在**
2. 有 `default gw` 吗？**没有**

两条都不成立 → `-ENETUNREACH`，包在协议栈就被丢掉，**连网卡发送队列都不进**，所以 TX 几乎不动。

### 修法（顺序不能反）

```sh
ifconfig eth0 192.168.1.20 netmask 255.255.0.0 up
route add default gw 192.168.3.1
```

先改掩码是**前提**：`/24` 下 3.1 落在网段外，直接 `route add` 会报 `Network is unreachable`。

### 验收

```
default via 192.168.3.1 dev eth0
192.168.0.0/16 dev eth0  src 192.168.1.20
ping 192.168.3.1   → ttl=64   1.879/0.982 ms   （网关直连，一跳）
ping 192.168.5.88  → ttl=128  1.866/3.485 ms   （ttl=128 是 Windows，说明真回到 PC 了）
```

> TX 从 10 涨起来、`ttl=128` 出现，这两条才是"回包通了"的铁证。

---

## 02 第二层：信令——先确认"有东西可拉"

不急着上 VLC，先用脚本把 RTSP 跑一遍，把"信令"和"媒体"分开验。

```python
# 554 端口探测 → DESCRIBE → 看 SDP
RTSP/1.0 200 OK
s=H.264 Live
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1; sprop-parameter-sets=Z0IAKpY1QPAET8s3AQEBAg==,aM4xsg==
```

`sprop-parameter-sets` 里带真 SPS/PPS，说明**编码器在产出**，不是空壳 server。

再完整走一次 SETUP/PLAY 数 RTP 包：

```
OPTIONS / DESCRIBE / SETUP / PLAY → 全部 200
Transport: RTP/AVP/UDP;unicast;client_port=63762-63763;server_port=554-555
RTP packets: 1574, bytes: 2118028, avg 1346 B/pkt, throughput 4.24 Mbps
```

**这一步的意义**：后面 VLC 黑屏时，能立刻区分"信令没通"还是"媒体没来"。

---

## 03 第三层：时钟——三个独立问题叠在一起

### 3.1 停在 1970：NTP 根本没出去

```
~ # date
Thu Jan  1 00:30:12 UTC 1970
```

手动跑一次就露馅：

```
~ # ntpd -n -q -p 203.107.6.88; date
ntpd: setting time to 2026-11-06 02:16:09.994619 (offset +1793929438.865444s)
Fri Nov  6 02:16:09 UTC 2026
```

`offset +1793929438s` ≈ **56.8 年**，正好是 1970 → 2026 —— 对时成功，但**只能手动成功**。

原因：S99rtsp 里写的是 `route add default gw 192.168.1.1`（家里的网关），公司环境下必然失败且被 `2>/dev/null` 吞掉，于是 NTP 请求出不去，`ntpd -q` 静默退出。

### 3.2 `ntpd -q` 会先 daemonize

busybox 的 `ntpd -q` 在部分版本会先 fork 再对时，导致 `date` 读到的还是旧值。加 `-n`（前台）才能保证"退出即完成"：

```sh
ntpd -n -q -p 203.107.6.88
```

### 3.3 时区：`/etc/TZ` 这个 busybox 不认

```
~ # echo "CST-8" > /etc/TZ; date
Fri Nov  6 02:22:59 UTC 2026          ← 没用
~ # sh -c 'export TZ=CST-8; date'
Fri Nov  6 10:22:17 CST 2026          ← 有用
```

结论：**只能靠环境变量**。但 `export` 只影响当前进程树——

> 串口里敲 `date` 依然显示 UTC，是因为你那个 shell 不继承启动脚本的环境；
> **OSD 水印是在 sample_venc 进程里取的，只要 S99rtsp 里 export 了就一定对**。

验证方法（别在登录 shell 里瞎猜）：

```sh
sh -c 'export TZ=CST-8; date'
```

### 3.4 RTC：写的是本地时间，读的时候按 UTC

这块最开始判断错了。`hwclock -r` 一开始是 1970（以为没电池），后来发现：

```
~ # hwclock -r        →  Fri Nov  6 18:49:59 2026
~ # date -u           →  Fri Nov  6 10:50:01 UTC 2026
```

差 8 小时 —— 根因是脚本里的 `hwclock -w` 默认把**本地时间**写进 RTC，而内核启动时 `hctosys` 把 RTC **当 UTC** 读。于是每重启一次就快 8 小时，越滚越偏。

修法：`hwclock -w -u`（RTC 里存 UTC），并且**只在 ntpd 成功后才写**：

```sh
if ntpd -n -q -p 203.107.6.88 >/dev/null 2>&1 ; then
    hwclock -w -u 2>/dev/null && blog "ntp: ok, rtc synced(utc)"
else
    blog "ntp: FAILED (no network?)"
fi
```

---

## 04 第四层：进程——两个 sample_venc 抢同一条码流

### 症状

```sh
ps | grep sample_venc | grep -v grep
285 root  0:04 /usr/bin/sample_venc 0
307 root  0:00 /usr/bin/sample_venc 0
```

日志每 12 秒刷出 **937KB**：

```
[SAMPLE_COMM_VENC_GetVencStreamProc]-2121: NOTE: Current  frame is NULL!
[SAMPLE_COMM_VENC_GetVencStreamProc]-2144: HI_MPI_VENC_GetStream failed with 0xa0088012!
```

`0xa0088012` = VENC 取流失败（通道被占/无可用帧）。两个进程取同一通道，谁都拿不到完整帧。

### 追父进程，一步定位

```sh
grep PPid /proc/285/status   →  PPid: 168
grep PPid /proc/307/status   →  PPid: 296   （296 是刚启动的 watcher）
cat /proc/168/cmdline        →  /bin/sh/etc/init.d/S99rtsp
```

**168 是上一轮遗留的老 watcher**：`stop` 只杀了子进程，它发现子进程没了，睡 3 秒又拉起一个新的。

### 第二个症状更阴

```
[RTSP2] sender thread running
[RTSP2] bind 554 failed: Address already in use
```

307 启动时 285 还占着 554，**bind 失败但程序不退出**，继续跑成一个"没有 RTSP、也抢不到帧"的**残废进程**。

> 这类进程最坑：`ps` 里看得见、像活着、实际什么都不干。
> 判据：`netstat -ln | grep 554` 空 + `ps` 有进程 = 残废。

清干净后重启，554 立刻 LISTEN，日志 4 秒 0 字节增长（不再刷错误），VENC `Sequence` 正常增长。

---

## 05 bug 一：`/etc/init.d/` 里的 `.bak` 会被执行

### 现象

改完脚本，第一次重启后**板子彻底不通**。串口看：

```
inet addr:192.168.1.20  Bcast:192.168.1.255  Mask:255.255.255.0
```

`Bcast` 是 `.1.255` → 掩码 `/24`。但文件里明明是 `/16`：

```sh
~ # grep -n 'ifconfig eth0' /etc/init.d/S80network
30:    ifconfig eth0 192.168.1.20 netmask 255.255.0.0 up
```

### 根因

```sh
~ # ls /etc/init.d/
S80network  S80network.bak_gw  S99rtsp  S99rtsp.bak2  S99rtsp.bak_gw
```

`rcS` 的重启逻辑是**通配符遍历**：

```sh
for initscript in /etc/init.d/S[0-9][0-9]* ; do
    if [ -x $initscript ] ; then $initscript ; fi
done
```

于是开机顺序变成：
1. `S80network` 配 `/16` ✓
2. `S80network.bak_gw` 执行 → **改回 `/24`** ✗
3. `S99rtsp.bak2` / `.bak_gw` 各自跑一遍 start，互相搅乱 `/mnt` 和锁

### 教训

> **`/etc/init.d/` 里不能放任何 `S` 开头的非脚本文件**，包括备份。
> 要备份就放 `/root/`、`/tmp`，或直接改名去掉 `S` 前缀。

这条坑的隐蔽性在于：文件内容完全正确，`sh -n` 也过，只是**被多执行了一遍**。

---

## 06 bug 二：锁落在 jffs2，服务永远起不来

### 现象

重启后：掩码对了、`/mnt` 空的、`ps` 里没有 `sample_venc`、554 不监听。手动敲一次：

```sh
~ # /etc/init.d/S99rtsp
already running (watcher 234)
```

**234 根本不存在**，但它让脚本直接退出。

### 取证

先看两件事：`/mnt` 到底是不是 tmpfs，锁在哪。

```sh
~ # mount | grep mnt        →  （空，不是 tmpfs）
~ # ls -la /mnt/
drwxr-xr-x  2 root root  rtsp.lock        ← 锁在 jffs2 上！
-rw-r--r--  1 root root  4  watcher.pid   → 234
```

再卸掉 tmpfs 复现：

```sh
~ # umount /mnt; ls -a /mnt/
.   ..   rtsp.lock       ← tmpfs 消失后，flash 上的锁露出来了
```

### 根因：脚本执行顺序错了

旧脚本：

```sh
if mkdir $LOCK 2>/dev/null ; then     # ← 此时 /mnt 还是 jffs2，锁写进 flash
    start_daemon &                    # ← 这里面才 mount tmpfs，把锁"盖住"
    echo $! > $LOCK/watcher.pid
fi
```

`sh -x` 跟踪得清清楚楚：

```
+ mkdir /mnt/rtsp.lock
+ mount -t tmpfs -o size=48m none /mnt
```

后果：**锁在 flash 上活过一次重启**，而它指向的 pid 早死了。下次开机 `mkdir` 失败 → 打印 `already running` → 退出 → 永远不启动。

### 修法

1. `setup_mnt()` 提到**所有锁操作之前**，锁和日志都落在 tmpfs → 掉电即清，天然无残留
2. 锁不能只看目录在不在，要**验 pid 还活着、且确实是本脚本**：

```sh
watcher_alive() {
    [ -f "$LOCK/watcher.pid" ] || return 1
    p=$(cat "$LOCK/watcher.pid" 2>/dev/null)
    [ -n "$p" ] || return 1
    kill -0 "$p" 2>/dev/null || return 1                     # 进程还在？
    grep -q "S99rtsp" "/proc/$p/cmdline" 2>/dev/null || return 1  # 是它吗？
    return 0
}
```

第二步的 `/proc/$p/cmdline` 校验不能省：重启后 pid 会**被复用**，只看 `kill -0` 会误判成"已在运行"。

---

## 07 bug 三：stop 杀不掉老 watcher

### 现象

`stop` 之后马上 `ps` 是空的，但 3 秒后进程又冒出来——正好落在 watcher 的 `sleep 3` 重启窗口里。

### 修法

```sh
clean_all() {
    [ -f "$LOCK/watcher.pid" ] && kill -9 "$(cat "$LOCK/watcher.pid")"   # 先杀 watcher 本体
    for p in $(pgrep -f "init.d/S99rtsp") ; do                          # 再扫同名实例
        [ "$p" = "$$" ] && continue
        kill -9 "$p"
    done
    killall -9 -q sample_venc ; sleep 1 ; killall -9 -q sample_venc     # 双重 kill，防漏
    rm -rf "$LOCK"
}
```

**顺序很重要**：先杀 watcher 再杀子进程。反过来的话，watcher 会认为"子进程崩了"，立刻重启一个新的。

### 验收方法（关键：要等过重启窗口）

```sh
/etc/init.d/S99rtsp stop; sleep 6; ps | grep sample_venc | grep -v grep
```

必须**完全空**。只等 1~2 秒是查不出问题的。

---

## 08 另外两处加固

### 8.1 网关探测：开机第一次 ping 一定失败

开机时网口刚 up，交换机还没完成 ARP 学习。单次 `ping -c 1 -W 1` 会失败，于是网关没加上 —— 重启验收里就出现过"路由缺 default"。

```sh
gw_ok=0
try=1
while [ $try -le 5 ]; do
    for gw in 192.168.3.1 192.168.1.1 ; do
        if ping -c 1 -W 1 $gw >/dev/null 2>&1 ; then
            route add default gw $gw 2>/dev/null && gw_ok=$gw
            break
        fi
    done
    [ "$gw_ok" != "0" ] && break
    try=$((try+1)) ; sleep 2
done
[ "$gw_ok" = "0" ] && route add default gw 192.168.3.1 2>/dev/null   # 全失败也硬加
```

日志里能看到它第 4 秒才成功：

```
11-06 10:51:27 daemon: loop begin
11-06 10:51:31 route: gw 192.168.3.1 ok
```

### 8.2 连续失败不再退出，只退避

旧逻辑 `fail >= 5 → return`，无人值守时等于**永久放弃**。改成退避重试：

```sh
if [ $fail -ge 5 ] ; then
    blog "daemon: $fail failures, backoff 30s"
    fail=0 ; sleep 30
else
    sleep 3
fi
```

### 8.3 网关和 NTP 挪到后台

```sh
( setup_route ; ntpd ... ; hwclock -w -u ) &
```

探测最坏要 20 秒，不能让 RTSP 陪着等。**先出流，再补网关和时间**。

### 8.4 加一个开机现场日志

`/var/log/rtsp.boot.log`，8KB 上限，只记关键节点：

```
10:51:26 mnt: tmpfs ok
10:51:27 boot start watcher 179
10:51:27 daemon: loop begin
10:51:27 daemon: exec
10:51:31 route: gw 192.168.3.1 ok
10:51:54 ntp: ok, rtc synced(utc)
```

> 上次排查卡了半小时，就是因为**开机那一段没有任何留痕**。
> 这个文件以后就是"开机为什么没起来"的第一现场。

---

## 09 最终脚本骨架

```sh
setup_mnt()      # 1. 先挂 tmpfs（锁/日志的地基）
watcher_alive()  # 2. 验 pid + 验身份，别信目录存在
clean_all()      # 3. 先杀 watcher 再杀子进程，双重 killall
start_daemon()   # 4. 等 eth0 → 后台补网关/NTP → 前台守护循环
case "$1" in
  stop)    clean_all ;;
  restart) clean_all; setup_mnt; ... ;;
  *)       setup_mnt; watcher_alive && exit 0; clean_all; ... ;;   # rcS 走这里（无参数）
esac
```

注意 `rcS` 调用是 `$initscript`（**不带 start**），所以 `*)` 分支必须能当 start 用。

---

## 10 重启验收（这才是唯一标准）

```sh
~ # reboot
```

55 秒后自动重连，逐项打勾：

| 项 | 实测 |
|---|---|
| 进程 | `watcher 179` + `sample_venc 203` 各一个 |
| 路由 | `default via 192.168.3.1 dev eth0` 自动补上 |
| 掩码 | `Mask:255.255.0.0`，`Bcast:192.168.255.255` |
| 时间 | `02:52 UTC` = **10:52 CST**，正确 |
| 端口 | `0.0.0.0:554 LISTEN` |
| 媒体 | RTP 1573 包 / 2.12MB / **4.24 Mbps** |
| 编码 | `VENC Sequence 1948` 持续增长，`Full=0` |
| 日志 | `/mnt/sample_venc.log` 空，零报错 |

`uptime` 1 分钟、Mem 空闲 90MB —— 干净的冷启动。

---

## 11 方法论：这次真正学到的东西

### 11.1 排障顺序：从下往上，一层验完再上一层

```
二层(ARP) → 三层(IP/路由) → 信令(RTSP) → 媒体(RTP) → 应用(进程/编码器)
```

每层都有**独立判据**，不要跳。这次如果一上来就看 VLC，会被"网络不通"和"进程打架"两个问题同时困住。

### 11.2 三条通用教训

1. **"看起来成功了"是最危险的**：`stop` 报 stopped 但进程 3 秒后复活；`bind 554` 失败但程序不退出；`mkdir` 成功但锁写错了介质。**每个"成功"都要找一个可观测证据去对**。
2. **备份也要讲位置**：备份文件名进了 `S??*` 通配符，就等于多了一个启动脚本。**备份不放在会被扫描执行的目录里**。
3. **状态目录必须和它的生命周期匹配**：锁是"本次开机有效"，就必须放 tmpfs；放 flash 就等于"上次开机的锁管到这次"。

### 11.3 我在这次里判断错的三处（记下来免得再犯）

| 错误判断 | 真相 | 教训 |
|---|---|---|
| 说 `LOG`/`WATCH` 指向同一文件 | 看低分辨率截图看错了，是两个文件 | **截图看不清就去 `grep`，别靠眼睛** |
| 猜 watcher 卡在 `grep "inet addr"` | 输出带 `addr:`，能匹配 | 猜测要跑一条命令验证 |
| 按 `/24` 口径讲"5.x 和 1.x 两个网段" | PC 是 `/16`，同网段 | **先算掩码再下结论** |

### 11.4 一句话

> 嵌入式设备"重启后不对劲"，九成不是驱动问题，是**启动脚本的环境假设**出了问题：
> 假设 `/mnt` 已挂好、假设网络已就绪、假设 pid 不会复用、假设备份文件不会被执行。
> 一条一条把这些假设变成**显式的检查和重试**，设备才真正"能自己活"。

---

## 附：本次涉及的文件

| 文件 | 改动 |
|---|---|
| `/etc/init.d/S80network` | 第 30 行掩码 `/24` → `/16` |
| `/etc/init.d/S99rtsp` | 整份重写（166 行）：setup_mnt 前置、pid 验活、双重清扫、网关 5 轮探测、`hwclock -w -u`、失败退避、bootlog |
| `/etc/init.d/*.bak*` | **全部删除**（会被 rcS 执行） |
| `/var/log/rtsp.boot.log` | 新增，8KB 上限，开机阶段留痕 |

**不进 jffs2 的**：`/mnt` 下的 `sample_venc.log`、`sample_venc_watch.log`、`rtsp.lock` —— 全在 tmpfs，掉电即清。
