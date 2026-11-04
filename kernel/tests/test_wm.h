#pragma once

// Managerul de ferestre al ycomp (T10.3): ruleaza /bin/wmtest (ycomp cu 5 ferestre, comenzi prin input injectat) si
// verifica la sfarsit ca nu raman procese, obiecte shm, socket-uri AF_UNIX, captura de input sau pagini pierdute;
// cmdline "test=wm"
void test_wm_run();
