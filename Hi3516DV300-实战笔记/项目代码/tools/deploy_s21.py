import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paramiko
from board import Board

VM = '192.168.142.25'
D = '/home/lmo/hisi/Hi3516CV500_SDK_V2.0.2.0/smp/a7_linux/mpp/sample/venc'

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(VM, username='lmo', password='123456', timeout=10)


def vm(cmd, t=120):
    i, o, e = c.exec_command(cmd, timeout=t)
    return o.read().decode(errors='replace') + e.read().decode(errors='replace')


print('=== VM: 放 tftp')
print(vm('cp -f %s/sample_venc /srv/tftp/sample_venc && chmod 644 /srv/tftp/sample_venc && md5sum /srv/tftp/sample_venc' % D))
md5_out = vm('md5sum %s/sample_venc' % D)
vmmd5 = md5_out.split()[0]
print('VM md5 =', vmmd5)

b = Board()
print('=== 板: stop')
print(b.raw('/etc/init.d/S99rtsp stop; sleep 1; echo STOPPED', 30))

print('=== 板: tftp 下载')
print(b.raw('rm -f /mnt/sample_venc_new; cd /mnt && tftp -g -r sample_venc -l /mnt/sample_venc_new 192.168.142.25; echo TFTP_DONE', 150))
dl = b.raw('md5sum /mnt/sample_venc_new', 30)
print(dl)
if vmmd5 not in dl:
    print('!!! MD5 不一致，中止')
    sys.exit(1)

print('=== 板: 替换')
print(b.raw('rm -f /usr/bin/sample_venc; echo RM_DONE', 30))
print(b.raw('cp /mnt/sample_venc_new /usr/bin/sample_venc; echo CP_DONE', 260))
print(b.raw('chmod 755 /usr/bin/sample_venc; md5sum /usr/bin/sample_venc', 40))

print('=== 板: start')
print(b.raw('/etc/init.d/S99rtsp start', 50))
print(b.raw('sleep 12; ps | grep sample_venc | grep -v grep; echo ---PS; netstat -ln | grep 554; echo ---PORT', 50))
print('=== [VB] / [DET] 日志')
print(b.raw('grep -a "\\[VB\\]\\|\\[DET\\]" /mnt/sample_venc.log | head -20; echo ---GREPLOG', 40))
print('=== 日志开头')
print(b.raw('head -16 /mnt/sample_venc.log', 30))
b.close()
