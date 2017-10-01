#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

struct monitor {				//用做定时器、监测、套接字和工作线程的运行函数都共享的参数
	int count;					//工作线程数量，即配置中配的
	struct skynet_monitor ** m;	//为每个工作线程存储监测信息的结构体
	pthread_cond_t cond;		//多线程同步机制中的条件变量
	pthread_mutex_t mutex;		//多线程同步机制中的互斥锁
	int sleep;					//记录处于阻塞状态的线程数量
	int quit;					//标记线程是否退出
};

struct worker_parm {			//用做工作线程的运行函数的参数
	struct monitor *m;
	int id;						//每个工作线程的序号
	int weight;					//标记每个线程每次处理服务队列中的消息数量
};

static int SIG = 0;

static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

//检测总的服务数量
#define CHECK_ABORT if (skynet_context_total()==0) break;

//创建线程
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

//唤醒等待条件触发的线程
static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);	//激活一个等待该条件的线程，存在多个等待线程时按入队顺序激活其中一个；
	}
}

//套接字线程运行函数
static void *
thread_socket(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_SOCKET);	//初始化该线程对应的私有数据块
	for (;;) {
		int r = skynet_socket_poll();	//处理所有套接字上的事件，返回处理的结果，将处理的结果及结果信息转发给对应的服务
		if (r==0)						//线程退出
			break;
		if (r<0) {
			CHECK_ABORT		//检测总的服务数量，为0则break
			continue;
		}
		wakeup(m,0);		//如果所有工作线程都处于等待状态，则唤醒其中一个
	}
	return NULL;
}

//释放资源
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

//检测线程运行函数
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;	//工作线程数量
	skynet_initthread(THREAD_MONITOR);	//初始化该线程对应的私有数据块
	for (;;) {
		CHECK_ABORT		//检测总的服务数量，为0则break
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);		//检查工作线程是否陷入死循环
		}
		for (i=0;i<5;i++) {		//睡眠5秒
			CHECK_ABORT		//检测总的服务数量，为0则break
			sleep(1);
		}
	}

	return NULL;
}

//发送服务内部消息打开log文件，将log输出到文件
static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");		//查找logger服务信息的handle
	if (logger) {
		skynet_context_push(logger, &smsg);		//将消息添加到对应的服务队列
	}
}

//定时器线程运行函数
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);	//初始化该线程对应的私有数据块
	for (;;) {
		skynet_updatetime();			//刷新时间
		CHECK_ABORT						//检测总的服务数量，为0则break
		wakeup(m,m->count-1);			//唤醒等待条件触发的线程
		usleep(2500);					//定时器线程挂起2500微秒
		if (SIG) {						//如果触发终端关闭的信号SIGHUP，则打开log文件
			signal_hup();				//发送服务内部消息打开log文件，将log输出到文件
			SIG = 0;
		}
	}
	// wakeup socket thread
	skynet_socket_exit();				//正常结束套接字服务
	// wakeup all worker thread
	pthread_mutex_lock(&m->mutex);		//获得互斥锁
	m->quit = 1;						//设置线程退出标志
	pthread_cond_broadcast(&m->cond);	//激活所有等待条件触发的线程
	pthread_mutex_unlock(&m->mutex);	//释放互斥锁
	return NULL;
}

//工作线程运行函数
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;				//当前工作线程的参数
	int id = wp->id;						//当前工作线程的序号
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	skynet_initthread(THREAD_WORKER);		//初始化该线程对应的私有数据块
	struct message_queue * q = NULL;
	while (!m->quit) {
		q = skynet_context_message_dispatch(sm, q, weight);		//消息分发
		if (q == NULL) {	//如果全局队列中没有服务队列信息，尝试获得互斥锁，等待定时器线程或套接字线程触发条件
			if (pthread_mutex_lock(&m->mutex) == 0) {	//获得互斥锁，如该锁已被其他工作线程锁住或拥有，则该线程阻塞直到可以
				++ m->sleep;		//线程阻塞计数加1
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);		//等待条件触发
				-- m->sleep;	//线程阻塞计数减1
				if (pthread_mutex_unlock(&m->mutex)) {		//释放互斥锁
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

static void
start(int thread) {
	pthread_t pid[thread+3];

	struct monitor *m = skynet_malloc(sizeof(*m));		//后面创建的线程都共享参数
	memset(m, 0, sizeof(*m));
	m->count = thread;		//工作线程的数量
	m->sleep = 0;			//记录处于阻塞状态的线程数量

	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *)); //为每个工作线程第一个存储监测信息的结构体
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();		//为每一个工作线程分配一块监测信息的内存
	}
	if (pthread_mutex_init(&m->mutex, NULL)) {	//初始化互斥锁
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) {	//初始化条件变量
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	create_thread(&pid[0], thread_monitor, m);		//创建监测线程
	create_thread(&pid[1], thread_timer, m);		//创建定时器线程
	create_thread(&pid[2], thread_socket, m);		//创建套接字线程

	static int weight[] = { 						//-1表示每个线程每次处理服务队列中的消息数量为1
		-1, -1, -1, -1, 0, 0, 0, 0,					//0表示每个线程每次处理服务队列中的所有消息
		1, 1, 1, 1, 1, 1, 1, 1, 					//1表示每个线程每次处理服务队列中的所有消息的1/2
		2, 2, 2, 2, 2, 2, 2, 2, 					//2表示每个线程每次处理服务队列中的所有消息的1/4
		3, 3, 3, 3, 3, 3, 3, 3, };					//3表示每个线程每次处理服务队列中的所有消息的1/8
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;			//各个线程共享参数部分
		wp[i].id = i;			//每个工作线程的序号
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			wp[i].weight = 0;
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);	//创建工作线程
	}

	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 	//等待各个线程结束
	}

	free_monitor(m);	//释放资源
}

//新建一个snlua服务
static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	sscanf(cmdline, "%s %s", name, args);	//name="snlua" args="bootstrap"
	struct skynet_context *ctx = skynet_context_new(name, args);	//新建一个snlua服务
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

void 
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	struct sigaction sa;
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

	//如果配置中有配置，以后台模式启动skynet
	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}
	skynet_harbor_init(config->harbor);		//初始化节点号
	skynet_handle_init(config->harbor);		//初始化全局服务信息
	skynet_mq_init();		//初始化全局队列
	skynet_module_init(config->module_path);	//初始化需要加载的动态库的路径
	skynet_timer_init();	//初始化计时
	skynet_socket_init();	//创建一个epoll
	skynet_profile_enable(config->profile);		//设置是否开启监测每个服务的CPU耗时标志

	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);	//新建有一个logger服务
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	bootstrap(ctx, config->bootstrap);		//新建一个snlua服务

	start(config->thread);		//开始工作，创建定时器、监测、套接字和相应数量的工作线程

	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();
	skynet_socket_free();
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
