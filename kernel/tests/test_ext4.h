#pragma once

// ext4 read-only (make test-ext4): imagine facuta cu mke2fs -d pe host (tests/mkext4.sh), radacina de pe sda2 din
// cmdline root=. mode = valoarea din "ext4mode=": "ok" (radacina ext4: verifica continutul, extente / harta de
// blocuri, htree / liniar, symlink-uri, -EROFS, listeaza tot arborele cu CRC32 pentru gazda), "refuse" (flag
// incompatibil necunoscut: montarea esueaza si radacina ramane initrd-ul) si "recover" (jurnal care cere
// recuperare: refuzat fara "noload"). cmdline "test=ext4 ext4mode=<mode>"
void test_ext4_run(const char* mode);
