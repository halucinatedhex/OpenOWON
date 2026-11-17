import serial, sys, time, re
port = serial.Serial('/dev/cu.usbserial-1110', 115200, timeout=0.2)
def cmd(line, until, tmax):
    port.reset_input_buffer(); port.write((line + '\n').encode())
    t = time.time(); buf = ''
    while time.time() - t < tmax:
        buf += port.read(4096).decode(errors='replace')
        if re.search(until, buf): break
    return buf
for rb in sys.argv[1].split(','):
    print(cmd(f'DIAG:CAPRB {rb}', r'caprb.*\n', 3).strip())
    for r in sys.argv[2:]:
        out = cmd(f'DIAG:CAPX {r} 24=01', r'(mean .*\n)|no counter result', 90)
        ms = [int(m) for m in re.findall(r': (\d+)ms CTB', out)]
        ctb = re.findall(r'CTB (\d+)', out)
        mean = re.search(r'mean (\S+) F', out)
        print(f"  rb={rb} range {r}: gates {ms} CTB {ctb} mean {mean[1] if mean else '-'}")
