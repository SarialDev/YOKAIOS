-- /etc/selftest.lua: partea Lua a testului de userspace (/bin/selftest). La succes ultima linie e "selftest.lua: ok"
local checks, failed = 0, 0

local function check(cond, what)
  checks = checks + 1
  if not cond then
    failed = failed + 1
    print("selftest.lua: FAIL " .. what)
  end
end

-- aritmetica, intregi si numere reale
check(7 // 2 == 3 and 7 % 3 == 1 and 2 ^ 10 == 1024.0, "aritmetica")
check(math.type(1) == "integer" and math.type(1.5) == "float", "math.type")
check(string.format("%.3f", math.pi) == "3.142", "format %f")
check(math.abs(math.sqrt(2) ^ 2 - 2) < 1e-12, "sqrt")
check(tonumber("0x1F") == 31 and tonumber("2.5e3") == 2500.0, "tonumber")
check(math.maxinteger + 1 == math.mininteger, "intregi pe 64 de biti")

-- siruri si tipare
check(("YokaiOS"):upper() == "YOKAIOS" and ("abc"):rep(2, "-") == "abc-abc", "string")
check(select(3, ("unu doi trei"):find("doi")) == nil and ("unu doi trei"):find("doi") == 5, "find")
check(("a=1, b=22"):gsub("%d+", function(d) return "<" .. d .. ">" end) == "a=<1>, b=<22>", "gsub")
local words = {}
for w in ("pipe redirect fork"):gmatch("%a+") do words[#words + 1] = w end
check(#words == 3 and words[2] == "redirect", "gmatch")

-- tabele, sortare, closure-uri, metatabele
local t = {5, 3, 9, 1}
table.sort(t)
check(table.concat(t, ",") == "1,3,5,9", "table.sort")
local function counter()
  local n = 0
  return function() n = n + 1; return n end
end
local c = counter(); c(); c()
check(c() == 3, "closure")
local V = setmetatable({}, {__index = function(_, k) return k * 2 end})
check(V[21] == 42, "__index")

-- corutine si erori
local co = coroutine.wrap(function(a)
  local b = coroutine.yield(a + 1)
  return b * 10
end)
check(co(1) == 2 and co(4) == 40, "coroutine")
local ok, err = pcall(error, {cod = 7})
check(not ok and err.cod == 7, "pcall")

-- fisiere: /tmp e tmpfs
local path = "/tmp/selftest-lua.txt"
local f = assert(io.open(path, "w"))
f:write("linia 1\n", "linia ", 2, "\n")
f:close()
local lines = {}
for l in io.lines(path) do lines[#lines + 1] = l end
check(#lines == 2 and lines[2] == "linia 2", "io.lines")
check(os.remove(path), "os.remove")
check(io.open(path, "r") == nil, "fisier sters")

-- mediu si procese: io.popen trece prin `ysh -c`
check(os.getenv("PATH") ~= nil, "os.getenv")
local p = assert(io.popen("echo din popen | wc -w"))
local s = p:read("a")
p:close()
check(s:match("^%s*2%s*$") ~= nil, "io.popen")
check(os.time() > 0 and type(os.clock()) == "number", "os.time")

if failed > 0 then
  print(string.format("selftest.lua: %d din %d verificari au esuat", failed, checks))
  os.exit(1)
end
print("selftest.lua: ok")
