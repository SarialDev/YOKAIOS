#!/bin/sh
# Semnaturi Ed25519 pe gazda (T8.5): un pachet / modul / kernel modificat cu un octet trebuie respins, nemodificat acceptat.
# Uz: tests/check_sign.sh <director build>   (cheia: $SIGN_KEY, $YOKAI_SIGN_KEY sau cheia de dezvoltare locala)
set -eu
B=${1:-build}
KEY=${SIGN_KEY:-}
SIGN="python3 tools/sign.py"
MKY="python3 tools/mkyokai"
if [ -n "$KEY" ]; then
    SIGN="$SIGN --key $KEY"
    MKY_KEY="--key $KEY"
else
    MKY_KEY=""
fi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0

# flip <fisier> <offset>: schimba un bit
flip() {
    python3 - "$1" "$2" <<'PY'
import sys
p, off = sys.argv[1], int(sys.argv[2])
b = bytearray(open(p, "rb").read())
if off < 0:
    off += len(b)
b[off] ^= 1
open(p, "wb").write(b)
PY
}

must_fail() {
    if "$@" >"$T/out" 2>&1; then
        echo "check_sign: FAIL: trebuia respins: $*"
        fail=1
    fi
}
must_pass() {
    if ! "$@" >"$T/out" 2>&1; then
        echo "check_sign: FAIL: trebuia acceptat: $*"
        cat "$T/out"
        fail=1
    fi
}

n=0
for pkg in "$B"/pkg/*.yokai; do
    must_pass $MKY verify "$pkg" $MKY_KEY
    # un octet schimbat in fiecare intrare a arhivei: antet tar, manifest (la inceput / mijloc / sfarsit), continutul fiecarui fisier
    offs=$(python3 - "$pkg" <<'PY'
import sys, tarfile
tf = tarfile.open(sys.argv[1], "r:")
out = []
for m in tf.getmembers():
    out.append(m.offset)                                   # antetul tar al intrarii (nume)
    if m.isfile() and m.size:
        out += [m.offset_data, m.offset_data + m.size // 2, m.offset_data + m.size - 1]
print(" ".join(str(o) for o in out))
PY
)
    for off in $offs; do
        cp "$pkg" "$T/bad.yokai"
        flip "$T/bad.yokai" "$off"
        must_fail $MKY verify "$T/bad.yokai" $MKY_KEY
        n=$((n + 1))
    done
    # alta cheie publica => semnatura nu corespunde
    must_fail $MKY verify "$pkg" --pub 00000000000000000000000000000000000000000000000000000000000000ff
    n=$((n + 1))
done
for ko in "$B"/modules/*.ko; do
    must_pass $SIGN verify-module "$ko"
    size=$(wc -c < "$ko")
    for off in 0 100 $((size / 2)) $((size - 73)) $((size - 72)) $((size - 9)); do
        cp "$ko" "$T/bad.ko"
        flip "$T/bad.ko" "$off"
        must_fail $SIGN verify-module "$T/bad.ko"
        n=$((n + 1))
    done
done
must_pass $SIGN verify-kernel "$B/kernel.bin" "$B/kernel.sig"
cp "$B/kernel.bin" "$T/kernel.bad"
flip "$T/kernel.bad" 5000
must_fail $SIGN verify-kernel "$T/kernel.bad" "$B/kernel.sig"
n=$((n + 1))

if [ "$fail" = 0 ]; then
    echo "[OK] check_sign: $n modificari de un octet respinse (pachete, module, kernel); originalele acceptate"
else
    exit 1
fi
