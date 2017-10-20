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

//获得本线程到当前代码系统CPU花费的时间 精确到秒
static double
get_time() {
#if  !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);	//本线程到当前代码系统CPU花费的时间

	int sec = ti.tv_sec & 0xffff;
	int nsec = ti.tv_nsec;

	return (double)sec + (double)nsec / NANOSEC;	//精确到秒
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

//获得当前CPU时间与start CPU时间之间的时间间隔
static inline double 
diff_time(double start) {
	double now = get_time();
	if (now < start) {
		return now + 0x10000 - start;
	} else {
		return now - start;
	}
}

/***************************
函数功能：开始初始化，记录下CPU时间在第一个upvalue值中t1[LUA_TTHREAD]=ti，
		初始化第二个upvalue值t2[LUA_TTHREAD]=0，该值是记录总时间
	
返回值：成功返回0
***************************/
static int
lstart(lua_State *L) {
	if (lua_gettop(L) != 0) {	//如果栈中的元素数量不为0
		lua_settop(L,1);	//设置栈中的元素数量为1
		luaL_checktype(L, 1, LUA_TTHREAD);	//检查第一个参数的类型
	} else {
		lua_pushthread(L);	//将当前lua状态机的主线程入栈
	}
	lua_pushvalue(L, 1);	// push coroutine	将当前lua状态机的主线程的副本入栈
	lua_rawget(L, lua_upvalueindex(2));		//将第二个upvalue值即t2[LUA_TTHREAD]值入栈
	if (!lua_isnil(L, -1)) {	//如果值不为nil
		return luaL_error(L, "Thread %p start profile more than once", lua_topointer(L, 1));
	}
	lua_pushvalue(L, 1);	// push coroutine 将当前lua状态机的主线程的副本入栈
	lua_pushnumber(L, 0);	//将数字0入栈
	lua_rawset(L, lua_upvalueindex(2));		//设置第二个upvalue值即t2[LUA_TTHREAD]=0

	lua_pushvalue(L, 1);	// push coroutine	将当前lua状态机的主线程的副本入栈
	double ti = get_time();	//本线程到当前代码系统CPU花费的时间 精确到秒
#ifdef DEBUG_LOG
	fprintf(stderr, "PROFILE [%p] start\n", L);
#endif
	lua_pushnumber(L, ti);
	lua_rawset(L, lua_upvalueindex(1));		//设置第一个upvalue值即t1[LUA_TTHREAD]=ti

	return 0;
}

/***************************
函数功能：线程结束处理，将t1[LUA_TTHREAD]和t1[LUA_TTHREAD]置为nil，
		计算该线程从开始到结束的CPU总耗时时间
	
返回值：返回值的数量为1
	1）该线程从开始到结束的CPU总耗时时间
***************************/
static int
lstop(lua_State *L) {
	if (lua_gettop(L) != 0) {	//栈中的元素数量不为0
		lua_settop(L,1);	//设置栈中元素数量为1
		luaL_checktype(L, 1, LUA_TTHREAD);	//检查栈中第一个元素的类型
	} else {
		lua_pushthread(L);	//将当前lua状态机的主线程入栈
	}
	lua_pushvalue(L, 1);	// push coroutine 将当前lua状态机的主线程的副本入栈
	lua_rawget(L, lua_upvalueindex(1));	//将第一个upvalue值即t1[LUA_TTHREAD]值入栈
	if (lua_type(L, -1) != LUA_TNUMBER) {
		return luaL_error(L, "Call profile.start() before profile.stop()");
	} 
	double ti = diff_time(lua_tonumber(L, -1));	//获得当前CPU时间和开始CPU时间之间的时间间隔
	lua_pushvalue(L, 1);	// push coroutine 将当前lua状态机的主线程的副本入栈
	lua_rawget(L, lua_upvalueindex(2));		//将第二个upvalue值即t2[LUA_TTHREAD]值入栈
	double total_time = lua_tonumber(L, -1);	//获得第二个upvalue值即t2[LUA_TTHREAD]值

	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));		//设置第一个upvalue值即t1[LUA_TTHREAD]值为nil

	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(2));		//设置第二个upvalue值即t2[LUA_TTHREAD]值为nil

	total_time += ti;		//该线程CUP的总运行时间
	lua_pushnumber(L, total_time);
