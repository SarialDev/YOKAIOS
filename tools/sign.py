#!/usr/bin/env python3
"""Semnaturi Ed25519 pentru YokaiOS (T8.5): kernel, module de kernel si pachete .yokai. Fara dependente (Python pur, RFC 8032).

Cheia privata sta in AFARA depozitului: --key, altfel $YOKAI_SIGN_KEY, altfel ~/.config/yokai/sign.key (cheie de dezvoltare,
generata local daca lipseste). Fisierul contine seed-ul de 32 de octeti ca 64 de cifre hex; se creeaza cu drepturi 0600.

  sign.py keygen [--key F]                      creeaza cheia daca lipseste (nu o suprascrie niciodata); afiseaza cheia publica
  sign.py pubkey [--key F] [--format hex|c] [-o F]   cheia publica (fisierul C = ce compileaza kernelul; se rescrie doar daca difera)
  sign.py sign-kernel KERNEL [-o SIG]           semnatura detasata (64 de octeti) pentru kernel.bin
  sign.py verify-kernel KERNEL SIG [--pub HEX]
  sign.py sign-module MOD.ko -o OUT.ko          modul = ELF relocabil + semnatura (64) + magic de 8 octeti la sfarsit
  sign.py verify-module OUT.ko [--pub HEX]
  sign.py sign-manifest FILE                    semnatura (hex) pentru manifest.toml (cu linia signature blanca)
Mesajul semnat este mereu <context de domeniu> || date, deci o semnatura de pachet nu valoreaza ca semnatura de modul.
"""
import hashlib
import os
import re
import stat
import sys

CTX_PKG = b"yokai-pkg-v1\n"
CTX_KMOD = b"yokai-kmod-v1\n"
CTX_KERNEL = b"yokai-kernel-v1\n"
MOD_MAGIC = b"~YKSIG~\n"                  # 8 octeti la sfarsitul unui modul semnat
assert len(MOD_MAGIC) == 8

