#pragma once

// Aplicatii GUI (T10.5): ruleaza /bin/appstest (ycomp + yapps, comenzi prin input injectat) si
// verifica la sfarsit ca nu raman procese, obiecte shm, socket-uri AF_UNIX, captura de input sau pagini pierdute;
// cmdline "test=apps"
void test_apps_run();
