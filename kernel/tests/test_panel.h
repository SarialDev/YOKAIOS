#pragma once

// Panoul ypanel (T10.4): ruleaza /bin/paneltest (ycomp + ypanel, comenzi prin input injectat) si
// verifica la sfarsit ca nu raman procese, obiecte shm, socket-uri AF_UNIX, captura de input sau pagini pierdute;
// cmdline "test=panel"
void test_panel_run();
