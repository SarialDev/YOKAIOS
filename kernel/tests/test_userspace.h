#pragma once

// test=userspace: porneste /sbin/init -t, care ruleaza /bin/selftest (utilitare, ysh -c, Lua, fork CoW, ypkg);
// trebuie sa iasa cu 0 dupa ce a tiparit "[OK] userspace"
void test_userspace_run();
