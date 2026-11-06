#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""S2-2 一键部署: VM编译产物+模型 -> tftp -> 板替换 -> 启动 -> 抓日志"""
import sys, time
sys.path.insert(0, r'C:\Users\33234\Desktop\work\11.6')
from board import Board

VM_TFTP = '192.168.142.25'
b = Board()

# 1. VM 侧: 拷贝产物到 tftp
import paramiko
c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(VM_TFTP, username='lmo', password='123456', timeout=10)
def vm(cmd, t=120):
    i, o, e = c.exec_command(cmd, timeout=t)
    return o.read().decode(errors='replace') + e.read().decode(errors='replace')
D = '/home/lmo/hisi/Hi3516CV500_SDK_V2.0.2.0/smp/a7_linux/mpp/sample/venc'
M = '/home/lmo/hisi/Hi3516CV500_SDK_V2.0.2.0/smp/a7_linux/mpp/sample/svp/nnie/data/nnie_model/detection/inst_ssd_cycle.wk'
print(vm('cp -f %s/sample_venc /srv/tftp/sample_venc && cp -f %s /srv/tftp/inst_ssd_cycle.wk && md5sum /srv/tftp/sample_venc /srv/tftp/inst_ssd_cycle.wk' % (D, M)))
c.close()

# 2. 板侧: 停服 -> 下载 -> 校验 -> 替换 -> 放模型 -> 启动
print(b.raw('/etc/init.d/S99rtsp stop', 30))
print(b.raw('rm -f /mnt/sample_venc_new /mnt/inst_ssd_cycle.wk', 15))
print(b.raw('tftp -g -r sample_venc -l /mnt/sample_venc_new ' + VM_TFTP, 180))
print(b.raw('tftp -g -r inst_ssd_cycle.wk -l /mnt/inst_ssd_cycle.wk ' + VM_TFTP, 300))
out = b.raw('md5sum /mnt/sample_venc_new /mnt/inst_ssd_cycle.wk; ls -l /mnt/sample_venc_new /mnt/inst_ssd_cycle.wk', 30)
print(out)
print(b.raw('cp /mnt/sample_venc_new /usr/bin/sample_venc && chmod 755 /usr/bin/sample_venc && md5sum /usr/bin/sample_venc', 240))
print(b.raw('mkdir -p /mnt/nnie && cp /mnt/inst_ssd_cycle.wk /mnt/nnie/ && md5sum /mnt/nnie/inst_ssd_cycle.wk', 240))
print(b.raw('df -h /mnt | tail -1', 15))

# 3. 启动并观察
print(b.raw('/etc/init.d/S99rtsp start', 30))
time.sleep(20)
print(b.raw("ps | grep sample_venc | grep -v grep; echo ---PS", 15))
print(b.raw("netstat -ln | grep 554; echo ---PORT", 15))
print(b.raw("grep -a 'DET' /mnt/sample_venc.log | head -25; echo ---DET", 20))
print(b.raw("grep -a 'VB\|ERROR\|failed' /mnt/sample_venc.log | head -10; echo ---ERR", 20))
