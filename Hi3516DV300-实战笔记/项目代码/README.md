# Hi3516DV300 网络摄像头 + NNIE 人形检测：完整项目代码

> 归档日期：2026-11-06　|　对应笔记：本目录上级的 01–13 篇实战笔记
> 环境：Ubuntu VM（192.168.142.25，tftp 服务器）→ 板子 192.168.1.20（jffs2 + tmpfs /mnt）
>
> **第一次使用：先读 [操作手册](操作手册.md)** —— 首次部署、日常启停、VLC 看画面、编译更新、故障对照，全流程 step by step。

## 目录结构

```
项目代码/
├── README.md             本文件（改动清单 + md5）
├── 操作手册.md            从零部署到出画面的完整操作流程 ← 先读这个
├── board/            板端启动脚本（放 /etc/init.d/）
│   ├── S99rtsp           RTSP 守护脚本（v2 最新版，含 TERM 优雅退出 + rc=255 reboot 自愈）
│   └── S80network        网络脚本（掩码 /16 固化）
├── venc/             sample/venc 源码（放 VM SDK 的 sample/venc/）
│   ├── venc_sample_venc.c    主程序：VB 三池、VPSS 三通道、DET 推理线程、退出码 exit(0)
│   ├── venc_nnie_detect.c    NNIE 检测模块：取帧 mmap / NV21→BGR / SSD 推理 / ARGB1555 红框
│   ├── venc_nnie_detect.h    检测模块头文件
│   ├── venc_osd_test.c       水印 OSD（关键改动：第 324 行 u32Layer 0→2，解决画框盖水印）
│   └── venc_Makefile         编译规则（TARGET 固定列表 + SVP 库链接）
├── common/
│   └── common_sample_comm.h  sample/common 头文件（RTSP_SERVER_ENABLE 改 #ifndef 守卫）
├── svp/
│   └── svp_nnie_Makefile     sample/svp/nnie/Makefile（末尾追加 -DRTSP_SERVER_ENABLE=0）
└── tools/            调试/部署工具（PC 端 Python，依赖 paramiko + telnetlib）
    ├── board.py              telnet 连板工具（Board 类，raw() 命令执行）
    ├── rtsp_probe.py         RTSP 端口/流探测
    ├── venc_fps.py           读 /proc/umap/venc 统计 6s 帧增量
    ├── png_check.py          纯 stdlib PNG 解码器（像素级验证水印区域）
    ├── deploy_s21.py         S2-1 取帧一键部署（补丁→编译→tftp→板端替换）
    ├── deploy_s22.py         S2-2 推理一键部署
    ├── deploy_s24.py         S2-4 红框上屏一键部署
    └── diag_vbexit.py        VB 池残留取证（3 轮 stop/start 实验）
```

## 关键改动清单（相对 SDK 原版）

| 文件 | 改动 | 对应笔记 |
|---|---|---|
| `venc_sample_venc.c` | VB 池加第三池；VPSS chn2 300×300；DET 线程（GetChnFrame→Mmap→NNIE→Munmap）；3 处画框钩子；HandleSig `exit(-1)→exit(0)` | 第 12 篇 |
| `venc_nnie_detect.c` | 全新模块：320×320 取帧（**必须 HI_MPI_SYS_Mmap**，VB_REMAP_MODE_NONE 下 virAddr 是内核地址）、NV21→BGR planar（BT.601 limited 最近邻）、SSD Forward+Ssd_GetResult（class15 person，阈值 0.5）、1920×1080 ARGB1555 overlay 画红框（layer=1，每 6 帧推理节流）、模型缺失自动降级 luma-only | 第 12 篇 |
| `venc_osd_test.c` | 第 324 行 `u32Layer 0→2`（本平台 layer 大者在上、透明像素替换下层 → 水印层必须高于画框层） | 第 12 篇 |
| `venc_Makefile` | TARGET 固定列表（wildcard 会把无 main 的 .c 当 target）；SVP 源附加；libive/libmd/libnnie.a | 第 12 篇 |
| `common_sample_comm.h` | `RTSP_SERVER_ENABLE` 加 `#ifndef` 守卫（否则 svp/nnie 链接 undefined） | 第 11 篇 |
| `svp_nnie_Makefile` | 末尾追加 `-DRTSP_SERVER_ENABLE=0` | 第 11 篇 |
| `board/S99rtsp` | 锁/日志前置到 tmpfs；网关 5 轮探测；stop：pgrep 全量清扫 + pid 验活 + /proc 验身份 + **TERM 优雅退出等 10s 再 -9 兜底**；连续 10 次 rc=255 → reboot 自愈；hwclock -w -u；ntpd 成功才写 RTC | 第 11/12 篇 |
| `board/S80network` | 掩码 /16（公司网关 192.168.3.1 在 /16 下才可达） | 第 11 篇 |

## 部署要点

1. **编译**（VM）：`export PATH=/opt/hisi-linux/x86-arm/arm-himix200-linux/bin:$PATH && cd <sdk>/sample/venc && make`（必须 cd 进目录，$(PWD) 不随 make -C 变）
2. **改 common/*.c 后**必须 `rm ../common/*.o` 重编，md5 验证
3. **传输**：VM→板 `tftp -g -r <文件> -l /mnt/<文件> 192.168.142.25`（大文件 telnet 命令超时要给 400s；/usr/bin 覆盖后记得 `chmod +x`，tftp 下载件是 644）
4. **模型**：`inst_ssd_cycle.wk` 放板端 `/mnt/nnie/`（tmpfs，重启丢失需重新 tftp——持久化待办）
5. **运行时**：GC2053 出图 = `sample_comm_vi.c` lane_id {0,2,-1,-1} + LANE_DIVIDE_MODE_1（这两处在板子 SDK，不在本归档内，见第 4 篇笔记）
6. **S00devs**：`-sensor0 gc2053 -osmem 128 -total 1024`（板端已固化）

## 文件 md5（对照用）

```
board/S80network               61a97166cd9e80a8289939e11aad85b3
board/S99rtsp                  0ff00a9c48b0f4d7858f93c7a9fb8834
common/common_sample_comm.h    89ff6187d2ed71d29645446b5dc6776a
svp/svp_nnie_Makefile          7f94bdbca1878ed28e8003d05fe3e894
venc/venc_Makefile             6c79adbac00ab40ee0ff1fc3a58c560d
venc/venc_nnie_detect.c        757ae95b496aaaa2a2d6bb6408185bbf
venc/venc_nnie_detect.h        97fa892e7016221e706fde86e6d650f0
venc/venc_osd_test.c           96d411594ed19f2cbee85d0a80073ffb
venc/venc_sample_venc.c        337ac914a3863c0df2364e636f464b6b
tools/board.py                 da4952925a4c72809b85bcc2e45381e5
tools/deploy_s21.py            f99f779d7864363ae32c80775ccc3301
tools/deploy_s22.py            7cc570689cc0bcf255846bc6d6166ed4
tools/deploy_s24.py            328522c186f46aee11825387bdc2a6fc
tools/diag_vbexit.py           6cac41d2c49479f7feaa941b7864db5e
tools/png_check.py             d43fdbaacead6e5fdaeb6c8abe70cf98
tools/rtsp_probe.py            6b728b4dd6c717f1604e09ad13ff2181
tools/venc_fps.py              c1c244e3886f2807b1be7dcb7562bf83
```

> 当前运行版 sample_venc 二进制 md5：8924dc97…（2026-11-06 15:10 编译，含 stop→start 修复）
