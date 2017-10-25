local skynet = require "skynet"
local harbor = require "skynet.harbor"
require "skynet.manager"	-- import skynet.launch, ...
local memory = require "skynet.memory"

skynet.start(function()
	local sharestring = tonumber(skynet.getenv "sharestring" or 4096)
	memory.ssexpand(sharestring)

	local standalone = skynet.getenv "standalone"	--获得配置中standalone的内容

	local launcher = assert(skynet.launch("snlua","launcher"))	--启动服务动态库为snlua的服务，传入参数launcher，服务初始化完后调用脚本launcher.lua
	skynet.name(".launcher", launcher)

	local harbor_id = tonumber(skynet.getenv "harbor" or 0)	--获得配置中配置的节点号
	if harbor_id == 0 then						--如果节点号没有配置或者配置为0
		assert(standalone ==  nil)				--配置中standalone也必须没有配置
		standalone = true
		skynet.setenv("standalone", "true")		--设置standalone为true

		local ok, slave = pcall(skynet.newservice, "cdummy")
		if not ok then
			skynet.abort()	--将全局服务信息结构中的所有服务信息删除
		end
		skynet.name(".cslave", slave)

	else		--节点号不为0
		if standalone then 			--standalone不为nil或false
			if not pcall(skynet.newservice,"cmaster") then
				skynet.abort()	--将全局服务信息结构中的所有服务信息删除
			end
		end

		local ok, slave = pcall(skynet.newservice, "cslave")
		if not ok then
			skynet.abort()	--将全局服务信息结构中的所有服务信息删除
		end
		skynet.name(".cslave", slave)
	end

	if standalone then
		local datacenter = skynet.newservice "datacenterd"
		skynet.name("DATACENTER", datacenter)
	end
	skynet.newservice "service_mgr"
	pcall(skynet.newservice,skynet.getenv "start" or "main")
	skynet.exit()
end)
