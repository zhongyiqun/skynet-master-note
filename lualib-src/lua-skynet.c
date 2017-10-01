#define LUA_LIB

#include "skynet.h"
#include "lua-seri.h"

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

struct snlua {
	lua_State * L;
	struct skynet_context * ctx;
	const char * preload;
};

//lua中调用C函数时出错处理函数
static int
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);	//获得第一个参数的字符串信息，错误信息
	if (msg)
		luaL_traceback(L, L, msg, 1);	//以错误消息为唯一参数, 获取当前的栈回溯信息, 将此消息附加到栈回溯信息之前. 返回这个结果.
	else {
		lua_pushliteral(L, "(no error message)");	//如果没有错误信息就返回"(no error message)"
	}
	return 1;
}

/***************************
函数功能：调用lua中注册的消息处理回调函数，分发消息处理函数需要释放掉msg内容
lua调用时需要传入的参数：
返回值：返回值的数量：0
	分发消息的处理函数dispatch_message需要释放掉msg内容
***************************/
static int
_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	lua_State *L = ud;			//获得lua环境
	int trace = 1;
	int r;
	int top = lua_gettop(L);	//获得栈中元素的数量
	if (top == 0) {
		lua_pushcfunction(L, traceback);		//将traceback函数入栈
		lua_rawgetp(L, LUA_REGISTRYINDEX, _cb);	//将registry[_cb]中注册的函数入栈
	} else {
		assert(top == 2);
	}
	lua_pushvalue(L,2);			//将栈上的第二个元素（注册函数）的副本入栈

	lua_pushinteger(L, type);	//将消息类型栈
	lua_pushlightuserdata(L, (void *)msg);	//将消息内容入栈
	lua_pushinteger(L,sz);			//将消息内容的大小入栈
	lua_pushinteger(L, session);	//将消息的session入栈
	lua_pushinteger(L, source);		//将消息源信息入栈

	r = lua_pcall(L, 5, 0 , trace);	//调用registry[_cb]中注册的函数，输入上面入栈的5个参数，出错则调用函数traceback进行处理

	if (r == LUA_OK) {
		return 0;
	}
	const char * self = skynet_command(context, "REG", NULL);	//获得":0x服务编号"形式的服务handle字符串
	switch (r) {
	case LUA_ERRRUN:	//运行时错误
		skynet_error(context, "lua call [%x to %s : %d msgsz = %d] error : " KRED "%s" KNRM, source , self, session, sz, lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:	//内存分配错误。对于这种错，Lua 不会调用错误处理函数。
		skynet_error(context, "lua memory error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRERR:	//在运行错误处理函数时发生的错误。
		skynet_error(context, "lua error in error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRGCMM:	//在运行 __gc 元方法时发生的错误。 （这个错误和被调用的函数无关。）
		skynet_error(context, "lua gc error : [%x to %s : %d]", source , self, session);
		break;
	};

	lua_pop(L,1);

	return 0;
}

/***************************
函数功能：调用lua中注册的消息处理回调函数，分发消息处理函数不需要释放掉msg内容
lua调用时需要传入的参数：
返回值：返回值的数量：1
	分发消息的处理函数dispatch_message不需要释放掉msg内容
***************************/
static int
forward_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	_cb(context, ud, type, session, source, msg, sz);
	// don't delete msg in forward mode.
	return 1;
}

/***************************
函数功能：在lua中注册消息处理的回调函数
lua调用时需要传入的参数：
	1）注册的函数，2）分发消息处理函数释放需要释放消息内容
返回值：返回值的数量：0
	lua中无返回值
***************************/
static int
lcallback(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息指针
	int forward = lua_toboolean(L, 2);		//获得第二个参数
	luaL_checktype(L,1,LUA_TFUNCTION);		//检测函数的第一个参数类型是否是LUA_TFUNCTION即lua函数
	lua_settop(L,1);						//将栈的元素设置只有一个
	lua_rawsetp(L, LUA_REGISTRYINDEX, _cb);	//将registry[_cb] = 传入的第一个参数callback函数

	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);		//将当前lua状态机的主线程入栈
	lua_State *gL = lua_tothread(L,-1);		//获得栈顶的lua线程

	if (forward) {
		skynet_callback(context, gL, forward_cb);	//注册服务中的回调函数和回调对象
	} else {
		skynet_callback(context, gL, _cb);		//注册服务中的回调函数和回调对象
	}

	return 0;
}