#ifdef DEBUG_LOG
	fprintf(stderr, "PROFILE [%p] stop (%lf/%lf)\n", lua_tothread(L,1), ti, total_time);
#endif

	return 1;
}

/***************************
函数功能：每次运行协程前，如果需要统计时间，则记录下运行前的时间
	
返回值：coroutine.resume函数的值
***************************/
static int
timing_resume(lua_State *L) {
	lua_pushvalue(L, -1);	//将栈顶的副本入栈 即协程的句柄
	lua_rawget(L, lua_upvalueindex(2));		//将第二个upvalue值即t2[协程的句柄]值入栈
	if (lua_isnil(L, -1)) {		// check total time 如果为nil
		lua_pop(L,2);	// pop from coroutine 出栈
	} else {
		lua_pop(L,1);
		double ti = get_time();		//获得本线程到当前代码系统CPU花费的时间 精确到秒
#ifdef DEBUG_LOG
		fprintf(stderr, "PROFILE [%p] resume %lf\n", lua_tothread(L, -1), ti);
#endif
		lua_pushnumber(L, ti);		//将时间入栈
		lua_rawset(L, lua_upvalueindex(1));	// set start time 设置第一个upvalue值即t1[协程的句柄]值为ti，即开始时间
	}

	lua_CFunction co_resume = lua_tocfunction(L, lua_upvalueindex(3)); 	//coroutine.resume

	return co_resume(L);	//运行coroutine.resume，即运行协程
}

/***************************
函数功能：每次运行协程前，如果需要统计时间，则记录下运行前的时间
	
返回值：coroutine.resume函数的值
***************************/
static int
lresume(lua_State *L) {
	lua_pushvalue(L,1);		//将栈低的副本入栈 即协程的句柄
	
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

/***************************
函数功能：将注册了函数的表入栈，并设置注册函数的upvalue值
	
返回值：返回值的数量：1
***************************/
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
	lua_setupvalue(L, -2, 3);				//设置闭包即注册函数"resume"对应的函数upvalue值的第三个值为coroutine.resume
	lua_pop(L,1);							

	lua_getfield(L, libtable, "resume_co");	//将注册函数"resume_co"对应的函数指针入栈
	lua_pushcfunction(L, co_resume);		//将函数co_resume入栈
	lua_setupvalue(L, -2, 3);				//设置闭包即注册函数"resume_co"对应的函数upvalue值的第三个值为coroutine.resume
	lua_pop(L,1);

	lua_getfield(L, -1, "yield");			//将coroutine.yield的值压入栈

	lua_CFunction co_yield = lua_tocfunction(L, -1);	//将coroutine.yield转换为C函数
	if (co_yield == NULL)
		return luaL_error(L, "Can't get coroutine.yield");
	lua_pop(L,1);	//出栈

	lua_getfield(L, libtable, "yield");		//将注册函数"yield"对应的函数指针入栈
	lua_pushcfunction(L, co_yield);			//将函数co_yield入栈
	lua_setupvalue(L, -2, 3);				//设置闭包即注册函数"yield"对应的函数upvalue值的第三个值为coroutine.yield
	lua_pop(L,1);

	lua_getfield(L, libtable, "yield_co");	//将注册函数"yield_co"对应的函数指针入栈
	lua_pushcfunction(L, co_yield);			//将函数co_yield入栈
	lua_setupvalue(L, -2, 3);				//设置闭包即注册函数"yield_co"对应的函数upvalue值的第三个值为coroutine.yield
	lua_pop(L,1);

	lua_settop(L, libtable);				//将注册了函数的表的栈的索引位置设置为栈顶

	return 1;
}
