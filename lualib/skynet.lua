-- read https://github.com/cloudwu/skynet/wiki/FAQ for the module "skynet.core"
local c = require "skynet.core"
local tostring = tostring
local tonumber = tonumber
local coroutine = coroutine
local assert = assert
local pairs = pairs
local pcall = pcall
local table = table

local profile = require "skynet.profile"

local coroutine_resume = profile.resume 		--协程运行
local coroutine_yield = profile.yield 			--协程挂起

--可以通过协议名获得协议的信息，也可以通过协议id获得
local proto = {}		--包含：name协议类型名，id协议id，pack打包函数，unpack解包函数

local skynet = {		--协议类型，即上面的协议id，即消息类型
	-- read skynet.h
	PTYPE_TEXT = 0,
	PTYPE_RESPONSE = 1,
	PTYPE_MULTICAST = 2,
	PTYPE_CLIENT = 3,
	PTYPE_SYSTEM = 4,
	PTYPE_HARBOR = 5,
	PTYPE_SOCKET = 6,
	PTYPE_ERROR = 7,
	PTYPE_QUEUE = 8,	-- used in deprecated mqueue, use skynet.queue instead
	PTYPE_DEBUG = 9,
	PTYPE_LUA = 10,
	PTYPE_SNAX = 11,
}

-- code cache
skynet.cache = require "skynet.codecache"

-----注册协议信息
function skynet.register_protocol(class)
	local name = class.name
	local id = class.id
	assert(proto[name] == nil and proto[id] == nil)
	assert(type(name) == "string" and type(id) == "number" and id >=0 and id <=255)
	proto[name] = class 	--通过协议名存储协议的信息
	proto[id] = class 		--通过协议id存储协议的信息
end

local session_id_coroutine = {}		--以session为键，记录消息返回响应对应的发生消息session，调用的处理协同程序
local session_coroutine_id = {}		--技能协程处理的响应消息session
local session_coroutine_address = {}
local session_response = {}
local unresponse = {}

local wakeup_queue = {}
local sleep_session = {}			--以协程句柄为键，等待该消息session响应的协程，即记录处于睡眠的协程

local watching_service = {}
local watching_session = {}
local dead_service = {}
local error_queue = {}
local fork_queue = {}			--协程队列

-- suspend is function
local suspend

--将字符串"0x+服务编号"，转换成数字形式的服务编号
local function string_to_handle(str)
	return tonumber("0x" .. string.sub(str , 2))
end

----- monitor exit

local function dispatch_error_queue()
	local session = table.remove(error_queue,1)
	if session then
		local co = session_id_coroutine[session]
		session_id_coroutine[session] = nil
		return suspend(co, coroutine_resume(co, false))
	end
end

local function _error_dispatch(error_session, error_source)
	if error_session == 0 then
		-- service is down
		--  Don't remove from watching_service , because user may call dead service
		if watching_service[error_source] then
			dead_service[error_source] = true
		end
		for session, srv in pairs(watching_session) do
			if srv == error_source then
				table.insert(error_queue, session)
			end
		end
	else
		-- capture an error for error_session
		if watching_session[error_session] then
			table.insert(error_queue, error_session)
		end
	end
end

-- coroutine reuse

local coroutine_pool = setmetatable({}, { __mode = "kv" }) --协同程序对象池

