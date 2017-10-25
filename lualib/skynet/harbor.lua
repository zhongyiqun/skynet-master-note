local skynet = require "skynet"

local harbor = {}

function harbor.globalname(name, handle)
	handle = handle or skynet.self()	--获得本服务的服务编号handle
	skynet.send(".cslave", "lua", "REGISTER", name, handle)--给服务名为"cslave"的服务发生内容为"REGISTER", name, handle的消息
end

function harbor.queryname(name)
	return skynet.call(".cslave", "lua", "QUERYNAME", name)
end

function harbor.link(id)
	skynet.call(".cslave", "lua", "LINK", id)
end

function harbor.connect(id)
	skynet.call(".cslave", "lua", "CONNECT", id)
end

function harbor.linkmaster()
	skynet.call(".cslave", "lua", "LINKMASTER")
end

return harbor
