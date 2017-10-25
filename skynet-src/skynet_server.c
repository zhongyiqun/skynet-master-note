#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"
#include "skynet_imp.h"
#include "skynet_log.h"
#include "skynet_timer.h"
#include "spinlock.h"
#include "atomic.h"

#include <pthread.h>

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
#define CHECKCALLING_END(ctx) spinlock_unlock(&ctx->calling);
#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
#define CHECKCALLING_DESTROY(ctx) spinlock_destroy(&ctx->calling);
#define CHECKCALLING_DECL struct spinlock calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DESTROY(ctx)
#define CHECKCALLING_DECL

#endif

//每个服务的信息存储结构体
struct skynet_context {
	void * instance;					//动态连接库的实例指针	即服务的实例指针
	struct skynet_module * mod;			//指定已加载的动态库信息
	void * cb_ud;						//服务回调的对象
	skynet_cb cb;						//服务的回调函数
	struct message_queue *queue;		//服务队列
	FILE * logfile;						//日志输出的文件
	uint64_t cpu_cost;	// in microsec	//消耗CUP时间	精确到微秒
	uint64_t cpu_start;	// in microsec	//本线程到当前代码系统CPU花费的时间
	char result[32];					//存储相应指令名执行的函数返回结果
	uint32_t handle;					//存储带有节点号的服务号
	int session_id;						//用于为消息分配一个session
	int ref;							//服务信息的引用计数
	int message_count;					//记录处理消息的数量
	bool init;							//服务是否初始化
	bool endless;						//标记服务是否陷入死循环
	bool profile;						//是否开启CPU耗时监测

	CHECKCALLING_DECL					//锁
};

struct skynet_node {
	int total;					//记录总的服务的数量
	int init;					//标记全局服务信息是否初始化
	uint32_t monitor_exit;
	pthread_key_t handle_key;	//与线程相关联的handle
	bool profile;	//是否开启CPU耗时监测 默认开启
};

static struct skynet_node G_NODE;

//获得总的服务数量
int 
skynet_context_total() {
	return G_NODE.total;
}

//递增记录总的服务数量
static void
context_inc() {
	ATOM_INC(&G_NODE.total);
}

//递减全局服务信息列表中总的服务数量
static void
context_dec() {
	ATOM_DEC(&G_NODE.total);
}

//获得当前线程处理的服务的服务编号
uint32_t 
skynet_current_handle(void) {
	if (G_NODE.init) {
		void * handle = pthread_getspecific(G_NODE.handle_key);
		return (uint32_t)(uintptr_t)handle;
	} else {
		uint32_t v = (uint32_t)(-THREAD_MAIN);
		return v;
	}
}

//将无符号整数id转换成":+16进制的整数"形式的字符串
static void
id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[9] = '\0';
}

struct drop_t {
	uint32_t handle;
};

//删除消息
static void
drop_message(struct skynet_message *msg, void *ud) {
	struct drop_t *d = ud;
	skynet_free(msg->data);
	uint32_t source = d->handle;
	assert(source);
	// report error to the message source
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}

