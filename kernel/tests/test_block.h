#pragma once

// Block layer fara hardware: coada de cereri, buffer cache (LRU, write-back, flush) si partitii MBR/GPT
// pe un disc in memorie; cmdline "test=block"
void test_block_run();
