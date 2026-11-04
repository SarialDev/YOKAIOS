#pragma once
#include "types.h"

// Intrarea compozitorului (T10.2): fd cu captura exclusiva a tastaturii si mouse-ului (uapi/yinput.h)
int64_t sys_inputctl(uint64_t op, uint64_t arg, uint64_t, uint64_t, uint64_t, uint64_t);
// Exista un fd de captura deschis?
bool yinput_grabbed();