/***************************
函数功能：通过传入的第一个参数作为命令，第二个参数为命令对应的函数的参数，调用对应的函数
lua调用时需要传入的参数：
	1）命令字符串cmd，2）参数parm默认为NULL
返回值：返回值的数量：1
	1）将调用的函数结果入栈，字符串类型
***************************/
static int
lcommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息
	const char * cmd = luaL_checkstring(L,1);	//检查第一个参数是否字符串，并获得该字符串
	const char * result;
	const char * parm = NULL;
	if (lua_gettop(L) == 2) {		//检查是否有两个参数
		parm = luaL_checkstring(L,2);	//检查第二个参数是否字符串，并获得该字符串
	}

	result = skynet_command(context, cmd, parm);	//以第一个参数为命令调用对应的函数，并将第二个参数作为调用函数的参数传入
	if (result) {
		lua_pushstring(L, result);		//将调用的函数结果入栈
		return 1;
	}
	return 0;
}

/***************************
函数功能：通过传入的第一个参数作为命令，第二个参数为命令对应的函数的参数，调用对应的函数
lua调用时需要传入的参数：
	1）命令字符串cmd，2）参数parm默认为NULL，该参数可以是数字
返回值：返回值的数量：1
	1）将调用的函数结果入栈，该结果是实数类型的
***************************/
static int
lintcommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息
	const char * cmd = luaL_checkstring(L,1);		//检查函数的第一个参数是否为字符串，并获得字符串
	const char * result;
	const char * parm = NULL;
	char tmp[64];	// for integer parm
	if (lua_gettop(L) == 2) {		//判断函数的参数是否为两个
		if (lua_isnumber(L, 2)) {	//如果第二个参数为数字
			int32_t n = (int32_t)luaL_checkinteger(L,2);	//将函数第二个参数转换为整数形式
			sprintf(tmp, "%d", n);
			parm = tmp;		//第二个参数的字符串形式
		} else {
			parm = luaL_checkstring(L,2);	//获得第二个参数的字符串形式
		}
	}

	result = skynet_command(context, cmd, parm);	//调用cmd中的字符串对应的注册函数，参数为parm
	if (result) {
		char *endptr = NULL; 
		lua_Integer r = strtoll(result, &endptr, 0);
		if (endptr == NULL || *endptr != '\0') {
			// may be real number
			double n = strtod(result, &endptr);
			if (endptr == NULL || *endptr != '\0') {
				return luaL_error(L, "Invalid result %s", result);
			} else {
				lua_pushnumber(L, n);	//将浮点数类型的结果压入栈
			}
		} else {
			lua_pushinteger(L, r);		//将整数结果压入栈
		}
		return 1;
	}
	return 0;
}

/***************************
函数功能：分配一个消息的session
lua调用时需要传入的参数：无
返回值：返回值的数量：1
	1）将session压入栈
***************************/
static int
lgenid(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息
	int session = skynet_send(context, 0, 0, PTYPE_TAG_ALLOCSESSION , 0 , NULL, 0);		//分配一个消息的session，但不会发送消息
	lua_pushinteger(L, session);	//将session压入栈
	return 1;
}

//获得栈中指定index位置的值并转换为字符串
static const char *
get_dest_string(lua_State *L, int index) {
	const char * dest_string = lua_tostring(L, index);	//获得栈中指定index位置的值并转换为字符串
	if (dest_string == NULL) {
		luaL_error(L, "dest address type (%s) must be a string or number.", lua_typename(L, lua_type(L,index)));
	}
	return dest_string;		//返回字符串
}

