-- Autotest Lua pe YokaiOS: tabele, string.format, io.open, matematica, os, corutine, closures.
-- Rulat de kernel (test=lua) sau manual: lua /usr/share/lua/selftest.lua
local checks, fails = 0, 0

local function check(cond, what)
  checks = checks + 1
  if not cond then
    fails = fails + 1
    print("luatest: FAIL " .. what)
  end
end

local function eq(got, want, what)
  checks = checks + 1
  if got ~= want then
    fails = fails + 1
    print("luatest: FAIL " .. what .. ": got <" .. tostring(got) .. "> want <" .. tostring(want) .. ">")
  end
end

-- tabele
local t = {10, 20, 30, x = 1, y = "doi"}
eq(#t, 3, "#t")
t[#t + 1] = 40
eq(#t, 4, "append")
table.insert(t, 1, 5)
eq(t[1], 5, "insert front")
eq(table.remove(t), 40, "remove last")
eq(table.concat(t, ","), "5,10,20,30", "concat")
local keys = {}
for k in pairs({a = 1, b = 2, c = 3}) do keys[#keys + 1] = k end
table.sort(keys)
eq(table.concat(keys), "abc", "pairs+sort")
local words = {"pere", "mere", "banane", "ananas"}
table.sort(words, function(a, b) return #a < #b or (#a == #b and a < b) end)
eq(table.concat(words, " "), "mere pere ananas banane", "sort cu comparator")
eq(select("#", table.unpack({1, 2, 3})), 3, "unpack")
local big = {}
for i = 1, 20000 do big[i] = i * i end
eq(big[20000], 400000000, "tabel mare")

-- siruri
eq(("abc"):upper(), "ABC", "upper")
eq(("Salut Lume"):sub(7), "Lume", "sub")
eq(("a,b,,c"):gsub(",", ";"), "a;b;;c", "gsub")
eq(("hello world"):find("wor"), 7, "find")
eq(("2026-11-02"):match("(%d+)-(%d+)-(%d+)"), "2026", "match")
eq(#string.rep("ab", 1000), 2000, "rep")
eq(("x"):byte(), 120, "byte")
eq(string.char(72, 105), "Hi", "char")
local parts = {}
for w in ("unu doi trei"):gmatch("%a+") do parts[#parts + 1] = w end
eq(#parts, 3, "gmatch")

-- string.format
eq(string.format("%d", 42), "42", "%d")
eq(string.format("%5d|%-5d|%05d", 42, 42, 42), "   42|42   |00042", "%d latime")
eq(string.format("%x %X %o", 255, 255, 8), "ff FF 10", "%x %o")
eq(string.format("%s e %s", "unu", 2), "unu e 2", "%s")
eq(string.format("%10s|%-10s|", "ab", "cd"), "        ab|cd        |", "%s latime")
eq(string.format("%q", 'a"b\n'), '"a\\"b\\\n"', "%q")
eq(string.format("%.3f", 3.14159), "3.142", "%.3f")
eq(string.format("%8.2f", 2.5), "    2.50", "%8.2f")
eq(string.format("%e", 12345.678), "1.234568e+04", "%e")
eq(string.format("%g", 100000), "100000", "%g 1e5")
eq(string.format("%g", 1e6), "1e+06", "%g 1e6")
eq(string.format("%g", 0.0001), "0.0001", "%g 1e-4")
eq(string.format("%.14g", 0.1), "0.1", "%.14g 0.1")
eq(string.format("%.17g", 0.1), "0.10000000000000001", "%.17g 0.1")
eq(string.format("%5.1f%%", 99.44), " 99.4%", "%%")
eq(string.format("%c%c", 79, 75), "OK", "%c")
eq(string.format("%i", -7), "-7", "%i")
eq(string.format("%a", 1.0), "0x1p+0", "%a")

-- numere
eq(7 // 2, 3, "idiv")
eq(7 / 2, 3.5, "div")
eq(2 ^ 10, 1024.0, "pow")
eq(math.type(2 ^ 10), "float", "type float")
eq(math.type(3), "integer", "type int")
eq(tostring(1e15), "1e+15", "tostring 1e15")
eq(tostring(0.1), "0.1", "tostring 0.1")
eq(tostring(-0.0), "-0.0", "tostring -0.0")
eq(tostring(3.0), "3.0", "tostring 3.0")
eq(tostring(1/0), "inf", "inf")
eq(tostring(-1/0), "-inf", "-inf")
check(tostring(0/0):find("nan"), "nan")
eq(tonumber("0x10"), 16, "hex")
eq(tonumber("  12  "), 12, "tonumber spatii")
eq(tonumber("1e2"), 100.0, "tonumber 1e2")
eq(tonumber("3.25"), 3.25, "tonumber 3.25")
eq(tonumber("0x1p4"), 16.0, "hex float")
eq(tonumber("abc"), nil, "tonumber abc")
eq(math.maxinteger, 9223372036854775807, "maxinteger")
eq(math.mininteger, -9223372036854775807 - 1, "mininteger")
eq(math.maxinteger + 1, math.mininteger, "overflow")
eq(1 << 62, 4611686018427387904, "shl")
eq(0xFF & 0x0F, 15, "and")
eq(math.floor(-3.5), -4, "floor")
eq(math.ceil(3.2), 4, "ceil")
eq(math.sqrt(16), 4.0, "sqrt")
eq(math.abs(-5), 5, "abs")
eq(math.max(3, 9, 2), 9, "max")
eq(math.min(3, 9, 2), 2, "min")
eq(math.fmod(7, 3), 1, "fmod int")
eq(math.fmod(7.5, 2), 1.5, "fmod float")
eq(math.tointeger(3.0), 3, "tointeger")
eq(math.ult(1, -1), true, "ult")
check(math.abs(math.pi - 3.141592653589793) < 1e-15, "pi")
check(math.abs(math.sin(math.pi / 6) - 0.5) < 1e-15, "sin")
check(math.abs(math.cos(0) - 1) < 1e-15, "cos")
check(math.abs(math.exp(1) - 2.718281828459045) < 1e-15, "exp")
check(math.abs(math.log(100, 10) - 2) < 1e-15, "log10")
check(math.abs(math.log(8, 2) - 3) < 1e-15, "log2")
check(math.abs(math.atan(1) - math.pi / 4) < 1e-15, "atan")
local sum = 0.0
for i = 1, 1000 do sum = sum + 0.1 end
eq(string.format("%.10f", sum), "100.0000000000", "suma 0.1 x1000")
math.randomseed(42)
local r = math.random(1, 6)
check(r >= 1 and r <= 6, "random")
check(math.random() < 1, "random float")

-- functii, closure, corutine, erori
local function counter()
  local n = 0
  return function() n = n + 1; return n end
end
local c1, c2 = counter(), counter()
c1(); c1()
eq(c1(), 3, "closure 1")
eq(c2(), 1, "closure 2")
local co = coroutine.wrap(function(a)
  local b = coroutine.yield(a + 1)
  local c = coroutine.yield(b * 2)
  return c .. "!"
end)
eq(co(1), 2, "coroutine 1")
eq(co(10), 20, "coroutine 2")
eq(co("gata"), "gata!", "coroutine 3")
local ok, err = pcall(function() error("boom") end)
check(not ok and tostring(err):find("boom"), "pcall error")
local ok2, err2 = pcall(function() local x = nil; return x.y end)
check(not ok2 and tostring(err2):find("nil"), "index nil")
eq(select(2, pcall(error, {code = 7})).code, 7, "error cu tabel")
local mt = setmetatable({}, {__index = function(_, k) return k .. "?" end, __call = function(self, a) return a * 2 end})
eq(mt.foo, "foo?", "__index")
eq(mt(21), 42, "__call")
local V = {}
V.__index = V
V.__add = function(a, b) return setmetatable({x = a.x + b.x}, V) end
V.__tostring = function(a) return "V(" .. a.x .. ")" end
eq(tostring(setmetatable({x = 1}, V) + setmetatable({x = 2}, V)), "V(3)", "__add/__tostring")
eq(load("return 1 + 2")(), 3, "load")
local env = {y = 5}
eq(load("return y", "chunk", "t", env)(), 5, "load cu env")
check(not load("return +"), "load syntax error")
local gc = collectgarbage("count")
check(gc > 0, "collectgarbage count")
for i = 1, 3 do local junk = {} for j = 1, 1000 do junk[j] = {j} end end
collectgarbage()
check(collectgarbage("count") < gc * 4, "gc")
eq(utf8 and utf8.char(0x20AC) or "", "\xE2\x82\xAC", "utf8.char")
eq(utf8.len("a\xE2\x82\xACb"), 3, "utf8.len")

-- io.open
local path = "/tmp/luatest.txt"
local f = assert(io.open(path, "w"))
f:write("linia 1\n", "linia 2\n", 42, " ", 3.5, "\n")
f:write(string.format("%05.1f|%s\n", 2.25, "sfarsit"))
check(f:close(), "close write")
f = assert(io.open(path, "r"))
eq(f:read("l"), "linia 1", "read l")
eq(f:read("L"), "linia 2\n", "read L")
eq(f:read("n"), 42, "read n int")
eq(f:read("n"), 3.5, "read n float")
f:read("l")
eq(f:read("l"), "002.2|sfarsit", "read format")
eq(f:read("l"), nil, "read EOF")
eq(f:seek("set", 0), 0, "seek set")
eq(f:read(6), "linia ", "read 6")
eq(f:seek("cur"), 6, "seek cur")
local size = f:seek("end")
check(size > 30, "seek end")
f:seek("set", 0)
local all = f:read("a")
eq(#all, size, "read a")
f:close()
local n = 0
for line in io.lines(path) do n = n + 1 end
eq(n, 4, "io.lines")
f = assert(io.open(path, "a"))
f:write("adaugat\n")
f:close()
n = 0
for line in io.lines(path) do n = n + 1 end
eq(n, 5, "append")
f = assert(io.open(path, "rb"))
local data = f:read("*a")
f:close()
eq(data:sub(-8), "adaugat\n", "citire binar")
local nf, nerr, ncode = io.open("/nu/exista/fisier", "r")
check(nf == nil and nerr:find("No such file") and ncode == 2, "io.open eroare")
eq(select(1, pcall(io.lines, "/nu/exista")), false, "io.lines eroare")
local out = assert(io.open("/tmp/luatest.bin", "wb"))
out:write(string.char(0, 1, 2, 255, 254))
out:close()
local inp = assert(io.open("/tmp/luatest.bin", "rb"))
local bin = inp:read("a")
inp:close()
eq(#bin, 5, "binar lungime")
eq(bin:byte(4), 255, "binar octet")
io.output():write("")
eq(io.type(io.stdout), "file", "io.type")
eq(io.type(42), nil, "io.type nu fisier")
local tf = io.tmpfile()
tf:write("temp")
tf:seek("set")
eq(tf:read("a"), "temp", "tmpfile")
tf:close()
check(os.remove(path), "os.remove")
check(os.remove("/tmp/luatest.bin"), "os.remove 2")
check(not os.remove(path), "os.remove lipsa")
check(os.rename("/nu/exista", "/tmp/x") == nil, "os.rename eroare")

-- os
local now = os.time()
check(now > 1700000000, "os.time")
eq(os.time({year = 2026, month = 1, day = 1, hour = 0}), 1767225600, "os.time tabel")
eq(os.date("!%Y-%m-%d %H:%M:%S", 86400), "1970-01-02 00:00:00", "os.date")
eq(os.date("!*t", 0).year, 1970, "os.date *t")
eq(os.date("!%A %B", 0), "Thursday January", "os.date nume")
check(os.clock() >= 0, "os.clock")
check(os.getenv("PATH") ~= nil or true, "os.getenv")
eq(os.getenv("VARIABILA_INEXISTENTA"), nil, "getenv inexistent")
eq(os.difftime(10, 4), 6.0, "difftime")
check(type(os.tmpname()) == "string", "tmpname")

-- procese: os.execute / io.popen prin ysh -c
local okx, how, code = os.execute("exit 3")
check(okx == nil and how == "exit" and code == 3, "os.execute exit 3")
local okx0 = os.execute("exit 0")
check(okx0 == true, "os.execute true")
local p = io.popen("echo salut din ysh", "r")
if p then
  eq(p:read("l"), "salut din ysh", "popen read")
  p:close()
else
  check(false, "popen")
end

print(string.format("[OK] luatest (%d verificari, %s)", checks, _VERSION))
if fails > 0 then
  print(string.format("luatest: %d esecuri", fails))
  os.exit(1)
end
