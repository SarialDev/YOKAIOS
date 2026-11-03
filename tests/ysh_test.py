#!/usr/bin/env python3
"""Testul interactiv pentru ysh (make test-ysh).

  ysh_test.py run <socket-monitor> <log-serial>   tasteaza STEPS in ysh prin `sendkey` (monitorul QEMU) si asteapta,
                                                  dupa fiecare pas, linia de iesire asteptata in log
  ysh_test.py check <log>                         verifica logul final: liniile CHECKS (egalitate exacta, de cate ori), liniile
                                                  FORBIDDEN lipsesc, apare prompt-ul cu cwd-ul schimbat

In STEPS, {nume} = tasta speciala (ret, tab, up, down, left, right, home, end, del, bs, ctrl-c).
Liniile de iesire se compara pe linie intreaga (fara \\r): textul tastat apare mereu dupa prompt, deci nu se confunda cu iesirea.
"""
import re
import socket
import sys
import time

# (taste, linie asteptata, de cate ori trebuie sa apara in tot logul pana acum)
# "re:..." = expresie regulata pe linie
STEPS = [
    ("V=ok; echo s1-$V{ret}", "s1-ok", 1),                                   # variabila shell, ; si $VAR
    ("ls /bin | cat{ret}", "ysh", 1),                                        # acceptanta: pipe
    ("echo x > /tmp/f; cat /tmp/f{ret}", "x", 1),                            # acceptanta: redirectare + ;
    ("echo got-$V > /tmp/g; echo more >> /tmp/g; cat /tmp/g{ret}", "more", 1),
    ("cat < /tmp/g | cat | cat{ret}", "more", 2),                            # < si pipe cu 3 etape
    ("export Q=exp; env | cat{ret}", "Q=exp", 1),                            # export ajunge in mediul copiilor
    ("echo bg-$V &{ret}", "bg-ok", 1),                                       # fundal
    ("cd /tmp; pwd{ret}", "/tmp", 1),
    ("cd; pwd{ret}", "/root", 1),
    ("echo one-$V{ret}", "one-ok", 1),
    ("echo two-$V{ret}", "two-ok", 1),
    ("{up}{up}{ret}", "one-ok", 2),                                          # istoric: de doua ori sus
    ("echo ac{left}b{ret}", "abc", 1),                                       # sageata stanga + inserare
    ("echo xyz{bs}{bs}q{ret}", "xq", 1),                                     # backspace
    ("echo abcd{home}{right}{right}{right}{right}{right}{del}{end}e{ret}", "bcde", 1),   # Home, Delete, End
    ("echo tab-$V /etc/os-rel{tab}{ret}", "tab-ok /etc/os-release", 1),      # Tab: cale
    ("upti{tab}{ret}", "re:^up [0-9]+(\\.[0-9]+)? s$", 1),                   # Tab: comanda
    ("cat /etc/{tab}mo{tab}{ret}", "Bine ai venit pe YokaiOS.", 1),          # Tab: lista, apoi prefix unic
    ("echo 'a  b' \"c $V\" d\\ e{ret}", "a  b c ok d e", 1),                 # ghilimele si backslash
    ("ls /nonexistent; echo st-$?{ret}", "st-1", 1),                         # $? dupa eroare
    ("nosuchcmd{ret}", "ysh: nosuchcmd: command not found", 1),
    ("echo st-$?{ret}", "st-127", 1),
    ("ls /bin/nope && echo no-and || echo yes-or{ret}", "yes-or", 1),       # && si ||
    ("cd /nonexistent{ret}", "ysh: cd: /nonexistent: No such file or directory", 1),
    ("echo nope{ctrl-c}echo cc-ok{ret}", "cc-ok", 1),                        # Ctrl-C renunta la linie
    ("spin{ret}", "spin: pornit", 1),                                       # program in bucla in prim-plan...
    ("{ctrl-c}", "^C", 1),                                                   # ...Ctrl-C = SIGINT pentru grupul lui
    ("echo st-$?{ret}", "st-130", 1),                                        # 128 + SIGINT, shell-ul a supravietuit
    ("lua -e \"print(6*7)\"{ret}", "42", 1),                                  # Lua: -e
    ("lua{ret}", "re:^Lua 5\\.4\\.[0-9]+  Copyright", 1),                    # Lua: REPL interactiv
    ("print(2^10){ret}", "1024.0", 1),
    ("print(string.format('%.3f', math.pi)){ret}", "3.142", 1),
    ("s = 0 for i = 1, 100 do s = s + i end print(s, 7 // 2){ret}", "re:^5050\\s+3$", 1),
    ("x = 0 while true do x = x + 1 end{ret}", None, 0),                     # bucla in Lua...
    ("{ctrl-c}", "interrupted!", 1),                                    # ...SIGINT -> hook -> eroare, REPL ramane
    ("print('alive-lua'){ret}", "alive-lua", 1),
    ("os.exit(7){ret}", None, 0),
    ("echo lua-st-$?{ret}", "lua-st-7", 1),
    ("ypkg install /usr/share/ypkg/hello.yokai{ret}", "instalat hello 1.0.0 (2 fisiere)", 1),   # ypkg pe radacina read-only: /var, /opt = tmpfs
    ("ypkg run hello pachet{ret}", "  arg 1 = \"pachet\"", 1),                 # pachetul instalat ruleaza
    ("/opt/hello/bin/hello{ret}", "salut de la hello 1.0.0 (pachet .yokai), argc=1", 1),
    ("ypkg list | grep hello{ret}", "re:^hello\\s+1\\.0\\.0\\s", 1),
    ("ypkg remove hello; ls /opt{ret}", "scos hello (2 fisiere)", 1),
    ("echo sync-last{ret}", "sync-last", 1),
    ("exit{ret}", None, 0),
]

