#pragma once
#include "types.h"
#include "proc/file.h"

const uint64_t PIPE_SIZE = 4096;

// Pipe cu buffer circular de 4 KiB: read blocheaza cat e gol (0 = EOF dupa ce s-au inchis toate capetele de
// scriere), write blocheaza pana scrie tot (-EPIPE daca nu mai exista cititori).
// rd si wr primesc cate o referinta; 0 sau -ENOMEM
int pipe_create(File*& rd, File*& wr);
