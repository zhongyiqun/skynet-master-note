local skynet = require "skynet"
local c = require "skynet.core"

--启动一个服务，返回服务编号
--第一个参数为服务的动态库名，第二个参数为传入的参数，第二个参数用于服务初始化完后调用相应的lua脚本
function skynet.launch(...)
	local addr = c.command("LAUNCH", table.concat({...}," "))	--启动一个服务，返回":+十六进制的服务编号"形式的字符串
	if addr then
		return tonumber("0x" .. string.sub(addr , 2))	--返回服务编号
	end
end

function skynet.kill(name)
	if type(name) == "number" then
		skynet.send(".launcher","lua","REMOVE",name, true)
		name = skynet.address(name)
	end
	c.command("KILL",name)
end

--将全局服务信息结构中的所有服务信息删除
function skynet.abort()
	c.command("ABORT")
end

--
local function globalname(name, handle)
	local c = string.sub(name,1,1)
	assert(c ~= ':')
	if c == '.' then
		return false
	end

	assert(#name <= 16)	-- GLOBALNAME_LENGTH is 16, defined in skynet_harbor.h
	assert(tonumber(name) == nil)	-- global name can't be number

	local harbor = require "skynet.harbor"

	harbor.globalname(name, handle)

	return true
end

function skynet.register(name)
	if not globalname(name) then
		c.command("REG", name)
	end
end

--设置指定服务的服务名，分为本地的和远程的
--本地的name为".+服务名"，handle为服务编号
--远程的name为"服务名"，handle为服务编号
function skynet.name(name, handle)
	if not globalname(name, handle) then
		c.command("NAME", name .. " " .. skynet.address(handle))	--设置指定服务的服务名
	end
end

local dispatch_message = skynet.dispatch_message

function skynet.forward_type(map, start_func)
	c.callback(function(ptype, msg, sz, ...)
		local prototype = map[ptype]
		if prototype then
			dispatch_message(prototype, msg, sz, ...)
		else
			dispatch_message(ptype, msg, sz, ...)
			c.trash(msg, sz)
		end
	end, true)
	skynet.timeout(0, function()
		skynet.init_service(start_func)
	end)
end

function skynet.filter(f ,start_func)
	c.callback(function(...)
		dispatch_message(f(...))
	end)
	skynet.timeout(0, function()
		skynet.init_service(start_func)
	end)
end

function skynet.monitor(service, query)
	local monitor
	if query then
		monitor = skynet.queryservice(true, service)
	else
		monitor = skynet.uniqueservice(true, service)
	end
	assert(monitor, "Monitor launch failed")
	c.command("MONITOR", string.format(":%08x", monitor))
	return monitor
end

return skynet
