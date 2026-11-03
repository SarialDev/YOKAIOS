#!/bin/sh
# Disc criptat (make test-crypt): ext4 construit pe gazda cu mke2fs -d, criptat cu tools/cryptimg (aceleasi surse lib/crypto ca
# in kernel), montat de kernel prin /bin/cryptsetup (test=crypt), scris acolo, apoi verificat pe gazda:
#  - textul in clar NU apare nicaieri in imaginea bruta (nici inainte, nici dupa boot);
#  - decriptata cu parola, imaginea trece e2fsck -fn si debugfs gaseste fisierele scrise de kernel;
#  - un AES-XTS independent (python `cryptography`, daca exista) decripteaza un sector la fel ca unealta noastra.
# Uz: tests/check_crypt.sh <iso> <dir de lucru> [xts|chacha20] ; QEMU_EXTRA / SMP se transmit lui run_qemu.sh
set -eu
ISO=${1:?iso}
W=${2:?dir}
CIPHER=${3:-xts}
PW="sda-disk-passphrase"
HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)
mkdir -p "$W/tree/secrets"
printf 'PLAINTEXT-ORIGINAL-SECRET: the quick brown fox 0123456789\n' > "$W/tree/secrets/plan.txt"

gcc -O2 -Wall -Wextra -I"$ROOT/lib" -o "$W/cryptimg" "$ROOT/tools/cryptimg.c" \
    "$ROOT"/lib/crypto/aes.c "$ROOT"/lib/crypto/xts.c "$ROOT"/lib/crypto/chacha20.c "$ROOT"/lib/crypto/sha256.c \
    "$ROOT"/lib/crypto/hmac.c "$ROOT"/lib/crypto/pbkdf2.c "$ROOT"/lib/crypto/ykcrypt.c

rm -f "$W/plain.img" "$W/enc.img"
mke2fs -q -F -t ext4 -b 4096 -L YKCRYPT -O 64bit,metadata_csum,flex_bg,dir_index,extent \
    -E root_owner=0:0 -d "$W/tree" "$W/plain.img" 16m
"$W/cryptimg" encrypt "$W/plain.img" "$W/enc.img" "$PW" "$CIPHER" 1000
# discul bate imaginea clara ca marime + antet (4 KiB)
if grep -qa "PLAINTEXT-ORIGINAL-SECRET" "$W/enc.img"; then
    echo "FAIL: textul in clar apare in imaginea criptata (inainte de boot)"; exit 1
fi
if ! grep -qa "PLAINTEXT-ORIGINAL-SECRET" "$W/plain.img"; then
    echo "FAIL: imaginea clara nu contine textul (verificarea ar fi inutila)"; exit 1
fi
head -c 8 "$W/enc.img" | grep -q YKCRYPT1 || { echo "FAIL: antet YKCRYPT1 lipsa"; exit 1; }
echo "check_crypt: imagine criptata ($CIPHER) facuta, fara text in clar"

TEST_TIMEOUT=${TEST_TIMEOUT:-900} DISK="$W/enc.img" EXPECT_FILE="$HERE/expect-crypt.txt" sh "$HERE/run_qemu.sh" "$ISO" "$W/boot.log"

for m in PLAINTEXT-ORIGINAL-SECRET TOPSECRET-WRITTEN-AT-RUNTIME TOPSECRET-BULK; do
    if grep -qa "$m" "$W/enc.img"; then
        echo "FAIL: \"$m\" apare in imaginea criptata dupa boot"; exit 1
    fi
done
echo "check_crypt: dupa boot, nici un text in clar in imaginea bruta"

"$W/cryptimg" decrypt "$W/enc.img" "$W/dec.img" "$PW"
for m in TOPSECRET-WRITTEN-AT-RUNTIME TOPSECRET-BULK; do
    grep -qa "$m" "$W/dec.img" || { echo "FAIL: \"$m\" lipseste din imaginea decriptata"; exit 1; }
done
e2fsck -fn "$W/dec.img" > "$W/e2fsck.log" 2>&1 || { cat "$W/e2fsck.log"; echo "FAIL: e2fsck pe imaginea decriptata"; exit 1; }
debugfs -R "cat /secrets/plan.txt" "$W/dec.img" 2>/dev/null | grep -q "PLAINTEXT-ORIGINAL-SECRET: the quick brown fox 0123456789" || { echo "FAIL: plan.txt"; exit 1; }
debugfs -R "cat /runtime.txt" "$W/dec.img" 2>/dev/null | grep -q "TOPSECRET-WRITTEN-AT-RUNTIME: pack my box with five dozen liquor jugs" || { echo "FAIL: runtime.txt"; exit 1; }
python3 - "$W" <<'PY'
import subprocess, sys
w = sys.argv[1]
out = subprocess.run(["debugfs", "-R", "cat /bulk.txt", w + "/dec.img"], capture_output=True).stdout
want = b"".join(b"TOPSECRET-BULK-%07d " % i + b"x" * 40 + b"\n" for i in range(8192))
if out != want:
    sys.exit("FAIL: bulk.txt difera (%d octeti, asteptat %d)" % (len(out), len(want)))
PY
echo "check_crypt: e2fsck curat, runtime.txt si bulk.txt (512 KiB) corecte dupa decriptare pe gazda"

# verificare incrucisata cu o implementare independenta (doar XTS)
python3 - "$W" "$CIPHER" "$PW" <<'PY'
import subprocess, sys
w, cipher, pw = sys.argv[1:4]
try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    import hashlib, hmac
except ImportError:
    print("check_crypt: python `cryptography` lipseste, sar peste verificarea independenta"); sys.exit(0)
if cipher != "xts":
    sys.exit(0)
img = open(w + "/enc.img", "rb").read()
import struct
iters = struct.unpack("<I", img[16:20])[0]
salt, nonce, wrapped, mac = img[56:88], img[88:100], img[100:164], img[164:196]
kek = hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, iters, 32)
kenc = hmac.new(kek, b"yokai-crypt-enc", "sha256").digest()
kmac = hmac.new(kek, b"yokai-crypt-mac", "sha256").digest()
assert hmac.compare_digest(hmac.new(kmac, img[:164], "sha256").digest(), mac), "MAC antet (python) difera"
c = Cipher(algorithms.ChaCha20(kenc, (0).to_bytes(4, "little") + nonce), mode=None).decryptor()
master = c.update(wrapped)
for s in (0, 1, 777, 2000):
    ct = img[4096 + s * 512: 4096 + (s + 1) * 512]
    d = Cipher(algorithms.AES(master), modes.XTS(s.to_bytes(16, "little"))).decryptor()
    got = d.update(ct) + d.finalize()
    mine = bytes.fromhex(subprocess.run([w + "/cryptimg", "sector", w + "/enc.img", pw, str(s)], capture_output=True, text=True).stdout.strip())
    assert got == mine, "sectorul %d: python != cryptimg" % s
print("check_crypt: antetul (PBKDF2 + ChaCha20 + HMAC) si 4 sectoare AES-XTS confirmate de python `cryptography`")
PY
echo "PASS check_crypt ($CIPHER)"