//新建一个服务信息 name为服务的动态库名，param为传入的参数
struct skynet_context * 
skynet_context_new(const char * name, const char *param) {
	struct skynet_module * mod = skynet_module_query(name);		//获得指定文件名的动态连接库信息

	if (mod == NULL)
		return NULL;

	void *inst = skynet_module_instance_create(mod);	//调用相应动态库的库文件名_create的API函数
	if (inst == NULL)
		return NULL;
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));
	CHECKCALLING_INIT(ctx)

	ctx->mod = mod;				//服务对应的动态库信息
	ctx->instance = inst;		//对应的动态库_create的函数返回的指针
	ctx->ref = 2;				//服务信息的引用计数，初始化完后会减1
	ctx->cb = NULL;				//服务的回调函数
	ctx->cb_ud = NULL;			//服务回调的对象
	ctx->session_id = 0;		//用于为消息分配一个session
	ctx->logfile = NULL;		//日志输出的文件

	ctx->init = false;			//服务是否初始化
	ctx->endless = false;		//服务是否陷入死循环

	ctx->cpu_cost = 0;			//消耗CUP时间	精确到微秒
	ctx->cpu_start = 0;			//本线程到当前代码系统CPU花费的时间
	ctx->message_count = 0;		//记录处理消息的数量
	ctx->profile = G_NODE.profile;	//是否开启CPU耗时监测
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	ctx->handle = 0;	//存储带有节点号的服务号
	ctx->handle = skynet_handle_register(ctx);	//将服务信息存储到全局服务信息中，并产生一个定位服务的编号
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);		//创建服务队列
	// init function maybe use ctx->handle, so it must init at last
	context_inc();	//服务总数加1

	CHECKCALLING_BEGIN(ctx)		//尝试获得锁
	int r = skynet_module_instance_init(mod, inst, ctx, param);		//调用相应动态库的库文件名_init的API函数
	CHECKCALLING_END(ctx)		//释放锁
	if (r == 0) {
		struct skynet_context * ret = skynet_context_release(ctx);	//服务信息的引用计数减1
		if (ret) {
			ctx->init = true;		//标记服务初始化成功
		}
		skynet_globalmq_push(queue);		//将服务队列添加到全局队列
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;
	} else {
		skynet_error(ctx, "FAILED launch %s", name);
		uint32_t handle = ctx->handle;
		skynet_context_release(ctx);		//递减服务信息的引用计数，如果计数为0则释放
		skynet_handle_retire(handle);		//将指定的服务信息从全局的服务信息数字中剔除掉
		struct drop_t d = { handle };
		skynet_mq_release(queue, drop_message, &d);		//将服务队列释放
		return NULL;
	}
}

//为消息分配一个session
int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number
	int session = ++ctx->session_id;
	if (session <= 0) {
		ctx->session_id = 1;
		return 1;
	}
	return session;
}

//增加服务信息的引用计数
void 
skynet_context_grab(struct skynet_context *ctx) {
	ATOM_INC(&ctx->ref);
}

void
skynet_context_reserve(struct skynet_context *ctx) {
	skynet_context_grab(ctx);
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0 .
	// the reserved context will be release at last.
	context_dec();
}

//清除服务信息
static void 
delete_context(struct skynet_context *ctx) {
	if (ctx->logfile) {
		fclose(ctx->logfile);
	}
	skynet_module_instance_release(ctx->mod, ctx->instance);
	skynet_mq_mark_release(ctx->queue);
	CHECKCALLING_DESTROY(ctx)
	skynet_free(ctx);
	context_dec();
}

//递减服务信息的引用计数，如果计数为0则释放
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	if (ATOM_DEC(&ctx->ref) == 0) {
		delete_context(ctx);
		return NULL;
	}
	return ctx;
}

//将消息添加到handle对于的服务信息中的服务队列中
int
skynet_context_push(uint32_t handle, struct skynet_message *message) {
	struct skynet_context * ctx = skynet_handle_grab(handle);	//根据服务编号（包含节点号和服务号），获得服务信息
	if (ctx == NULL) {
		return -1;
	}
	skynet_mq_push(ctx->queue, message);	//将消息添加到服务队列
	skynet_context_release(ctx);	//递减服务信息的引用计数，如果计数为0则释放

	return 0;
}

//标记相应的服务陷入死循环
void 
skynet_context_endless(uint32_t handle) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	ctx->endless = true;	//标记为该服务陷入死循环
	skynet_context_release(ctx);	//递减服务信息的引用计数，如果计数为0则释放
}

//判断handle是否非本地节点，是返回true，harbor为节点编号
int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {
	int ret = skynet_harbor_message_isremote(handle);
	if (harbor) {
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}

//对分发的消息进行处理
static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	assert(ctx->init);
	CHECKCALLING_BEGIN(ctx)
	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));	//将handle与线程关联
	int type = msg->sz >> MESSAGE_TYPE_SHIFT;		//获得消息类型
	size_t sz = msg->sz & MESSAGE_TYPE_MASK;		//获得消息的大小
	if (ctx->logfile) {			//如果打开了日志文件，则将消息输出到日志文件
		skynet_log_output(ctx->logfile, msg->source, type, msg->session, msg->data, sz);
	}
	++ctx->message_count;	//记录处理消息的数量
	int reserve_msg;
	if (ctx->profile) {		//记录消耗CPU时间
		ctx->cpu_start = skynet_thread_time();
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);		//调用服务回调进行消息处理
		uint64_t cost_time = skynet_thread_time() - ctx->cpu_start;
		ctx->cpu_cost += cost_time;
	} else {
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);		//调用服务回调进行消息处理
	}
	if (!reserve_msg) {
		skynet_free(msg->data);
	}
	CHECKCALLING_END(ctx)
}

