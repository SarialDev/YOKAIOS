#pragma once

// Criptografia din lib/crypto (SHA-256, HMAC-SHA256, PBKDF2, format pwhash) compilata in kernel + /bin/cryptotest
// (aceleasi surse legate din libc.a in userland: vectorii RFC 6234 / 4231 / 7914); cmdline "test=crypto"
void test_crypto_run();