# Verificari finale pe log: (linie exacta, minim aparitii)
CHECKS = [
    ("ysh", 1), ("cat", 1), ("ls", 1),              # ls /bin
    ("x", 1), ("got-ok", 1), ("more", 2), ("Q=exp", 1), ("bg-ok", 1),
    ("/tmp", 1), ("/root", 1), ("one-ok", 2), ("two-ok", 1),
    ("abc", 1), ("xq", 1), ("bcde", 1), ("tab-ok /etc/os-release", 1),
    ("Bine ai venit pe YokaiOS.", 1), ("a  b c ok d e", 1), ("st-1", 1), ("st-127", 1), ("yes-or", 1),
    ("cc-ok", 1), ("spin: pornit", 1), ("st-130", 1), ("sync-last", 1),
    ("instalat hello 1.0.0 (2 fisiere)", 1), ("scos hello (2 fisiere)", 1),
]
FORBIDDEN = ["nope", "no-and"]
# Substringuri care trebuie sa apara (prompt-ul cu cwd-ul curent, lista de la Tab)
CONTAINS = [
    "yokai@drone_terminal:~$ ", "yokai@drone_terminal:/tmp$ ",
    "exec.caps     group         hostname      hosts         motd          mtab          os-release    passwd        resolv.conf",
    "selftest.lua  shadow",
    "YOKAI OS version 0.1 - ysh",
    "[OK] test_ysh",
]

KEYS = {
    " ": "spc", "-": "minus", "=": "equal", "/": "slash", ".": "dot", ",": "comma", ";": "semicolon",
    "'": "apostrophe", "\\": "backslash", "`": "grave_accent", "[": "bracket_left", "]": "bracket_right",
    "|": "shift-backslash", ">": "shift-dot", "<": "shift-comma", "$": "shift-4", "&": "shift-7", "~": "shift-grave_accent",
    "_": "shift-minus", ":": "shift-semicolon", '"': "shift-apostrophe", "*": "shift-8", "(": "shift-9", ")": "shift-0",
    "!": "shift-1", "#": "shift-3", "?": "shift-slash", "+": "shift-equal", "@": "shift-2", "%": "shift-5", "^": "shift-6",
}
SPECIAL = {"ret": "ret", "tab": "tab", "up": "up", "down": "down", "left": "left", "right": "right", "home": "home",
           "end": "end", "del": "delete", "bs": "backspace", "ctrl-c": "ctrl-c"}


def tokens(text):
    i = 0
    while i < len(text):
        c = text[i]
        if c == "{":
            j = text.index("}", i)
            yield SPECIAL[text[i + 1:j]]
            i = j + 1
            continue
        if c.isupper():
            yield "shift-" + c.lower()
        elif c.isalnum():
            yield c
        else:
            yield KEYS[c]
        i += 1


def lines_of(path):
    try:
        with open(path, "rb") as f:
            data = f.read().decode("latin-1")
    except OSError:
        return []
    return data.replace("\r", "").split("\n")


def count_matches(lines, want):
    if want.startswith("re:"):
        rx = re.compile(want[3:])
        return sum(1 for l in lines if rx.search(l))
    return sum(1 for l in lines if l == want)


def run(sock_path, log_path):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    time.sleep(0.3)
    s.settimeout(0.05)

    def send(cmd):
        s.sendall((cmd + "\n").encode())
        try:                                   # goleste raspunsul monitorului
            while s.recv(4096):
                pass
        except (socket.timeout, OSError):
            pass

    # Asteapta primul prompt (init a montat, ysh a pornit)
    deadline = time.time() + 90
    while time.time() < deadline and not any("drone_terminal:" in l for l in lines_of(log_path)):
        time.sleep(0.3)
    time.sleep(0.5)
    for keys, want, times in STEPS:
        for k in tokens(keys):
            send("sendkey " + k)
            time.sleep(0.09)
        if want is None:
            continue
        deadline = time.time() + 90
        while time.time() < deadline:
            if count_matches(lines_of(log_path), want) >= times:
                break
            time.sleep(0.25)
        else:
            print("ysh_test: timeout asteptand %r (x%d) dupa %r" % (want, times, keys), file=sys.stderr)
        time.sleep(0.4)                        # prompt-ul urmator ajunge sa se deseneze


def check(log_path):
    lines = lines_of(log_path)
    text = "\n".join(lines)
    ok = True
    for want, n in CHECKS:
        got = count_matches(lines, want)
        if got < n:
            print("FAIL: linia %r apare de %d ori (minim %d)" % (want, got, n))
            ok = False
    for bad in FORBIDDEN:
        if bad in lines:
            print("FAIL: linia interzisa %r" % bad)
            ok = False
    for sub in CONTAINS:
        if sub not in text:
            print("FAIL: lipseste %r" % sub)
            ok = False
    return ok


if __name__ == "__main__":
    if sys.argv[1] == "run":
        run(sys.argv[2], sys.argv[3])
    else:
        if not check(sys.argv[2]):
            sys.exit(1)
        print("PASS (ysh interactiv: %d pasi)" % len(STEPS))
