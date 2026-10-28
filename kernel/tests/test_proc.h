#pragma once

// Pipe si message queue intre thread-uri de kernel, apoi programul user forktest (fork, pipe, waitpid,
// execve, WNOHANG, orfani); cmdline "test=proc"
void test_proc_run();
