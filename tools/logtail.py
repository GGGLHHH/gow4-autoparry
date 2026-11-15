"""打印 gow4_probe.log 自上次调用以来新增的行（偏移记在 tools/.logtail_pos）。--reset 从头开始。"""
import os, sys
log = os.path.join(os.environ['TEMP'], 'gow4_probe.log')
pos_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.logtail_pos')
pos = 0 if '--reset' in sys.argv or not os.path.exists(pos_file) else int(open(pos_file).read() or 0)
size = os.path.getsize(log)
if size < pos: pos = 0
with open(log, 'rb') as f:
    f.seek(pos); data = f.read()
cut = data.rfind(b'\n') + 1
sys.stdout.reconfigure(encoding='utf-8')
print(data[:cut].decode('utf-8', 'replace'), end='')
open(pos_file, 'w').write(str(pos + cut))
