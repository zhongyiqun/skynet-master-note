#include "skynet.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MEMORY_WARNING_REPORT (1024 * 1024 * 32)	//32M

struct snlua {
	lua_State * L;					//lua虚拟机
	struct skynet_context * ctx;	//服务信息
	size_t mem;
	size_t mem_report;
	size_t mem_limit;
};

// LUA_CACHELIB may defined in patched lua for shared proto
#ifdef LUA_CACHELIB

#define codecache luaopen_cache

#else

static int
cleardummy(lua_State *L) {
  return 0;
}

static int 
codecache(lua_State *L) {
	luaL_Reg l[] = {
		{ "clear", cleardummy },
		{ "mode", cleardummy },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);		//新建一个表，并把表l中的函数注册进去
	lua_getglobal(L, "loadfile");	//把全局变量"loadfile"里的值压栈，返回该值的类型。
	lua_setfield(L, -2, "loadfile");	//l["loadfile"]="loadfile"里的值
	return 1;
}

#endif

static int 
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);	//将栈低的值赋值给msg
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

//向服务名为"launcher"的服务发送内容为"ERROR"的消息
static void
report_launcher_error(struct skynet_context *ctx) {
	// sizeof "ERROR" == 5
	skynet_sendname(ctx, 0, ".launcher", PTYPE_TEXT, 0, "ERROR", 5);
}

//从全局lua环境中获取键key的值，如果该键对应的值为NULL，则返回str
static const char *
optstring(struct skynet_context *ctx, const char *key, const char * str) {
	const char * ret = skynet_command(ctx, "GETENV", key);	//从全局lua环境中获取键key的值
	if (ret == NULL) {
		return str;
	}
	return ret;
}

static int
init_cb(struct snlua *l, struct skynet_context *ctx, const char * args, size_t sz) {
	lua_State *L = l->L;
	l->ctx = ctx;
	lua_gc(L, LUA_GCSTOP, 0);		//停止垃圾收集器
	lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");	//设置registry["LUA_NOENV"] = 1
	luaL_openlibs(L);	//打开所有lua标准库
	lua_pushlightuserdata(L, ctx);		//将本服务信息指针压入栈
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");	//设置registry["skynet_context"] = ctx
	luaL_requiref(L, "skynet.codecache", codecache , 0);	//如果"skynet.codecache"不在package.loaded中，则调用函数openf，并传入字符串"skynet.codecache"
												//将其返回值置入package.loaded["skynet.codecache"]
	lua_pop(L,1);		//弹出栈顶值，此时栈现在为空

	//从全局lua环境中获取键"lua_path"的值，如果该键对应的值为NULL，则返回"./lualib/?.lua;./lualib/?/init.lua"
	const char *path = optstring(ctx, "lua_path","./lualib/?.lua;./lualib/?/init.lua");
	lua_pushstring(L, path);	//将path压入栈
	lua_setglobal(L, "LUA_PATH");	//该服务所在的lua环境中的全局变量LUA_PATH = path，并将path从栈中弹出 LUA_PATH存的是lua写的skynet库
	const char *cpath = optstring(ctx, "lua_cpath","./luaclib/?.so");
	lua_pushstring(L, cpath);
	lua_setglobal(L, "LUA_CPATH");	//全局变量LUA_CPATH = cpath  LUA_CPATH存的是C写的skynet提供给lua调用的库
	const char *service = optstring(ctx, "luaservice", "./service/?.lua");
	lua_pushstring(L, service);
	lua_setglobal(L, "LUA_SERVICE");	//全局变量LUA_SERVICE = service  LUA_SERVICE存的是lua写的服务模块
	const char *preload = skynet_command(ctx, "GETENV", "preload");		//获取preload全局变量的值，如果配置文件进行了配置则返回配置中的字符串，否则为NULL
	lua_pushstring(L, preload);
	lua_setglobal(L, "LUA_PRELOAD");	//默认全局变量LUA_PRELOAD = NULL

	lua_pushcfunction(L, traceback);
	assert(lua_gettop(L) == 1);			//判断栈中元素的个数是否等于1

	const char * loader = optstring(ctx, "lualoader", "./lualib/loader.lua");

	int r = luaL_loadfile(L,loader);	//加载"./lualib/loader.lua"文件的代码，将其入栈
	if (r != LUA_OK) {
		skynet_error(ctx, "Can't load %s : %s", loader, lua_tostring(L, -1));	//输出log
		report_launcher_error(ctx);		//向服务名为"launcher"的服务发送内容为"ERROR"的消息
		return 1;
	}
	lua_pushlstring(L, args, sz);	//将args压入栈，此处args的内容为"bootstrap"
	r = lua_pcall(L,1,0,1);		//运行加载的"./lualib/loader.lua"代码，传入参数为“bootstrap”，没有返回值，错误是调用traceback函数处理
	if (r != LUA_OK) {
		skynet_error(ctx, "lua loader error : %s", lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}
	lua_settop(L,0);		//清除栈
	if (lua_getfield(L, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER) {	//将
		size_t limit = lua_tointeger(L, -1);
		l->mem_limit = limit;
		skynet_error(ctx, "Set memory limit to %.2f M", (float)limit / (1024 * 1024));
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "memlimit");
	}
	lua_pop(L, 1);			//弹出栈顶值

	lua_gc(L, LUA_GCRESTART, 0);	//重启垃圾收集器

	return 0;
}

//该服务的消息处理回调函数，该函数只在初始化完后回调一次，以后的消息处理都不会再调用本函数
//
static int
launch_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz) {
	assert(type == 0 && session == 0);
	struct snlua *l = ud;
	skynet_callback(context, NULL, NULL);	//清除注册的该服务的回调函数和回调的对象
	int err = init_cb(l, context, msg, sz);	//msg = "bootstrap"
	if (err) {
		skynet_command(context, "EXIT", NULL);
	}

	return 0;
}

