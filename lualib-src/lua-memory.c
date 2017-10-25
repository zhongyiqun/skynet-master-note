#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>

#include "malloc_hook.h"
#include "luashrtbl.h"

/***************************
函数功能：获得所有服务分配的内存大小
lua调用时需要传入的参数：	无

返回值：返回值的数量：1
	1）所有服务分配的内存大小
***************************/
static int
ltotal(lua_State *L) {
	size_t t = malloc_used_memory();	//获得所有服务分配的内存大小
	lua_pushinteger(L, (lua_Integer)t);	//入栈

	return 1;
}

/***************************
函数功能：获得所有服务分配的内存块数
lua调用时需要传入的参数：	无

返回值：返回值的数量：1
	1）所有服务分配的内存块数
***************************/
static int
lblock(lua_State *L) {
	size_t t = malloc_memory_block();	//获得所有服务分配的内存块数
	lua_pushinteger(L, (lua_Integer)t);

	return 1;
}

/***************************
函数功能：以人类可读的方式向标准误 stderr 中输出当前的 jemalloc 统计信息
lua调用时需要传入的参数：	无

返回值：返回值的数量：0
	
***************************/
static int
ldumpinfo(lua_State *L) {
	memory_info_dump();

	return 0;
}

/***************************
函数功能：输出所有服务的内存分配信息
lua调用时需要传入的参数：	无

返回值：返回值的数量：0
	
***************************/
static int
ldump(lua_State *L) {
	dump_c_mem();

	return 0;
}

static int
lexpandshrtbl(lua_State *L) {
	int n = luaL_checkinteger(L, 1);
	luaS_expandshr(n);	//扩展共享字符串的大小
	return 0;
}

//获得当前服务的内存分配大小
static int
lcurrent(lua_State *L) {
	lua_pushinteger(L, malloc_current_memory());
	return 1;
}

LUAMOD_API int
luaopen_skynet_memory(lua_State *L) {
	luaL_checkversion(L);

	luaL_Reg l[] = {
		{ "total", ltotal },
		{ "block", lblock },
		{ "dumpinfo", ldumpinfo },
		{ "dump", ldump },
		{ "info", dump_mem_lua },
		{ "ssinfo", luaS_shrinfo },
		{ "ssexpand", lexpandshrtbl },
		{ "current", lcurrent },
		{ NULL, NULL },
	};

	luaL_newlib(L,l);	//创建一张新的表，并把列表 l 中的函数注册进去

	return 1;
}
