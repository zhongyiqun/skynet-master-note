#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"
#include "luashrtbl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}

static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}

static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

//将配置文件的键值对添加到lua全局信息内
static void
_init_env(lua_State *L) {
	lua_pushnil(L);  /* first key */
	while (lua_next(L, -2) != 0) {		//读加载的配置表中的键值对
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		const char * key = lua_tostring(L,-2);
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );		//设置boolean类型的键值对
		} else {
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);		//设置字符串类型的键值对
		}
		lua_pop(L,1);
	}
	lua_pop(L,1);
}

int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

static const char * load_config = "\
	local result = {}\n\
	local function getenv(name) return assert(os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\ 			//返回'/'
	local current_path = [[.]]..sep\n\				//当前路径为'./'
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\		//path="examples/",name=配置文件名
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\		//"./examples/"
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\		//打开配置文件
		local code = assert(f:read [[*a]])\n\					//读配置文件
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\		//替换$开头的环境变量
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\		//加载代码块
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\		//设置result的元表
	local config_name = ...\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";

int
main(int argc, char *argv[]) {
	const char * config_file = NULL ;
	if (argc > 1) {
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	luaS_initshr();		//初始化读写锁
	skynet_globalinit();	//初始化主线程
	skynet_env_init();		//初始化一个全局的Lua环境

	sigign();	//忽略SIGPIPE信号

	struct skynet_config config;

	struct lua_State *L = luaL_newstate();		//新建一个lua状态机
	luaL_openlibs(L);	// link lua lib

	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");	//加载load_config中的代码块
	assert(err == LUA_OK);
	lua_pushstring(L, config_file);		//将配置文件的路径压入栈

	err = lua_pcall(L, 1, 1, 0);		//运行加载的代码块，并将配置文件路径作为参数传入
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	_init_env(L);		//将配置文件的键值对添加到lua全局信息内

	config.thread =  optint("thread",8);
	config.module_path = optstring("cpath","./cservice/?.so");
	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);
	config.logservice = optstring("logservice", "logger");
	config.profile = optboolean("profile", 1);

	lua_close(L);		//消耗上面创建的lua状态机

	skynet_start(&config);		//skynet开始
	skynet_globalexit();
	luaS_exitshr();

	return 0;
}
