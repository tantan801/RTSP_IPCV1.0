from board import Board
import time, sys
sys.path.insert(0, '.')
from board import Board

b = Board()

# 1. 确认模型还在(tmpfs)
out = b.raw("ls -l /mnt/nnie/inst_ssd_cycle.wk; echo ---MODEL", 15)
print(out)
if "inst_ssd_cycle.wk" not in out:
    print("!! MODEL MISSING - need re-tftp")
    sys.exit(1)

# 2. 停服务
print(b.raw("/etc/init.d/S99rtsp stop", 30))
time.sleep(3)

# 3. 下载新二进制(tmpfs, 快)
out = b.raw("rm -f /mnt/sample_venc_new; cd /mnt && tftp -g -r sample_venc -l /mnt/sample_venc_new 192.168.142.25 2>&1 | tail -c 120", 200)
print(out)

# 4. 校验
out = b.raw("md5sum /mnt/sample_venc_new", 30)
print(out)
if "e1d0f7d62ff64070028cd18612ed04ae" not in out:
    print("!! MD5 MISMATCH")
    sys.exit(1)

# 5. 替换(jffs2 写 3.9MB, 长等待分两步)
b.raw("rm -f /usr/bin/sample_venc", 30)
out = b.raw("cp /mnt/sample_venc_new /usr/bin/sample_venc; echo CP_RC=$?", 240)
print(out)
out = b.raw("md5sum /usr/bin/sample_venc; chmod 755 /usr/bin/sample_venc", 60)
print(out)

# 6. 启动
print(b.raw("/etc/init.d/S99rtsp start", 30))
time.sleep(20)

# 7. 检查关键日志
out = b.raw("grep -a 'DET\\|DRAW\\|OSD' /mnt/sample_venc.log | head -15; echo ---LOG", 20)
print(out)
out = b.raw("ps | grep sample_venc | grep -v grep; netstat -ln | grep 554; echo ---PS", 15)
print(out)