//处理服务中的所有消息
void 
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	struct skynet_message msg;
	struct message_queue *q = ctx->queue;
	while (!skynet_mq_pop(q,&msg)) {
		dispatch_message(ctx, &msg);
	}
}

//消息分发
struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	//如果服务队列q为空，则会从全局队列中去取
	if (q == NULL) {
		q = skynet_globalmq_pop();		//从全局队列中取出服务队列，并删除全局队列中的
		if (q==NULL)
			return NULL;
	}

	uint32_t handle = skynet_mq_handle(q);		//获取服务的handle

	struct skynet_context * ctx = skynet_handle_grab(handle);	//根据服务编号（包含节点号和服务号），获得服务信息，会增加服务信息的引用计数
	if (ctx == NULL) {			//如果服务信息为NULL，则释放服务队列信息
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
		return skynet_globalmq_pop();	//返回全局队列中的下一个服务队列
	}

	int i,n=1;
	struct skynet_message msg;

	for (i=0;i<n;i++) {
		if (skynet_mq_pop(q,&msg)) {	//从服务队列中取出消息
			skynet_context_release(ctx);	//递减服务信息的引用计数，如果计数为0则释放
			return skynet_globalmq_pop();	//本服务队列暂无消息，不会返回全局队列，返回下一个服务队列
		} else if (i==0 && weight >= 0) {
			n = skynet_mq_length(q);	//获得消息的数量
			n >>= weight;				//根据weight来决定线程本次处理服务队列中消息的数量
		}
		int overload = skynet_mq_overload(q);	//获得消息数量超出阈值时的消息数量，并清零记录值
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);		//将错误信息输出到logger服务
		}

		skynet_monitor_trigger(sm, msg.source , handle);	//记录消息源、目的地、version增1，用于监测线程监测该线程是否卡死与某条消息的处理

		if (ctx->cb == NULL) {		//如果服务没有注册回调函数则释放掉消息内容
			skynet_free(msg.data);
		} else {
			dispatch_message(ctx, &msg);	//有回调函数调用相应的回调函数进行处理
		}

		skynet_monitor_trigger(sm, 0,0);		//清除记录的消息源、目的地
	}

	assert(q == ctx->queue);
	struct message_queue *nq = skynet_globalmq_pop();	//取出下一个服务队列
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)
		skynet_globalmq_push(q);	//将当前处理的服务队列压入全局队列
		q = nq;
	} 
	skynet_context_release(ctx);	//递减服务信息的引用计数，如果计数为0则释放

	return q;
}

static void
copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH && addr[i];i++) {
		name[i] = addr[i];
	}
	for (;i<GLOBALNAME_LENGTH;i++) {
		name[i] = '\0';
	}
}

uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
		return strtoul(name+1,NULL,16);
	case '.':
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s",name);
	return 0;
}

//退出服务地址为 handle 的服务, 如果 handle 为 0 则退出 context 服务.
//此函数在存在服务退出监控的情况下, 会向其发送消息.
static void
handle_exit(struct skynet_context * context, uint32_t handle) {
	if (handle == 0) {
		handle = context->handle;
		skynet_error(context, "KILL self");
	} else {
		skynet_error(context, "KILL :%0x", handle);
	}
	if (G_NODE.monitor_exit) {
		skynet_send(context,  handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
	}
	skynet_handle_retire(handle);	//将指定的服务信息从全局的服务信息数字中剔除掉
}

// skynet command
// skynet命令
struct command_func {
	const char *name;	//命令名
	const char * (*func)(struct skynet_context * context, const char * param);	//命令名对应的函数
};

//添加一个定时节点，时间到后推送一条消息到相应的服务，该服务根据本函数返回的session本该消息进行处理，
//从而实现可定时执行某些操作
static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;
	int ti = strtol(param, &session_ptr, 10);
	int session = skynet_context_newsession(context);	//产生一个唯一的session
	skynet_timeout(context->handle, ti, session);
	sprintf(context->result, "%d", session);
	return context->result;
}

//param为NULL返回":0x服务编号"，否则为服务命名
static const char *
cmd_reg(struct skynet_context * context, const char * param) {
	if (param == NULL || param[0] == '\0') {
		sprintf(context->result, ":%x", context->handle);
		return context->result;		//返回":0x服务编号"
	} else if (param[0] == '.') {
		return skynet_handle_namehandle(context->handle, param + 1);	//插入为服务命名为参数param指定的名字，从param + 1开始
	} else {
		skynet_error(context, "Can't register global name %s in C", param);
		return NULL;
	}
}

