#!/bin/sh
# KASLR + ASLR: porneste de doua ori ISO-ul cu test=aslr si cere ca adresa lui kernel_main si asezarea lui /bin/aslrtest
# (stiva, heap, mmap, cod) sa difere intre cele doua boot-uri.
# Uz: tests/check_aslr.sh <iso> <prefix-log>   (log-urile: <prefix>-1.log, <prefix>-2.log)
# KASLR are ~500 de pozitii (2 MiB in 1 GiB): la o coincidenta (~0,2%) se mai face un singur boot
set -u
ISO=${1:?iso lipsa}
PRE=${2:-build/aslr}
DIR=$(dirname "$0")

boot(){
    EXPECT_FILE="$DIR/expect-aslr.txt" sh "$DIR/run_qemu.sh" "$ISO" "$PRE-$1.log" || exit 1
}

field(){        # field <log> <cheie>: valoarea "cheie=..." din prima linie care o are
    grep -a -o " $2=[0-9a-fx]*" "$1" | head -n1 | cut -d= -f2
}

boot 1
boot 2
k1=$(field "$PRE-1.log" kernel_main); k2=$(field "$PRE-2.log" kernel_main)
if [ "$k1" = "$k2" ]; then
    echo "check_aslr: kernel_main la fel ($k1), coincidenta posibila: inca un boot"
    boot 3
    cp "$PRE-3.log" "$PRE-2.log"
    k2=$(field "$PRE-2.log" kernel_main)
fi

fail=0
for key in kernel_main stack heap mmap main; do
    a=$(field "$PRE-1.log" $key); b=$(field "$PRE-2.log" $key)
    if [ -z "$a" ] || [ -z "$b" ]; then
        echo "FAIL: $key lipseste din log"; fail=1
    elif [ "$a" = "$b" ]; then
        echo "FAIL: $key identic in ambele boot-uri ($a)"; fail=1
    else
        echo "  $key: $a / $b"
    fi
done
[ $fail = 0 ] || exit 1
echo "PASS (KASLR + ASLR diferite intre doua boot-uri)"
