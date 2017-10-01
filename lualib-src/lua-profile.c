#define LUA_LIB

#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>

#include <time.h>

#if defined(__APPLE__)
#include <mach/task.h>
#include <mach/mach.h>
#endif

#define NANOSEC 1000000000
#define MICROSEC 1000000

// #define DEBUG_LOG

static double
get_time() {
#if  !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	int sec = ti.tv_sec & 0xffff;
	int nsec = ti.tv_nsec;

	return (double)sec + (double)nsec / NANOSEC;	
#else
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t )&aTaskInfo, &aTaskInfoCount)) {
		return 0;
	}

	int sec = aTaskInfo.user_time.seconds & 0xffff;
	int msec = aTaskInfo.user_time.microseconds;

	return (double)sec + (double)msec / MICROSEC;
#endif
}

static inline double 
diff_time(double start) {
	double now = get_time();
	if (now < start) {
		return now + 0x10000 - start;
	} else {
		return now - start;
	}
}

static int
lstart(lua_State *L) {
	if (lua_gettop(L) != 0) {
		lua_settop(L,1);
		luaL_checktype(L, 1, LUA_TTHREAD);
	} else {
		lua_pushthread(L);
	}
	lua_pushvalue(L, 1);	// push coroutine
	lua_rawget(L, lua_upvalueindex(2));
	if (!lua_isnil(L, -1)) {
		return luaL_error(L, "Thread %p start profile more than once", lua_topointer(L, 1));
	}
	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnumber(L, 0);
	lua_rawset(L, lua_upvalueindex(2));

	lua_pushvalue(L, 1);	// push coroutine
	double ti = get_time();
#ifdef DEBUG_LOG
	fprintf(stderr, "PROFILE [%p] start\n", L);
#endif
	lua_pushnumber(L, ti);
	lua_rawset(L, lua_upvalueindex(1));

	return 0;
}

static int
lstop(lua_State *L) {
	if (lua_gettop(L) != 0) {
		lua_settop(L,1);
		luaL_checktype(L, 1, LUA_TTHREAD);
	} else {
		lua_pushthread(L);
	}
	lua_pushvalue(L, 1);	// push coroutine
	lua_rawget(L, lua_upvalueindex(1));
	if (lua_type(L, -1) != LUA_TNUMBER) {
		return luaL_error(L, "Call profile.start() before profile.stop()");
	} 
	double ti = diff_time(lua_tonumber(L, -1));
	lua_pushvalue(L, 1);	// push coroutine
	lua_rawget(L, lua_upvalueindex(2));
	double total_time = lua_tonumber(L, -1);

	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));

	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(2));

	total_time += ti;
	lua_pushnumber(L, total_time);
#ifdef DEBUG_LOG
	fprintf(stderr, "PROFILE [%p] stop (%lf/%lf)\n", lua_tothread(L,1), ti, total_time);
#endif

	return 1;
}

static int
timing_resume(lua_State *L) {
	lua_pushvalue(L, -1);	//将栈顶的副本入栈
	lua_rawget(L, lua_upvalueindex(2));		//将第二个upvalue值
	if (lua_isnil(L, -1)) {		// check total time
		lua_pop(L,2);	// pop from coroutine
	} else {
		lua_pop(L,1);
		double ti = get_time();
#ifdef DEBUG_LOG
		fprintf(stderr, "PROFILE [%p] resume %lf\n", lua_tothread(L, -1), ti);
#endif
		lua_pushnumber(L, ti);
		lua_rawset(L, lua_upvalueindex(1));	// set start time
	}

	lua_CFunction co_resume = lua_tocfunction(L, lua_upvalueindex(3));

	return co_resume(L);
}

static int
lresume(lua_State *L) {
	lua_pushvalue(L,1);		//将栈低的副本入栈
	
	return timing_resume(L);
}

static int
lresume_co(lua_State *L) {
	luaL_checktype(L, 2, LUA_TTHREAD);
	lua_rotate(L, 2, -1);	// 'from' coroutine rotate to the top(index -1)

	return timing_resume(L);
}

