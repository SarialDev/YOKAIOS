#pragma once

// virtio-gpu (T10.6): ruleaza /bin/gputest (/dev/gpu0 direct, apoi ycomp pe backend-ul virtio si pe fb0, cu timpul CPU
// comparat) si verifica la sfarsit ca resursa 2D e eliberata (fara scanout, fara pagini pierdute), ca nu raman procese,
// obiecte shm, socket-uri AF_UNIX sau captura de input; cmdline "test=gpu" (QEMU cu -device virtio-gpu-pci)
void test_gpu_run();
