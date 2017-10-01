local args = {}
for word in string.gmatch(..., "%S+") do
	table.insert(args, word)
end

SERVICE_NAME = args[1]	--"bootstrap"

local main, pattern

local err = {}
for pat in string.gmatch(LUA_SERVICE, "([^;]+);*") do 		--获得每个";"之间的字符串，如果没有";"则是整个字符串
	local filename = string.gsub(pat, "?", SERVICE_NAME)	--将字符串中的"?"替换成"bootstrap"
	local f, msg = loadfile(filename)		--加载"./service/bootstrap.lua"文件
	if not f then
		table.insert(err, msg)
	else
		pattern = pat  			--"./service/?.lua"
		main = f  				--加载的代码块
		break
	end
end

if not main then
	error(table.concat(err, "\n"))
end

LUA_SERVICE = nil
package.path , LUA_PATH = LUA_PATH   	--lualib中lua文件的文件
package.cpath , LUA_CPATH = LUA_CPATH   --luaclib中C写的skynet提供给lua调用的库

local service_path = string.match(pattern, "(.*/)[^/?]+$")		--./service/

if service_path then
	service_path = string.gsub(service_path, "?", args[1])
	package.path = service_path .. "?.lua;" .. package.path 	--./service/?.lua;./lualib/?.lua;./lualib/?/init.lua
	SERVICE_PATH = service_path 	--./service/
else
	local p = string.match(pattern, "(.*/).+$")
	SERVICE_PATH = p
end

if LUA_PRELOAD then
	local f = assert(loadfile(LUA_PRELOAD))
	f(table.unpack(args))
	LUA_PRELOAD = nil
end

main(select(2, table.unpack(args)))		--运行"./service/bootstrap.lua"文件