--首先从协同程序对象池中取并运行如果没有则创建
local function co_create(f)
	local co = table.remove(coroutine_pool)	--取出协同程序对象池中的最后一个
	if co == nil then 	--如果没有则创建一个
		co = coroutine.create(function(...)
			f(...)
			while true do
				f = nil
				coroutine_pool[#coroutine_pool+1] = co 	--该协程运行时将其句柄添加到协程池里
				f = coroutine_yield "EXIT"
				f(coroutine_yield())
			end
		end)
	else
		coroutine_resume(co, f)  --运行
	end
	return co
end

local function dispatch_wakeup()
	local co = table.remove(wakeup_queue,1)
	if co then
		local session = sleep_session[co]
		if session then
			session_id_coroutine[session] = "BREAK"
			return suspend(co, coroutine_resume(co, false, "BREAK"))
		end
	end
end

local function release_watching(address)
	local ref = watching_service[address]
	if ref then
		ref = ref - 1
		if ref > 0 then
			watching_service[address] = ref
		else
			watching_service[address] = nil
		end
	end
end

-- suspend is local function
function suspend(co, result, command, param, size)
	if not result then
		local session = session_coroutine_id[co]
		if session then -- coroutine may fork by others (session is nil)
			local addr = session_coroutine_address[co]
			if session ~= 0 then
				-- only call response error
				c.send(addr, skynet.PTYPE_ERROR, session, "")
			end
			session_coroutine_id[co] = nil
			session_coroutine_address[co] = nil
		end
		error(debug.traceback(co,tostring(command)))
	end
	if command == "CALL" then
		session_id_coroutine[param] = co
	elseif command == "SLEEP" then 		--唤起睡眠的协程
		session_id_coroutine[param] = co
		sleep_session[co] = param
	elseif command == "RETURN" then
		local co_session = session_coroutine_id[co]
		if co_session == 0 then
			if size ~= nil then
				c.trash(param, size)
			end
			return suspend(co, coroutine_resume(co, false))	-- send don't need ret
		end
		local co_address = session_coroutine_address[co]
		if param == nil or session_response[co] then
			error(debug.traceback(co))
		end
		session_response[co] = true
		local ret
		if not dead_service[co_address] then
			ret = c.send(co_address, skynet.PTYPE_RESPONSE, co_session, param, size) ~= nil
			if not ret then
				-- If the package is too large, returns nil. so we should report error back
				c.send(co_address, skynet.PTYPE_ERROR, co_session, "")
			end
		elseif size ~= nil then
			c.trash(param, size)
			ret = false
		end
		return suspend(co, coroutine_resume(co, ret))
	elseif command == "RESPONSE" then
		local co_session = session_coroutine_id[co]   	--通过协程句柄获得对应的session
		local co_address = session_coroutine_address[co] 	--通过协程句柄获得消息发送源source
		if session_response[co] then
			error(debug.traceback(co))
		end
		local f = param
		local function response(ok, ...)
			if ok == "TEST" then
				if dead_service[co_address] then
					release_watching(co_address)
					unresponse[response] = nil
					f = false
					return false
				else
					return true
				end
			end
			if not f then
				if f == false then
					f = nil
					return false
				end
				error "Can't response more than once"
			end

			local ret
			-- do not response when session == 0 (send)
			if co_session ~= 0 and not dead_service[co_address] then
				if ok then
					ret = c.send(co_address, skynet.PTYPE_RESPONSE, co_session, f(...)) ~= nil
					if not ret then
						-- If the package is too large, returns false. so we should report error back
						c.send(co_address, skynet.PTYPE_ERROR, co_session, "")
					end
				else
					ret = c.send(co_address, skynet.PTYPE_ERROR, co_session, "") ~= nil
				end
			else
				ret = false
			end
			release_watching(co_address)
			unresponse[response] = nil
			f = nil
			return ret
		end
		watching_service[co_address] = watching_service[co_address] + 1
		session_response[co] = true
		unresponse[response] = true
		return suspend(co, coroutine_resume(co, response))
	elseif command == "EXIT" then
		-- coroutine exit
		local address = session_coroutine_address[co]
		if address then
			release_watching(address)
			session_coroutine_id[co] = nil
			session_coroutine_address[co] = nil
			session_response[co] = nil
		end
	elseif command == "QUIT" then
		-- service exit
		return
	elseif command == "USER" then
		-- See skynet.coutine for detail
		error("Call skynet.coroutine.yield out of skynet.coroutine.resume\n" .. debug.traceback(co))
	elseif command == nil then
		-- debug trace
		return
	else
		error("Unknown command : " .. command .. "\n" .. debug.traceback(co))
	end
	dispatch_wakeup()
	dispatch_error_queue()
end

--定时ti(单位为1/100秒)执行函数func
function skynet.timeout(ti, func)
	local session = c.intcommand("TIMEOUT",ti)
	assert(session)
	local co = co_create(func)		--创建协程
	assert(session_id_coroutine[session] == nil)  --判断是否为新的session，用于接收消息响应时，定位到是响应哪一条消息，由发送消息的服务生成
	session_id_coroutine[session] = co 		--记录下新协程的句柄
end

--睡眠ti(单位为1/100秒)等待协程被唤起
function skynet.sleep(ti)
	local session = c.intcommand("TIMEOUT",ti)		--发送计时
	assert(session)
	local succ, ret = coroutine_yield("SLEEP", session)
	sleep_session[coroutine.running()] = nil		--协程已经唤起，则将表中的协程信息置空
	if succ then
		return
	end
	if ret == "BREAK" then
		return "BREAK"
	else
		error(ret)
	end
end

--挂起协程
function skynet.yield()
	return skynet.sleep(0)
end

--等待指定协程唤起
function skynet.wait(co)
	local session = c.genid() 	--分配一个消息的session
	local ret, msg = coroutine_yield("SLEEP", session)
	co = co or coroutine.running()
	sleep_session[co] = nil
	session_id_coroutine[session] = nil
end

local self_handle
function skynet.self()	--获得本服务的服务编号handle
	if self_handle then
		return self_handle
	end
	self_handle = string_to_handle(c.command("REG"))
	return self_handle
end

--根据服务名获得服务编号的数字形式
function skynet.localname(name)
	local addr = c.command("QUERY", name)
	if addr then
		return string_to_handle(addr)
	end
end

skynet.now = c.now 		--获得系统的当前时间， 精确到1/100秒

local starttime
--获得开始时间，精确到秒
function skynet.starttime()
	if not starttime then
		starttime = c.intcommand("STARTTIME")
	end
	return starttime
end

--获得系统当前的UTC时间，精确到秒
function skynet.time()
	return skynet.now()/100 + (starttime or skynet.starttime())
end

function skynet.exit()
	fork_queue = {}	-- no fork coroutine can be execute after skynet.exit 将协程队列清空
	skynet.send(".launcher","lua","REMOVE",skynet.self(), false)
	-- report the sources that call me
	for co, session in pairs(session_coroutine_id) do
		local address = session_coroutine_address[co]
		if session~=0 and address then
			c.send(address, skynet.PTYPE_ERROR, session, "")
		end
	end
	for resp in pairs(unresponse) do
		resp(false)
	end
	-- report the sources I call but haven't return
	local tmp = {}
	for session, address in pairs(watching_session) do
		tmp[address] = true
	end
	for address in pairs(tmp) do
		c.send(address, skynet.PTYPE_ERROR, 0, "")
	end
	c.command("EXIT")
	-- quit service
	coroutine_yield "QUIT"
end

--获取lua全局变量key的值
function skynet.getenv(key)
	return (c.command("GETENV",key))
end

--设置lua全局变量key的值为value
function skynet.setenv(key, value)
	assert(c.command("GETENV",key) == nil, "Can't setenv exist key : " .. key)
	c.command("SETENV",key .. " " ..value)
end

--发送经过打包的消息，该消息的session参数为0表示不需要回复，参数如下，
--1）消息的目的服务handle或服务名，可以为".+服务名"或者":0x服务编号"，2）消息类型
--3）之后的参数都为消息内容
function skynet.send(addr, typename, ...)
	local p = proto[typename]
	return c.send(addr, p.id, 0 , p.pack(...))
end

--发生未经过打包的消息
function skynet.rawsend(addr, typename, msg, sz)
	local p = proto[typename]
	return c.send(addr, p.id, 0 , msg, sz)
end

skynet.genid = assert(c.genid)	--分配一个消息的session，为lua-skynet.c中的lgenid函数

skynet.redirect = function(dest,source,typename,...)	--发送带有消息源source的消息内容函数，为lua-skynet.c中的lredirect函数
	return c.redirect(dest, source, proto[typename].id, ...)
end

skynet.pack = assert(c.pack)	--打包函数为lua-seri.c中的luaseri_pack函数
skynet.packstring = assert(c.packstring)	--打包字符串的函数为lua-skynet.c中的lpackstring函数
skynet.unpack = assert(c.unpack)	--解包函数为lua-seri.c中的luaseri_unpack函数
skynet.tostring = assert(c.tostring) 	--转换为字符串函数，为lua-skynet.c中的ltostring函数
skynet.trash = assert(c.trash)	--释放轻量用户数据，为lua-skynet.c中的ltrash函数


--把当前协程添加到观察列表，挂起协程，等待协程唤起
local function yield_call(service, session)
	watching_session[session] = service 		--观察的消息session对应的服务handle或服务名
	local succ, msg, sz = coroutine_yield("CALL", session)	--挂起协程，给coroutine_resume传入的参数为"CALL", session
	watching_session[session] = nil 		--清空观察列表
	if not succ then
		error "call failed"
	end
	return msg,sz
end

--给指定的服务发生消息，消息中的session参数为系统分配，
--addr为消息的目的服务handle或服务名，可以为".+服务名"或者":0x服务编号"，
--typename为消息的类型，可以为名字也可以为id，
--之后的参数为消息内容
function skynet.call(addr, typename, ...)
	local p = proto[typename]
	local session = c.send(addr, p.id , nil , p.pack(...)) --向指定服务发生消息
	if session == nil then
		error("call to invalid address " .. skynet.address(addr))
	end
	return p.unpack(yield_call(addr, session))
end

function skynet.rawcall(addr, typename, msg, sz)
	local p = proto[typename]
	local session = assert(c.send(addr, p.id , nil , msg, sz), "call to invalid address")
	return yield_call(addr, session)
end

function skynet.ret(msg, sz)
	msg = msg or ""
	return coroutine_yield("RETURN", msg, sz)
end

--挂起协程，传入 "RESPONSE", pack
function skynet.response(pack)
	pack = pack or skynet.pack  	--打包函数为lua-seri.c中的luaseri_pack函数
	return coroutine_yield("RESPONSE", pack)  --挂起协程，传入 "RESPONSE", pack
end

function skynet.retpack(...)
	return skynet.ret(skynet.pack(...))
end

function skynet.wakeup(co)
	if sleep_session[co] then
		table.insert(wakeup_queue, co)
		return true
	end
end

--修改指定 typename 类型的协议注册 dispatch 函数为func
--func为nil返回指定 typename 类型的协议是否有 dispatch 函数
function skynet.dispatch(typename, func)
	local p = proto[typename]	--获得协议信息
	if func then
		local ret = p.dispatch
		p.dispatch = func
		return ret
	else
		return p and p.dispatch
	end
end

local function unknown_request(session, address, msg, sz, prototype)
	skynet.error(string.format("Unknown request (%s): %s", prototype, c.tostring(msg,sz)))
	error(string.format("Unknown session : %d from %x", session, address))
end

function skynet.dispatch_unknown_request(unknown)
	local prev = unknown_request
	unknown_request = unknown
	return prev
end

local function unknown_response(session, address, msg, sz)
	skynet.error(string.format("Response message : %s" , c.tostring(msg,sz)))
	error(string.format("Unknown session : %d from %x", session, address))
end

function skynet.dispatch_unknown_response(unknown)
	local prev = unknown_response
	unknown_response = unknown
	return prev
end

--创建协程，并将协程句柄插入表fork_queue，
--协程运行时会调用创建时传入的函数func，以及传入的参数
function skynet.fork(func,...)
	local args = table.pack(...)
	local co = co_create(function()
		func(table.unpack(args,1,args.n))
	end)
	table.insert(fork_queue, co)
	return co
end

--[[
函数功能：服务的消息处理函数，根据消息类型，分为需要回应的消息和不需要回应的消息处理
参数：
	1）prototype消息类型，即skynet.h中定义的，2）msg消息内容，
	3）sz消息大小，4）session对应回应哪条消息，5）source消息源
返回值：无返回值
]]
local function raw_dispatch_message(prototype, msg, sz, session, source)
	-- skynet.PTYPE_RESPONSE = 1, read skynet.h
	if prototype == 1 then --回应包消息类型 消息类型为 skynet.PTYPE_RESPONSE 的消息
		local co = session_id_coroutine[session] 	--获得处理回应消息的协程
		if co == "BREAK" then
			session_id_coroutine[session] = nil
		elseif co == nil then
			unknown_response(session, source, msg, sz)
		else
			session_id_coroutine[session] = nil
			suspend(co, coroutine_resume(co, true, msg, sz))
		end
	else 	--不需要回应的消息
		local p = proto[prototype]	--获得该消息类型的协议信息
		if p == nil then
			if session ~= 0 then
				c.send(source, skynet.PTYPE_ERROR, session, "")
			else
				unknown_request(session, source, msg, sz, prototype)
			end
			return
		end
		local f = p.dispatch 		--获得协议中的 dispatch 函数
		if f then
			local ref = watching_service[source]
			if ref then
				watching_service[source] = ref + 1
			else
				watching_service[source] = 1
			end
			local co = co_create(f)		--创建一个协程，该协程运行时调用f函数
			session_coroutine_id[co] = session  --将该协程和对应的session关联
			session_coroutine_address[co] = source 	--将该协程和消息发送源source关联
			suspend(co, coroutine_resume(co, session,source, p.unpack(msg,sz))) --先运行上面创建的协程传入的参数为 session,source, p.unpack(msg,sz)
		elseif session ~= 0 then
			c.send(source, skynet.PTYPE_ERROR, session, "")
		else
			unknown_request(session, source, msg, sz, proto[prototype].name)
		end
	end
end

--[[
函数功能：服务的消息处理回调函数
参数：
	1）prototype消息类型，即skynet.h中定义的，2）msg消息内容，
	3）sz消息大小，4）session对应回应哪条消息，5）source消息源
返回值：无返回值
]]
function skynet.dispatch_message(...)
	local succ, err = pcall(raw_dispatch_message,...) 	--调用函数raw_dispatch_message
	while true do
		local key,co = next(fork_queue)
		if co == nil then
			break
		end
		fork_queue[key] = nil
		local fork_succ, fork_err = pcall(suspend,co,coroutine_resume(co))
		if not fork_succ then
			if succ then
				succ = false
				err = tostring(fork_err)
			else
				err = tostring(err) .. "\n" .. tostring(fork_err)
			end
		end
	end
	assert(succ, tostring(err))
end

--通过服务"launcher"启动一个snlua服务，服务名为name
function skynet.newservice(name, ...)
	return skynet.call(".launcher", "lua" , "LAUNCH", "snlua", name, ...)
end

function skynet.uniqueservice(global, ...)
	if global == true then
		return assert(skynet.call(".service", "lua", "GLAUNCH", ...))
	else
		return assert(skynet.call(".service", "lua", "LAUNCH", global, ...))
	end
end

function skynet.queryservice(global, ...)
	if global == true then
		return assert(skynet.call(".service", "lua", "GQUERY", ...))
	else
		return assert(skynet.call(".service", "lua", "QUERY", global, ...))
	end
end

--获得服务编号的字符串形式
function skynet.address(addr)
	if type(addr) == "number" then
		return string.format(":%08x",addr)
	else
		return tostring(addr)
	end
end

--输入一个服务节点编号，获得该服务的节点号以及判断该节点是否非本地节点，true表示是非本地节点
function skynet.harbor(addr)
	return c.harbor(addr)
end

skynet.error = c.error 	--将error信息输出到log

----- register protocol
-----注册协议
do
	local REG = skynet.register_protocol

	REG {
		name = "lua",
		id = skynet.PTYPE_LUA,
		pack = skynet.pack,
		unpack = skynet.unpack,
	}

	REG {
		name = "response",
		id = skynet.PTYPE_RESPONSE,
	}

	REG {
		name = "error",
		id = skynet.PTYPE_ERROR,
		unpack = function(...) return ... end,
		dispatch = _error_dispatch,
	}
end

local init_func = {}

function skynet.init(f, name)
	assert(type(f) == "function")
	if init_func == nil then
		f()
	else
		table.insert(init_func, f)
		if name then
			assert(type(name) == "string")
			assert(init_func[name] == nil)
			init_func[name] = f
		end
	end
end

local function init_all()
	local funcs = init_func
	init_func = nil
	if funcs then
		for _,f in ipairs(funcs) do
			f()
		end
	end
end

local function ret(f, ...)
	f()
	return ...
end

local function init_template(start, ...)
	init_all()
	init_func = {}
	return ret(init_all, start(...))
end

--调用start函数
function skynet.pcall(start, ...)
	return xpcall(init_template, debug.traceback, start, ...)
end

--初始化服务，调用函数开始start，
--成功则发送消息类型为"lua",给服务".launcher"，内容为"LAUNCHOK"
function skynet.init_service(start)
	local ok, err = skynet.pcall(start)
	if not ok then
		skynet.error("init service failed: " .. tostring(err))
		skynet.send(".launcher","lua", "ERROR")		--发送消息类型为"lua",给服务".launcher"，内容为"ERROR"
		skynet.exit()
	else
		skynet.send(".launcher","lua", "LAUNCHOK")	--发送消息类型为"lua",给服务".launcher"，内容为"LAUNCHOK"
	end
end

--注册服务处理消息的回调函数，初始化服务，调用函数start_func
function skynet.start(start_func)
	c.callback(skynet.dispatch_message)		--注册服务的回调函数
	skynet.timeout(0, function() 			--定时执行函数，此处定时的时间为0
		skynet.init_service(start_func) 	--初始化服务，调用函数start_func
	end)
end

--该服务是否陷入死循环
function skynet.endless()
	return (c.intcommand("STAT", "endless") == 1)
end

--获得服务队列中消息队列的长度
function skynet.mqlen()
	return c.intcommand("STAT", "mqlen")
end

--获得指定服务的一些状态信息
--what为"mqlen"，获得服务队列中消息队列的长度
--what为"endless"，该服务是否陷入死循环
--what为"cpu"，获得该服务消耗CPU时间，整数部分为秒，小数部分精确到微秒
--what为"time"，获得距离该服务最近一条消息处理开始的时间间隔
--what为"message"，该服务已经处理消息的数量
function skynet.stat(what)
	return c.intcommand("STAT", what)
end

function skynet.task(ret)
	local t = 0
	for session,co in pairs(session_id_coroutine) do
		if ret then
			ret[session] = debug.traceback(co)
		end
		t = t + 1
	end
	return t
end

function skynet.term(service)
	return _error_dispatch(0, service)
end

function skynet.memlimit(bytes)
	debug.getregistry().memlimit = bytes
	skynet.memlimit = nil	-- set only once
end

-- Inject internal debug framework
local debug = require "skynet.debug"
debug.init(skynet, {
	dispatch = skynet.dispatch_message,
	suspend = suspend,
})

return skynet