//初始化snlua服务实例l，ctx为服务信息，agrs="bootstrap"，这个参数用于调用相应的lua脚本bootstrap.lua
//注册服务的回调函数，以及向自己发送一条消息，说明初始化完后调用回调函数launch_cb
int
snlua_init(struct snlua *l, struct skynet_context *ctx, const char * args) {
	int sz = strlen(args);
	char * tmp = skynet_malloc(sz);
	memcpy(tmp, args, sz);
	skynet_callback(ctx, l , launch_cb);	//注册服务的回调函数和回调的对象
	const char * self = skynet_command(ctx, "REG", NULL);	//执行"REG"指令对应的函数，返回":0x服务编号"
	uint32_t handle_id = strtoul(self+1, NULL, 16);		//将16进制的服务编号转换为无符号整数
	// it must be first message
	skynet_send(ctx, 0, handle_id, PTYPE_TAG_DONTCOPY,0, tmp, sz);	//向服务自己发送消息  内容为："bootstrap"
	return 0;
}

//内存分配函数
static void *
lalloc(void * ud, void *ptr, size_t osize, size_t nsize) {
	struct snlua *l = ud;
	size_t mem = l->mem;
	l->mem += nsize;
	if (ptr)
		l->mem -= osize;
	if (l->mem_limit != 0 && l->mem > l->mem_limit) {
		if (ptr == NULL || nsize > osize) {
			l->mem = mem;
			return NULL;
		}
	}
	if (l->mem > l->mem_report) {
		l->mem_report *= 2;
		skynet_error(l->ctx, "Memory warning %.2f M", (float)l->mem / (1024 * 1024));
	}
	return skynet_lalloc(ptr, osize, nsize);
}

//创建一个snlua服务实例
struct snlua *
snlua_create(void) {
	struct snlua * l = skynet_malloc(sizeof(*l));	//创建一个snlua服务实例
	memset(l,0,sizeof(*l));
	l->mem_report = MEMORY_WARNING_REPORT;
	l->mem_limit = 0;
	l->L = lua_newstate(lalloc, l);			//创建lua虚拟机
	return l;
}

void
snlua_release(struct snlua *l) {
	lua_close(l->L);
	skynet_free(l);
}

void
snlua_signal(struct snlua *l, int signal) {
	skynet_error(l->ctx, "recv a signal %d", signal);
	if (signal == 0) {
#ifdef lua_checksig
	// If our lua support signal (modified lua version by skynet), trigger it.
	skynet_sig_L = l->L;
#endif
	} else if (signal == 1) {
		skynet_error(l->ctx, "Current Memory %.3fK", (float)l->mem / 1024);
	}
}
