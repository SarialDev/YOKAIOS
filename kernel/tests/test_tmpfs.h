#pragma once

// tmpfs montat pe /tmp si /run: fisiere cu pagini la cerere (goluri, trunchiere, scurgeri), directoare, symlink-uri,
// fisier sters cat e deschis, limita size=; cmdline "test=tmpfs"
void test_tmpfs_run();
