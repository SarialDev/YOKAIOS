#pragma once

// Teste pentru T2 (exceptii, ceas); ruleaza doar cu cmdline "test=cpu"
void test_cpu_run();

// Depaseste un buffer de pe stiva: trebuie sa se termine cu panic "stack smashing" (cmdline "test=smash")
void test_stack_smash();