/***************************
函数功能：发送消息
参数：L：对应的lua虚拟机环境，
	  source：
	  idx_type：消息类型参数的在栈中的位置
lua调用时需要传入的参数：
	1）消息的目的服务handle或服务名，2）消息类型，3）消息中的session参数，为nil则系统分配，
	4）消息的内容，5）如果消息内容为LUA_TLIGHTUSERDATA类型则需要该参数，该参数为消息内容的长度
返回值：返回值的数量：1
	1）将session入栈
***************************/
static int
send_message(lua_State *L, int source, int idx_type) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));	//获取服务信息
	uint32_t dest = (uint32_t)lua_tointeger(L, 1);	//获得第一个参数，消息的目的服务handle
	const char * dest_string = NULL;
	if (dest == 0) {		//如果第一个参数不是数字
		if (lua_type(L,1) == LUA_TNUMBER) {
			return luaL_error(L, "Invalid service address 0");
		}
		dest_string = get_dest_string(L, 1);	//获得第一个参数，消息的目的服务名
	}

	int type = luaL_checkinteger(L, idx_type+0);	//获得第二个参数，即消息类型
	int session = 0;
	if (lua_isnil(L,idx_type+1)) {			//第三个参数是nil时返回1，否则返回0
		type |= PTYPE_TAG_ALLOCSESSION;		//消息类型中标记消息的session需要系统分配
	} else {
		//获取第三个参数，即为用于请求回应，即标记回应的消息是对应哪一条发送消息进行回应，0表示不需要回应
		session = luaL_checkinteger(L,idx_type+1);		
	}

	int mtype = lua_type(L,idx_type+2);	//获取第四个参数的类型
	switch (mtype) {
	case LUA_TSTRING: {			//字符串
		size_t len = 0;
		void * msg = (void *)lua_tolstring(L,idx_type+2,&len);	//获得第四个参数的内容，即发送的数据，以及数据的长度
		if (len == 0) {
			msg = NULL;
		}
		if (dest_string) {
			//发送消息，提供的是目标服务名
			session = skynet_sendname(context, source, dest_string, type, session , msg, len);
		} else {
			//发送消息，提供的是目标服务handle
			session = skynet_send(context, source, dest, type, session , msg, len);
		}
		break;
	}
	case LUA_TLIGHTUSERDATA: {		//轻量用户数据
		void * msg = lua_touserdata(L,idx_type+2);	//获得第四个参数的内容
		int size = luaL_checkinteger(L,idx_type+3);	//获得第四个参数的长度
		if (dest_string) {
			//发送消息，提供的是目标服务名
			session = skynet_sendname(context, source, dest_string, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		} else {
			//发送消息，提供的是目标服务handle
			session = skynet_send(context, source, dest, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		}
		break;
	}
	default:
		luaL_error(L, "invalid param %s", lua_typename(L, lua_type(L,idx_type+2)));
	}
	if (session < 0) {
		// send to invalid address
		// todo: maybe throw an error would be better
		return 0;
	}
	lua_pushinteger(L,session);		//将session入栈
	return 1;
}

/*
	uint32 address
	 string address
	integer type
	integer session
	string message
	 lightuserdata message_ptr
	 integer len
 */
/***************************
函数功能：发送不需要回复的消息内容，因为source为0
lua调用时需要传入的参数：
	1）消息的目的服务handle或服务名，2）消息类型，3）消息中的session参数，为nil则系统分配，
	4）消息的内容，5）如果消息内容为LUA_TLIGHTUSERDATA类型则需要该参数，该参数为消息内容的长度
返回值：返回值的数量：1
	1）将session入栈
***************************/
static int
lsend(lua_State *L) {
	return send_message(L, 0, 2);
}

/*
	uint32 address
	 string address
	integer source_address
	integer type
	integer session
	string message
	 lightuserdata message_ptr
	 integer len
 */
/***************************
函数功能：发送带有消息源source的消息内容
lua调用时需要传入的参数：
	1）消息的目的服务handle或服务名，2）消息源source，3）消息类型，4）消息中的session参数，为nil则系统分配，
	5）消息的内容，6）如果消息内容为LUA_TLIGHTUSERDATA类型则需要该参数，该参数为消息内容的长度
返回值：返回值的数量：1
	1）将session入栈
***************************/
static int
lredirect(lua_State *L) {
	uint32_t source = (uint32_t)luaL_checkinteger(L,2);
	return send_message(L, source, 3);
}

/***************************
函数功能：将error信息输出到log
lua调用时需要传入的参数：
	将所以的参数串联成字符串的形式输出到log
返回值：返回值的数量：0
	将所以的参数串联成字符串入栈
***************************/
static int
lerror(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));		//获得服务信息
	int n = lua_gettop(L);		//获得函数的参数数量
	if (n <= 1) {	//只有一个参数，直接输出错误信息
		lua_settop(L, 1);		//设置栈中只有一个参数
		const char * s = luaL_tolstring(L, 1, NULL);	//获得第一个参数的字符串形式，并且会入栈
		skynet_error(context, "%s", s);			//输出第一个参数的信息到log
		return 0;
	}
	//当有多个参数的时候，错误信息的处理
	luaL_Buffer b;		
	luaL_buffinit(L, &b);	//初始化缓存
	int i;
	for (i=1; i<=n; i++) {
		luaL_tolstring(L, i, NULL);		//将参数一个个转换为字符串的形式压入栈
		luaL_addvalue(&b);		//弹出栈顶值添加到缓存
		if (i<n) {
			luaL_addchar(&b, ' ');	//加入缓存的参数之间加入空格
		}
	}
	luaL_pushresult(&b);	//将缓存的值压入栈
	skynet_error(context, "%s", lua_tostring(L, -1));	//将栈顶的信息输出到log
	return 0;
}

