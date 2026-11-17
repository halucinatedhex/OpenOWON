import serial, sys, time, re
port = serial.Serial('/dev/cu.usbserial-1110', 115200, timeout=0.2)
def cmd(line, until, tmax):
    port.reset_input_buffer()
    port.write((line + '\n').encode())
    t = time.time(); buf = ''
    while time.time() - t < tmax:
        buf += port.read(4096).decode(errors='replace')
        if re.search(until, buf): break
    return buf
args = sys.argv[1:]
if args and args[0] == 'idn':
    print(cmd('*IDN?', r'\n', 3)); sys.exit()
ranges = [int(a) for a in args] or [0, 1, 2, 3, 4]
for r in ranges:
    out = cmd(f'DIAG:CAPX {r} 24=01', r'(mean .*\n)|no counter result|capx:.*range', 90)
    for l in out.splitlines():
        if l.startswith('cap') and 'R24' not in l and 'configured' not in l: print(l)