//根据服务名获得":0x+服务编号"形式的服务编号
static const char *
cmd_query(struct skynet_context * context, const char * param) {
	if (param[0] == '.') {
		uint32_t handle = skynet_handle_findname(param+1);	//根据服务名查找定位服务的编号（包括节点号和服务号），没找到返回0
		if (handle) {
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

//设置指定服务的服务名，从参数中获得服务名和服务编号的字符串形式
//参数param的形式为：".+服务名 :+十六进制形式的服务编号"
static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size+1];
	char handle[size+1];
	sscanf(param,"%s %s",name,handle);
	if (handle[0] != ':') {		//handle中的第一个字符符为':'
		return NULL;
	}
	uint32_t handle_id = strtoul(handle+1, NULL, 16);
	if (handle_id == 0) {
		return NULL;
	}
	if (name[0] == '.') {
		return skynet_handle_namehandle(handle_id, name + 1);
	} else {
		skynet_error(context, "Can't set global name %s in C", name);
	}
	return NULL;
}

//退出当前服务
static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);	//退出当前服务
	return NULL;
}

//通过param参数获得服务编号，
//param可以是":+十六进制的服务编号"或者是".+服务名"形式
static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	if (param[0] == ':') {
		handle = strtoul(param+1, NULL, 16);
	} else if (param[0] == '.') {
		handle = skynet_handle_findname(param+1);
	} else {
		skynet_error(context, "Can't convert %s to handle",param);
	}

	return handle;
}

//kill掉指定的服务
//param可以是":+十六进制的服务编号"或者是".+服务名"形式
static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);	//获得param所指向的服务编号
	if (handle) {
		handle_exit(context, handle);	//退出服务
	}
	return NULL;
}

//启动一个param指定的服务，返回":+十六进制的服务编号"形式的字符串
//param参数的形式为：启动服务要加载的动态库名空格链接需要传入的参数，
static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char tmp[sz+1];
	strcpy(tmp,param);
	char * args = tmp;
	char * mod = strsep(&args, " \t\r\n");
	args = strsep(&args, "\r\n");
	struct skynet_context * inst = skynet_context_new(mod,args);
	if (inst == NULL) {
		return NULL;
	} else {
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

//获取lua全局变量param的值
static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

//设置lua全局变量的值
static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char key[sz+1];
	int i;
	for (i=0;param[i] != ' ' && param[i];i++) {
		key[i] = param[i];
	}
	if (param[i] == '\0')
		return NULL;

	key[i] = '\0';
	param += i+1;
	
	skynet_setenv(key,param);
	return NULL;
}

//获得系统的开始实时时间，从UTC1970-1-1 0:0:0开始计时，精确到秒
//返回结果是时间的一个字符串形式
static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	uint32_t sec = skynet_starttime();
	sprintf(context->result,"%u",sec);
	return context->result;
}

//将全局服务信息结构中的所有服务信息删除
static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

//监测退出的服务，param为NULL即获得监测的服务编号，有则返回":+十六进制的服务编号"，否则为NULL
//param不为NULL，则添加监测退出的服务，返回NULL，param可以是":+十六进制的服务编号"或者是".+服务名"形式
static const char *
cmd_monitor(struct skynet_context * context, const char * param) {
	uint32_t handle=0;
	if (param == NULL || param[0] == '\0') {
		if (G_NODE.monitor_exit) {
			// return current monitor serivce
			sprintf(context->result, ":%x", G_NODE.monitor_exit);
			return context->result;
		}
		return NULL;
	} else {
		handle = tohandle(context, param);	//获得服务标号 param可以是":+十六进制的服务编号"或者是".+服务名"形式
	}
	G_NODE.monitor_exit = handle;
	return NULL;
}

