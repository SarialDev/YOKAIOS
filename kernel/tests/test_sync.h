#pragma once

// Teste pentru mutex, semafor, completion si priority inheritance; ruleaza cu cmdline "test=sync"
void test_sync_run();
// Ia acelasi spinlock de doua ori: trebuie sa se termine cu panic "re-lock" (cmdline "test=relock")
void test_spin_relock();
