# Lua 5.4 (third_party/lua-5.4, neschimbat) -> /bin/lua. Inclus din Makefile.
# Se compileaza cu libc-ul propriu; LUA_USE_POSIX: popen, mkstemp, isatty, sigaction, _setjmp (toate in libc-ul YokaiOS).
# Fara readline, fara dlopen (require incarca doar module Lua), fara luac.
LUA_DIR     := third_party/lua-5.4/src
LUA_SRCS    := $(filter-out $(LUA_DIR)/luac.c,$(wildcard $(LUA_DIR)/*.c))
LUA_OBJS    := $(patsubst $(LUA_DIR)/%.c,$(BUILD)/lua/%.o,$(LUA_SRCS))
LUA_HDRS    := $(wildcard $(LUA_DIR)/*.h)
LUA_CFLAGS  := -O2 -std=gnu99 -Wall -Wextra -DLUA_USE_POSIX -DLUA_COMPAT_5_3
LUA_SHARE   := user/bin/lua/selftest.lua

$(BUILD)/lua/%.o: $(LUA_DIR)/%.c $(LUA_HDRS) $(SYSROOT)/.installed
	@mkdir -p $(dir $@)
	$(UCC) $(LUA_CFLAGS) -c $< -o $@

$(BUILD)/ubin/lua.elf: $(LUA_OBJS) $(SYSROOT)/.installed
	@mkdir -p $(dir $@)
	$(UCC) -o $@ $(LUA_OBJS)