//获得指定服务的一些状态信息
//param为"mqlen"，获得服务队列中消息队列的长度
//param为"endless"，该服务是否陷入死循环
//param为"cpu"，获得该服务消耗CPU时间，整数部分为秒，小数部分精确到微秒
//param为"time"，获得距离该服务最近一条消息处理开始的时间间隔
//param为"message"，该服务已经处理消息的数量
static const char *
cmd_stat(struct skynet_context * context, const char * param) {
	if (strcmp(param, "mqlen") == 0) {		//如果param为"mqlen"
		int len = skynet_mq_length(context->queue);	//获得服务队列中消息队列的长度
		sprintf(context->result, "%d", len);
	} else if (strcmp(param, "endless") == 0) {		//如果param为"endless"
		if (context->endless) {					//该服务是否陷入死循环
			strcpy(context->result, "1");		//陷入死循环
			context->endless = false;			//清除标记
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "cpu") == 0) {		//如果param为"cpu"
		double t = (double)context->cpu_cost / 1000000.0;	// microsec
		sprintf(context->result, "%lf", t);		//获得该服务消耗CPU时间，整数部分为秒，小数部分精确到微秒
	} else if (strcmp(param, "time") == 0) {	//如果param为"time"
		if (context->profile) {
			uint64_t ti = skynet_thread_time() - context->cpu_start;	//获得距离该服务最近一条消息处理开始的时间间隔
			double t = (double)ti / 1000000.0;	// microsec
			sprintf(context->result, "%lf", t);
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "message") == 0) {	//如果param为"message"
		sprintf(context->result, "%d", context->message_count);		//该服务已经处理消息的数量
	} else {
		context->result[0] = '\0';
	}
	return context->result;
}

//为指定服务打开一个log文件，该文件的名字为：指定服务编号的十六进制形式.log
//param可以是":+十六进制的服务编号"或者是".+服务名"形式
static const char *
cmd_logon(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param); //获得服务标号 param可以是":+十六进制的服务编号"或者是".+服务名"形式
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);	//根据服务编号（包含节点号和服务号），获得服务信息
	if (ctx == NULL)
		return NULL;
	FILE *f = NULL;
	FILE * lastf = ctx->logfile;	//该服务的日志输出的文件
	if (lastf == NULL) {
		f = skynet_log_open(context, handle);
		if (f) {
			if (!ATOM_CAS_POINTER(&ctx->logfile, NULL, f)) {
				// logfile opens in other thread, close this one.
				fclose(f);
			}
		}
	}
	skynet_context_release(ctx);	//递减服务信息的引用计数，如果计数为0则释放
	return NULL;
}

//关闭指定服务的log文件，
//param可以是":+十六进制的服务编号"或者是".+服务名"形式
static const char *
cmd_logoff(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);	//获得服务标号 param可以是":+十六进制的服务编号"或者是".+服务名"形式
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);	//根据服务编号（包含节点号和服务号），获得服务信息
	if (ctx == NULL)
		return NULL;
	FILE * f = ctx->logfile;
	if (f) {
		// logfile may close in other thread
		if (ATOM_CAS_POINTER(&ctx->logfile, f, NULL)) {
			skynet_log_close(context, f, handle);
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

//调用指定服务的动态库的库文件名_signal的API函数
//param可以是":+十六进制的服务编号"或者是".+服务名"形式
static const char *
cmd_signal(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);	//获得服务标号 param可以是":+十六进制的服务编号"或者是".+服务名"形式
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);	//根据服务编号（包含节点号和服务号），获得服务信息
	if (ctx == NULL)
		return NULL;
	param = strchr(param, ' ');
	int sig = 0;
	if (param) {
		sig = strtol(param, NULL, 0);
	}
	// NOTICE: the signal function should be thread safe.
	skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

	skynet_context_release(ctx);
	return NULL;
}

static struct command_func cmd_funcs[] = {
	{ "TIMEOUT", cmd_timeout },
	{ "REG", cmd_reg },
	{ "QUERY", cmd_query },
	{ "NAME", cmd_name },
	{ "EXIT", cmd_exit },
	{ "KILL", cmd_kill },
	{ "LAUNCH", cmd_launch },
	{ "GETENV", cmd_getenv },
	{ "SETENV", cmd_setenv },
	{ "STARTTIME", cmd_starttime },
	{ "ABORT", cmd_abort },
	{ "MONITOR", cmd_monitor },
	{ "STAT", cmd_stat },
	{ "LOGON", cmd_logon },
	{ "LOGOFF", cmd_logoff },
	{ "SIGNAL", cmd_signal },
	{ NULL, NULL },
};

