#!/usr/bin/env python3
# Drives vmdos (voodoo3) in QEMU through its monitor socket: types each Glide
# test's command at the DOS prompt, checks pixels in screenshots while it
# draws, quits it with Esc and waits for "DPMI: client exits (0)" in the
# serial log. A test that never exits is a hang (the run stops there).
#   run.py outdir      (glidetest.sh starts QEMU with outdir/qmon, outdir/serial.log)
# TESTS="TEST00 W00 ..." runs only those.
import os, re, socket, sys, time

out = sys.argv[1]
mon = out + '/qmon'

BLUE, GREEN, NONBLACK = (0, 0, 255), (0, 255, 0), 'nonblack'
# name: (directory, command, [(x, y, want)] at 640x480, self-exiting)
TESTS = [('TEST%02d' % i, '', 'test%02d -n 100000' % i, [], False) for i in range(30)]
CHECKS = {
    'TEST00': [(320, 240, BLUE)],
    'TEST04': [(350, 180, NONBLACK)],
    'TEST05': [(500, 240, GREEN)],
    'TEST17': [(320, 240, NONBLACK)],
    'TEST25': [(320, 240, NONBLACK)],
    'W00': [(320, 240, BLUE)],
    'W04': [(350, 180, NONBLACK)],
    'W05': [(500, 240, GREEN)],
    'W17': [(320, 240, NONBLACK)],
    'W25': [(320, 240, NONBLACK)],
}
TESTS += [('W%s' % i, 'w', 'w%s -n 100000' % i, [], False) for i in ('00', '04', '05', '11', '17', '20', '25', '26')]
TESTS += [('WINIT', 'w', 'winit', [], True), ('AINIT', 'w', 'dos32a ainit', [], True)]
TESTS = [(n, d, c, CHECKS.get(n, []), s) for n, d, c, _, s in TESTS]
only = os.environ.get('TESTS', '').upper().split()
if only:
    TESTS = [t for t in TESTS if t[0] in only]

def cmd(c):
    s = socket.socket(socket.AF_UNIX); s.connect(mon); s.recv(4096)
    s.send((c + '\n').encode()); time.sleep(0.2); s.close()

def log():
    try: return open(out + '/serial.log', errors='replace').read()
    except OSError: return ''

KEYS = {'\n': 'ret', ' ': 'spc', '-': 'minus', '.': 'dot', '\\': 'backslash'}
def typ(s):
    for ch in s:
        cmd('sendkey ' + KEYS.get(ch, ch)); time.sleep(0.05)

def count(pat): return len(re.findall(pat, log()))

def wait_count(pat, n, t):
    end = time.time() + t
    while count(pat) <= n:
        if time.time() > end: return False
        time.sleep(0.5)
    return True

def shot(name):
    f = '%s/%s.ppm' % (out, name)
    cmd('screendump ' + f); time.sleep(0.8)
    return f

def px(f, x, y):
    d = open(f, 'rb').read(); hdr = d.split(b'\n', 3)
    w, h = map(int, hdr[1].split()); o = len(b'\n'.join(hdr[:3])) + 1
    x, y = x * w // 640, y * h // 480
    return tuple(d[o + (y * w + x) * 3: o + (y * w + x) * 3 + 3])

def ok(got, want):
    if want == NONBLACK: return max(got) > 40
    return all(abs(a - b) < 40 for a, b in zip(got, want))

EXIT = r'DPMI: client exits \((\d+)\)'
ENTER = r'DPMI: 32-bit client entered'

end = time.time() + 600
while 'hd: VMHD drives' not in log():
    if time.time() > end: sys.exit('vmdos never booted')
    time.sleep(0.5)
time.sleep(8)                                  # the prompt
results, cwd = [], ''
for name, d, command, checks, selfexit in TESTS:
    if d != cwd:
        typ('cd \\' + d + '\n'); time.sleep(2); cwd = d
    n_in, n_out = count(ENTER), count(EXIT)
    typ(command + '\n')
    if not wait_count(ENTER, n_in, 90):
        results.append((name, 'FAIL', 'never started')); print(name, 'FAIL never started', flush=True); continue
    why = ''
    if checks:
        end, good, f = time.time() + 90, False, None
        while time.time() < end and not good:
            time.sleep(1.5)
            f = shot(name.lower())
            try: good = all(ok(px(f, x, y), w) for x, y, w in checks)
            except (OSError, ValueError, IndexError): pass
        if not good:
            why = 'pixels ' + ' '.join('(%d,%d)=%s' % (x, y, px(f, x, y)) for x, y, _ in checks)
    elif not selfexit:
        time.sleep(8); shot(name.lower())
    if not selfexit:
        cmd('sendkey esc')
        if not wait_count(EXIT, n_out, 15):
            cmd('sendkey esc')
    if not wait_count(EXIT, n_out, 120):
        results.append((name, 'HANG', 'no exit (the machine is stuck: stopping)'))
        print(name, 'HANG', flush=True)
        break
    code = re.findall(EXIT, log())[-1]
    if code != '0': why = (why + ' exit code ' + code).strip()
    results.append((name, 'FAIL' if why else 'ok', why))
    print(name, 'FAIL ' + why if why else 'ok', flush=True)
    time.sleep(3)                              # back at the prompt

bad = [r for r in results if r[1] != 'ok']
print('%d of %d ok%s' % (len(results) - len(bad), len(TESTS), '' if len(results) == len(TESTS) else ' (stopped early)'))
for r in bad: print('  %s: %s %s' % r)
sys.exit(1 if bad or len(results) != len(TESTS) else 0)
