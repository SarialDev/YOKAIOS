#pragma once

// NVMe pe QEMU (make test-nvme): doua namespace-uri (32 MiB x 512 B cu GPT, 4 MiB x 4096 B), IDENTIFY, partitii,
// READ/WRITE cu PRP (1, 2, lista), interogare si MSI-X, flush, bcache pe disc real. cmdline "test=nvme"
void test_nvme_run();