static int
timing_yield(lua_State *L) {
#ifdef DEBUG_LOG
	lua_State *from = lua_tothread(L, -1);
#endif
	lua_pushvalue(L, -1);
	lua_rawget(L, lua_upvalueindex(2));	// check total time
	if (lua_isnil(L, -1)) {
		lua_pop(L,2);
	} else {
		double ti = lua_tonumber(L, -1);
		lua_pop(L,1);

		lua_pushvalue(L, -1);	// push coroutine
		lua_rawget(L, lua_upvalueindex(1));
		double starttime = lua_tonumber(L, -1);
		lua_pop(L,1);

		double diff = diff_time(starttime);
		ti += diff;
#ifdef DEBUG_LOG
		fprintf(stderr, "PROFILE [%p] yield (%lf/%lf)\n", from, diff, ti);
#endif

		lua_pushvalue(L, -1);	// push coroutine
		lua_pushnumber(L, ti);
		lua_rawset(L, lua_upvalueindex(2));
		lua_pop(L, 1);	// pop coroutine
	}

	lua_CFunction co_yield = lua_tocfunction(L, lua_upvalueindex(3));

	return co_yield(L);
}

static int
lyield(lua_State *L) {
	lua_pushthread(L);

	return timing_yield(L);
}

static int
lyield_co(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTHREAD);
	lua_rotate(L, 1, -1);
	
	return timing_yield(L);
}

LUAMOD_API int
luaopen_skynet_profile(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "start", lstart },
		{ "stop", lstop },
		{ "resume", lresume },
		{ "yield", lyield },
		{ "resume_co", lresume_co },
		{ "yield_co", lyield_co },
		{ NULL, NULL },
	};
	luaL_newlibtable(L,l);			//创建一张可以存储l的表
	lua_newtable(L);	// table thread->start time 	创建一张空表，并将其压栈。假如名字为t1后面用
	lua_newtable(L);	// table thread->total time 	创建一张空表，并将其压栈。t2

	lua_newtable(L);	// weak table 					创建一张空表，并将其压栈。t3
	lua_pushliteral(L, "kv");		//将字符串"kv"压入栈
	lua_setfield(L, -2, "__mode");	//t3["__mode"] = "kv"

	lua_pushvalue(L, -1);			//拷贝表t3，压入栈顶
	lua_setmetatable(L, -3); 		//将表t3设置为t2的元表
	lua_setmetatable(L, -3);		//将表t3设置为t1的元表

	lua_pushnil(L);	// cfunction (coroutine.resume or coroutine.yield)	将一个空值入栈
	luaL_setfuncs(L,l,3);			//将l中的函数注册到表中，并且所有函数共享表t1，t2，nil值

	int libtable = lua_gettop(L);		//返回注册了函数的表的栈的索引位置

	lua_getglobal(L, "coroutine");		//将全局变量coroutine的值压入栈
	lua_getfield(L, -1, "resume");		//将coroutine.resume的值压入栈

	lua_CFunction co_resume = lua_tocfunction(L, -1);		//将coroutine.resume转换为C函数
	if (co_resume == NULL)
		return luaL_error(L, "Can't get coroutine.resume");
	lua_pop(L,1);		//出栈

	lua_getfield(L, libtable, "resume");	//将注册函数"resume"对应的函数指针入栈
	lua_pushcfunction(L, co_resume);		//将函数co_resume入栈
	lua_setupvalue(L, -2, 3);				//调用注册函数"resume"对应的函数，upvalue值为第三个值nil
	lua_pop(L,1);							

	lua_getfield(L, libtable, "resume_co");	//将注册函数"resume_co"对应的函数指针入栈
	lua_pushcfunction(L, co_resume);		//将函数co_resume入栈
	lua_setupvalue(L, -2, 3);				//调用注册函数"resume"对应的函数，upvalue值为第三个值nil
	lua_pop(L,1);

	lua_getfield(L, -1, "yield");

	lua_CFunction co_yield = lua_tocfunction(L, -1);	
	if (co_yield == NULL)
		return luaL_error(L, "Can't get coroutine.yield");
	lua_pop(L,1);

	lua_getfield(L, libtable, "yield");
	lua_pushcfunction(L, co_yield);
	lua_setupvalue(L, -2, 3);
	lua_pop(L,1);

	lua_getfield(L, libtable, "yield_co");
	lua_pushcfunction(L, co_yield);
	lua_setupvalue(L, -2, 3);
	lua_pop(L,1);

	lua_settop(L, libtable);

	return 1;
}