//根据命令名和参数对相应的服务调用相应的函数
const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];
	while(method->name) {
		if (strcmp(cmd, method->name) == 0) {
			return method->func(context, param);	//执行cmd_funcs数组中cmd指令名对应的函数
		}
		++method;
	}

	return NULL;
}

//消息发送前的参数处理
static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);		//判断是否需要拷贝消息内容
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;	//是否需要分配session
	type &= 0xff;		//获得消息的类型（256种）

	if (allocsession) {
		assert(*session == 0);
		*session = skynet_context_newsession(context);		//为消息分配一个session，用于回应
	}

	if (needcopy && *data) {		//拷贝发现消息的内容
		char * msg = skynet_malloc(*sz+1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	*sz |= (size_t)type << MESSAGE_TYPE_SHIFT;		//将消息类型添加到消息大小的高8位
}

/****************************
函数功能：发送消息
参数：
	context：服务信息指针
	source：消息源，即服务编号
	destination：消息的目的地服务编号
	type：消息类别
	session：用于请求回应，即标记回应的消息是对应哪一条发送消息进行回应，0表示不需要回应
	data：发送内容
	sz：发送内容大小
返回值：session
****************************/
int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {
	if ((sz & MESSAGE_TYPE_MASK) != sz) {	//消息大小超过24位
		skynet_error(context, "The message to %x is too large", destination);
		if (type & PTYPE_TAG_DONTCOPY) {
			skynet_free(data);
		}
		return -1;
	}
	_filter_args(context, type, &session, (void **)&data, &sz);		//消息发送前的参数处理

	if (source == 0) {
		source = context->handle;	//发送消息的源
	}

	if (destination == 0) {
		return session;
	}
	if (skynet_harbor_message_isremote(destination)) {		//判断消息发送的目的地是否非本地节点
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));	//拷贝非本地节点的发送消息
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz;
		skynet_harbor_send(rmsg, source, session);		//跨节点发送消息
	} else {
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;
		smsg.data = data;
		smsg.sz = sz;

		if (skynet_context_push(destination, &smsg)) {		//将消息添加到handle对于的服务信息中的服务队列中
			skynet_free(data);
			return -1;
		}
	}
	return session;
}

//通过addr指定的服务发送消息，addr的形式可以为".+服务名"或者":0x服务编号"
//返回session
int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr , int type, int session, void * data, size_t sz) {
	if (source == 0) {
		source = context->handle;		//获得本服务的handle
	}
	uint32_t des = 0;
	if (addr[0] == ':') {
		des = strtoul(addr+1, NULL, 16);	//将16进制的服务编号转换为无符号整数
	} else if (addr[0] == '.') {
		des = skynet_handle_findname(addr + 1);		//根据服务名获得服务的handle，没找到返回0
		if (des == 0) {
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -1;
		}
	} else {
		_filter_args(context, type, &session, (void **)&data, &sz);

		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz;

		skynet_harbor_send(rmsg, source, session);
		return session;
	}

	return skynet_send(context, source, des, type, session, data, sz);	//发送消息
}

//返回定位服务的编号（包含节点号和服务号）
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

//注册服务中的回调函数和回调对象
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}

/****************************
函数功能：用于跨节点发送消息
参数：
	ctx：服务信息
	msg：消息内容，用远程消息结构remote_message
	sz：消息内容大小
	source：消息源，即服务编号
	type：消息类型
	session：用于请求回应，即标记回应的消息是对应哪一条发送消息进行回应，0表示不需要回应
****************************/
void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | (size_t)type << MESSAGE_TYPE_SHIFT;

	skynet_mq_push(ctx->queue, &smsg);		//将消息添加到服务队列
}

//初始化与服务相关的全局信息
void 
skynet_globalinit(void) {
	G_NODE.total = 0;
	G_NODE.monitor_exit = 0;
	G_NODE.init = 1;
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {		//创建线程私有数据的key
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key
	skynet_initthread(THREAD_MAIN);		//初始化主线程私有数据的key
}

//删除掉与线程相关的私有数据
void 
skynet_globalexit(void) {
	pthread_key_delete(G_NODE.handle_key);
}

//初始化与对应线程相关的私有数据
void
skynet_initthread(int m) {
	uintptr_t v = (uint32_t)(-m);
	pthread_setspecific(G_NODE.handle_key, (void *)v);
}

//初始化是否开启监测服务CPU耗时标记
void
skynet_profile_enable(int enable) {
	G_NODE.profile = (bool)enable;
}