/***************************
函数功能：将指定的数据copy入栈
lua调用时需要传入的参数：
	1）数据内容，2）数据的大小
返回值：返回值的数量：1
	1）数据的副本入栈
***************************/
static int
ltostring(lua_State *L) {
	if (lua_isnoneornil(L,1)) {	//如果第一个参数为nil返回1否则返回0
		return 0;
	}
	char * msg = lua_touserdata(L,1);	//获得第一参数指针
	int sz = luaL_checkinteger(L,2);	//检查第二个参数是否为整数，并获得第二个参数，即为第一个参数中数据的大小
	lua_pushlstring(L,msg,sz);			//将第一个参数的数据副本入栈
	return 1;
}

/***************************
函数功能：输入一个服务的handle，获得该服务的节点号以及判断该节点是否本地节点
lua调用时需要传入的参数：
	1）一个服务的handle
返回值：返回值的数量为：2
	1）输入服务handle的节点号，2）该节点是否是本地节点
***************************/
static int
lharbor(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息
	uint32_t handle = (uint32_t)luaL_checkinteger(L,1);		//检查第一个参数是否为整数，并获得第一个参数，即为服务handle
	int harbor = 0;
	int remote = skynet_isremote(context, handle, &harbor);	//判断handle是否为本地节点，是返回true，harbor存储节点编号
	lua_pushinteger(L,harbor);		//将节点编号入栈
	lua_pushboolean(L, remote);		//将判断结果入栈

	return 2;
}

static int
lpackstring(lua_State *L) {
	luaseri_pack(L);
	char * str = (char *)lua_touserdata(L, -2);
	int sz = lua_tointeger(L, -1);
	lua_pushlstring(L, str, sz);
	skynet_free(str);
	return 1;
}

/***************************
函数功能：释放轻量用户数据
lua调用时需要传入的参数：
	1）轻量用户数据
	2）轻量用户数据的大小
返回值：返回值的数量为：0
***************************/
static int
ltrash(lua_State *L) {
	int t = lua_type(L,1);		//获得第一个参数的类型
	switch (t) {
	case LUA_TSTRING: {
		break;
	}
	case LUA_TLIGHTUSERDATA: {	//如果是轻量用户数据
		void * msg = lua_touserdata(L,1);	//获得数据的内容
		luaL_checkinteger(L,2);		//检查第二个参数是否为整数，并获得
		skynet_free(msg);	//释放轻量用户数据
		break;
	}
	default:
		luaL_error(L, "skynet.trash invalid param %s", lua_typename(L,t));
	}

	return 0;
}

/***************************
函数功能：获得系统的当前时间
lua调用时需要传入的参数：无
返回值：返回值的数量为：1
	将时间压入栈
***************************/
static int
lnow(lua_State *L) {
	uint64_t ti = skynet_now();
	lua_pushinteger(L, ti);
	return 1;
}

LUAMOD_API int
luaopen_skynet_core(lua_State *L) {
	luaL_checkversion(L);	//检查是否有创建lua虚拟机

	luaL_Reg l[] = {
		{ "send" , lsend },
		{ "genid", lgenid },
		{ "redirect", lredirect },
		{ "command" , lcommand },
		{ "intcommand", lintcommand },
		{ "error", lerror },
		{ "tostring", ltostring },
		{ "harbor", lharbor },
		{ "pack", luaseri_pack },
		{ "unpack", luaseri_unpack },
		{ "packstring", lpackstring },
		{ "trash" , ltrash },
		{ "callback", lcallback },
		{ "now", lnow },
		{ NULL, NULL },
	};

	luaL_newlibtable(L, l);		//创建一张足够存l的表

	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");		//将registry["skynet_context"]，即服务信息入栈
	struct skynet_context *ctx = lua_touserdata(L,-1);		//获得栈顶的服务信息
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");
	}

	luaL_setfuncs(L,l,1);		//注册l中的函数到表中，并且所有函数都共享一个服务信息。

	return 1;
}