# ---- Ed25519 (RFC 8032, sectiunea 6 - implementarea de referinta, scurta si lenta; suficienta pentru cateva semnaturi) ----
P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
I = pow(2, (P - 1) // 4, P)
BY = 4 * pow(5, P - 2, P) % P


def _recover_x(y, sign):
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * I % P
    if (x * x - x2) % P:
        raise ValueError("punct invalid")
    if x & 1 != sign:
        x = P - x
    return x


BX = _recover_x(BY, 0)
B = (BX, BY, 1, BX * BY % P)


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    d = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _enc(p):
    zi = pow(p[2], P - 2, P)
    x, y = p[0] * zi % P, p[1] * zi % P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _dec(b):
    y = int.from_bytes(b, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return (x, y, 1, x * y % P)


def _h(m):
    return int.from_bytes(hashlib.sha512(m).digest(), "little")


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    return _enc(_mul(_expand(seed)[0], B))


def sign_raw(seed, msg):
    a, prefix = _expand(seed)
    pub = _enc(_mul(a, B))
    r = _h(prefix + msg) % L
    R = _enc(_mul(r, B))
    s = (r + _h(R + pub + msg) * a) % L
    return R + s.to_bytes(32, "little")


def verify_raw(pub, msg, sig):
    if len(sig) != 64 or len(pub) != 32:
        return False
    try:
        A = _dec(pub)
        R = _dec(sig[:32])
    except ValueError:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= L:
        return False
    h = _h(sig[:32] + pub + msg) % L
    sb, ha = _mul(s, B), _add(R, _mul(h, A))
    return (sb[0] * ha[2] - ha[0] * sb[2]) % P == 0 and (sb[1] * ha[2] - ha[1] * sb[2]) % P == 0


# ---- cheia ----
def key_path(arg=None):
    if arg:
        return arg
    if os.environ.get("YOKAI_SIGN_KEY"):
        return os.environ["YOKAI_SIGN_KEY"]
    return os.path.join(os.path.expanduser("~"), ".config", "yokai", "sign.key")


def ensure_key(path=None):
    """Intoarce seed-ul (32 octeti); genereaza cheia de dezvoltare daca fisierul nu exista."""
    path = key_path(path)
    if not os.path.exists(path):
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        seed = os.urandom(32)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(seed.hex() + "\n")
        print("sign.py: cheie de dezvoltare noua in %s (NU o pune in depozit)" % path, file=sys.stderr)
        return seed
    txt = open(path).read().strip()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", txt):
        sys.exit("sign.py: %s nu contine un seed de 64 de cifre hex" % path)
    if os.name == "posix" and os.stat(path).st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        print("sign.py: atentie: %s este lizibil de altii (chmod 600)" % path, file=sys.stderr)
    return bytes.fromhex(txt)


def pub_from_arg(args):
    if args.get("pub"):
        return bytes.fromhex(args["pub"])
    return public_key(ensure_key(args.get("key")))


# ---- formate ----
def sign_blob(seed, ctx, data):
    return sign_raw(seed, ctx + data)


def verify_blob(pub, ctx, data, sig):
    return verify_raw(pub, ctx + data, sig)


SIG_LINE = re.compile(rb"^signature[ \t]*=.*$", re.M)


def manifest_blank(text):
    """Manifestul asa cum se semneaza: linia `signature = ...` inlocuita prin `signature = ""`. Exact o linie."""
    if len(SIG_LINE.findall(text)) != 1:
        raise ValueError("manifestul trebuie sa aiba exact o linie 'signature = ...'")
    return SIG_LINE.sub(b'signature = ""', text, count=1)


def manifest_sign(seed, text):
    return sign_blob(seed, CTX_PKG, manifest_blank(text)).hex()


def manifest_set_signature(text, sighex):
    manifest_blank(text)
    return SIG_LINE.sub(('signature = "%s"' % sighex).encode(), text, count=1)


def manifest_verify(pub, text):
    """True/False; None daca manifestul nu e semnat (semnatura goala)."""
    m = SIG_LINE.search(text)
    q = re.search(rb'"([0-9a-f]*)"', m.group(0)) if m else None
    if not q:
        raise ValueError("linia signature nu are forma `signature = \"hex\"`")
    sighex = q.group(1)
    if not sighex:
        return None
    if len(sighex) != 128:
        return False
    return verify_blob(pub, CTX_PKG, manifest_blank(text), bytes.fromhex(sighex.decode()))


def module_sign(seed, elf):
    return elf + sign_blob(seed, CTX_KMOD, elf) + MOD_MAGIC


def module_split(blob):
    if len(blob) < 72 or blob[-8:] != MOD_MAGIC:
        return None
    return blob[:-72], blob[-72:-8]


def pubkey_c(pub):
    rows = ",\n".join("    " + ", ".join("0x%02x" % b for b in pub[i:i + 8]) for i in range(0, 32, 8))
    return ("/* Generat de tools/sign.py pubkey --format c: cheia publica Ed25519 de care are incredere acest kernel. */\n"
            "#include <stdint.h>\n"
            "extern const uint8_t yokai_sign_pubkey[32];\n"
            "const uint8_t yokai_sign_pubkey[32] = {\n%s\n};\n" % rows)


def write_if_changed(path, data):
    if isinstance(data, str):
        data = data.encode()
    try:
        if open(path, "rb").read() == data:
            return
    except OSError:
        pass
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    open(path, "wb").write(data)


def main(argv):
    args = {}
    pos = []
    i = 0
    flags = {"--key": "key", "--pub": "pub", "-o": "out", "--format": "format"}
    while i < len(argv):
        a = argv[i]
        if a in flags:
            i += 1
            if i >= len(argv):
                sys.exit("sign.py: %s cere o valoare" % a)
            args[flags[a]] = argv[i]
        else:
            pos.append(a)
        i += 1
    if not pos:
        sys.exit(__doc__)
    cmd, rest = pos[0], pos[1:]
    if cmd == "keygen":
        print(public_key(ensure_key(args.get("key"))).hex())
    elif cmd == "pubkey":
        pub = public_key(ensure_key(args.get("key")))
        if args.get("format", "hex") == "c":
            if "out" not in args:
                sys.exit("sign.py: --format c cere -o")
            write_if_changed(args["out"], pubkey_c(pub))
        else:
            print(pub.hex())
    elif cmd == "sign-kernel" and len(rest) == 1:
        data = open(rest[0], "rb").read()
        sig = sign_blob(ensure_key(args.get("key")), CTX_KERNEL, data)
        open(args.get("out", rest[0] + ".sig"), "wb").write(sig)
    elif cmd == "verify-kernel" and len(rest) == 2:
        ok = verify_blob(pub_from_arg(args), CTX_KERNEL, open(rest[0], "rb").read(), open(rest[1], "rb").read())
        print("kernel: semnatura %s" % ("valida" if ok else "INVALIDA"))
        sys.exit(0 if ok else 1)
    elif cmd == "sign-module" and len(rest) == 1 and "out" in args:
        open(args["out"], "wb").write(module_sign(ensure_key(args.get("key")), open(rest[0], "rb").read()))
    elif cmd == "verify-module" and len(rest) == 1:
        sp = module_split(open(rest[0], "rb").read())
        ok = bool(sp) and verify_blob(pub_from_arg(args), CTX_KMOD, sp[0], sp[1])
        print("modul: semnatura %s" % ("valida" if ok else "INVALIDA / lipsa"))
        sys.exit(0 if ok else 1)
    elif cmd == "sign-manifest" and len(rest) == 1:
        print(manifest_sign(ensure_key(args.get("key")), open(rest[0], "rb").read()))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
