#pragma once

// Compozitorul ycomp (T10.2): ruleaza /bin/comptest (shm, AF_UNIX, poll, input, rt_stats, apoi ycomp cu doi clienti)
// si verifica la sfarsit ca nu raman procese, obiecte shm, socket-uri AF_UNIX, captura de input sau pagini pierdute;
// cmdline "test=comp"
void test_comp_run();
