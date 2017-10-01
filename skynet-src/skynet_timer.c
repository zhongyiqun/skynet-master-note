#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#if defined(__APPLE__)
#include <sys/time.h>
#include <mach/task.h>
#include <mach/mach.h>
#endif

typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)	//256
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)	//64
#define TIME_NEAR_MASK (TIME_NEAR-1)		//0xff
#define TIME_LEVEL_MASK (TIME_LEVEL-1)		//0x3f

struct timer_event {
	uint32_t handle;		//记录定位服务的编号
	int session;			//记录用于接收消息响应时，定位到是响应哪一条消息，由发送消息的服务生成
};

struct timer_node {				//节点
	struct timer_node *next;	//指向下一个节点
	uint32_t expire;			//保存该节点的timer_event消息回复事件的触发时间片为：添加时的时间片加上延时触发的时间
};

struct link_list {				//链表
	struct timer_node head;		//头节点，head.next指向第一个节点
	struct timer_node *tail;	//尾节点
};

struct timer {
	struct link_list near[TIME_NEAR];	//保存时间片低8位的链表，每次都是从该数组中取链表
	struct link_list t[4][TIME_LEVEL];	//保存时间片高24位的链表，按照0，1，2，3从低位到高位都分别对应6位
	struct spinlock lock;				//锁
	uint32_t time;						//当前的时间片，单位为1/100秒
	uint32_t starttime;					//系统的开始实时时间，从UTC1970-1-1 0:0:0开始计时，精确到秒
	uint64_t current;					//从开始时刻到现在的时长，精确到1/100秒
	uint64_t current_point;				//系统启动时长，精确到1/100秒
};

static struct timer * TI = NULL;

//清除指定链表，并返回指向链表的第一个节点的指针
static inline struct timer_node *
link_clear(struct link_list *list) {
	struct timer_node * ret = list->head.next;	//取出指向第一个节点的指针
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

//向link_list链表中添加timer_node，head->next指向第一个，
//tail指向尾节点，并且节点timer_node的后面附加有timer_event的信息
static inline void
link(struct link_list *list,struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

//添加节点，将节点触发的时间和当前时间相比小于256的节点添加到near数组中，
//依次越靠近当前时间的添加到越低位
static void
add_node(struct timer *T,struct timer_node *node) {
	uint32_t time=node->expire;			//回复的时间片
	uint32_t current_time=T->time;
	
	//TIME_NEAR_MASK=0xff，如果高24位相等则将该节点添加到底8位中对应的链表
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {		
		link(&T->near[time&TIME_NEAR_MASK],node);	//将该节点加入低8位对应的链表
	} else {
		int i;
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;		//TIME_NEAR=256，TIME_LEVEL_SHIFT=6初始化为低14位的掩码
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;	//往左移6位，即依次产生20位，26位，32位掩码
		}

		//将该节点加入高24位对应的链表 TIME_NEAR_SHIFT=8，TIME_LEVEL_MASK=0x3f
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
	}
}

//向链表中添加节点，time的单位为1/100秒
static void
timer_add(struct timer *T,void *arg,size_t sz,int time) {
	//分配一个节点timer_node和附加的timer_event的内存块
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);	
	memcpy(node+1,arg,sz);

	SPIN_LOCK(T);

		node->expire=time+T->time;			//记录回复的时间片
		add_node(T,node);					//添加节点到相应的链表

	SPIN_UNLOCK(T);
}

//将对应的t[level][idx]链表取出，然后再依次将链表的节点添加，即实现了移动操作
static void
move_list(struct timer *T, int level, int idx) {
	struct timer_node *current = link_clear(&T->t[level][idx]);		//取出链表中指向第一个节点的指针
	while (current) {
		struct timer_node *temp=current->next;
		add_node(T,current);		//添加节点到相应的链表
		current=temp;
	}
}

//将高24位对应的4个6位的数组中的各个元素的链表往低位移
static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;	//256掩码
	uint32_t ct = ++T->time;	//时间片自加1
	if (ct == 0) {				//如果时间片已经溢出即归0
		move_list(T, 3, 0);		//将对应的t[3][0]链表取出，然后再依次将链表的节点添加，即实现了移动操作
	} else {
		uint32_t time = ct >> TIME_NEAR_SHIFT;	//ct右移8位
		int i=0;

		while ((ct & (mask-1))==0) {	//开始时，如果低8位溢出产生进位
			int idx=time & TIME_LEVEL_MASK;		//time & 0x3f  即取T->time的9-14位的值
			if (idx!=0) {
				move_list(T, i, idx);	//将对应的t[i][idx]链表取出，然后再依次将链表的节点添加，即实现了移动操作
				break;				
			}
			mask <<= TIME_LEVEL_SHIFT;		//mask左移6位
			time >>= TIME_LEVEL_SHIFT;		//time右移6位
			++i;
		}
	}
}

//处理链表中各个节点的消息，将消息分发到对应的服务
static inline void
dispatch_list(struct timer_node *current) {
	do {
		struct timer_event * event = (struct timer_event *)(current+1);
		struct skynet_message message;
		message.source = 0;
		message.session = event->session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;	//消息类型

		skynet_context_push(event->handle, &message);		//将消息添加到handle对应的服务信息中的服务队列中
		
		struct timer_node * temp = current;
		current=current->next;		//获取链表的下一个节点
		skynet_free(temp);			//释放处理过的节点的内存
	} while (current);
}

//检查当前的时间片的低8位对应的数组元素的链表是否为空，不为空则取出
static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK;		//取低8位对应的值
	
	while (T->near[idx].head.next) {		//如果低8位值对应的数组元素有链表，则取出
		struct timer_node *current = link_clear(&T->near[idx]);		//取出对应的链表
		SPIN_UNLOCK(T);
		// dispatch_list don't need lock T
		dispatch_list(current);		//处理取出链表中各个节点的消息，将消息分发到对应的服务
		SPIN_LOCK(T);
	}
}

//刷新时间片
static void 
timer_update(struct timer *T) {
	SPIN_LOCK(T);

	// try to dispatch timeout 0 (rare condition)
	timer_execute(T);		//检查当前的时间片的低8位对应的数组元素的链表是否为空，不为空则取出

	// shift time first, and then dispatch timer message
	timer_shift(T);			//时间片time自加1，移动高24位的链表

	timer_execute(T);		//检查当前的时间片的低8位对应的数组元素的链表是否为空，不为空则取出

	SPIN_UNLOCK(T);
}

//创建一个计时信息结构体
static struct timer *
timer_create_timer() {
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);	//清空链表
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);	//清空链表
		}
	}

	SPIN_INIT(r)

	r->current = 0;

	return r;
}

//定时回复，time的单位为1/100秒
int
skynet_timeout(uint32_t handle, int time, int session) {
	if (time <= 0) {	//如果时间小于或等于0，则立刻回复消息
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		if (skynet_context_push(handle, &message)) {	//将消息入对应服务的服务队列中
			return -1;
		}
	} else {		//给定时间后回复消息，将消息添加到队列中
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 seconds  cs:改为存1/100秒
//获得系统的实时时间，从UTC1970-1-1 0:0:0开始计时，精确到1/100秒
static void
systime(uint32_t *sec, uint32_t *cs) {
#if !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec;		//存储秒的部分
	*cs = (uint32_t)(ti.tv_nsec / 10000000);	//存储小于秒的部分，精确到1/100秒
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	*sec = tv.tv_sec;
	*cs = tv.tv_usec / 10000;
#endif
}

//获得从系统启动开始计时的时间，不受系统时间被用户改变的影响，精确到1/100秒
static uint64_t
gettime() {
	uint64_t t;
#if !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint64_t)ti.tv_sec * 100;		//将秒的部分乘以100
	t += ti.tv_nsec / 10000000;			//小于秒的部分，现在一个单位就是1/100秒
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint64_t)tv.tv_sec * 100;
	t += tv.tv_usec / 10000;
#endif
	return t;
}

//刷新时间
void
skynet_updatetime(void) {
	uint64_t cp = gettime();	//获得从系统启动开始计时的时间，不受系统时间被用户改变的影响，精确到1/100秒
	if(cp < TI->current_point) {	//如果
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;
	} else if (cp != TI->current_point) {
		uint32_t diff = (uint32_t)(cp - TI->current_point);		//从系统启动到目前的时间差，精确到1/100秒
		TI->current_point = cp;				//记录下当前的时间，不受系统时间被用户改变的影响，精确到1/100秒
		TI->current += diff;	
		int i;
		for (i=0;i<diff;i++) {
			timer_update(TI);		//刷新时间片
		}
	}
}

//获得开始时间，精确到秒
uint32_t
skynet_starttime(void) {
	return TI->starttime;
}

//获得从开始时间到当前的时长，精确到1/100秒
uint64_t 
skynet_now(void) {
	return TI->current;
}

//初始化系统计时
void 
skynet_timer_init(void) {
	TI = timer_create_timer();			//新建一个计时信息结构体
	uint32_t current = 0;
	systime(&TI->starttime, &current);	//获取系统初始化时的UTC时间
	TI->current = current;
	TI->current_point = gettime();	//获得系统启动时的CPU时间
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

//获取本线程到当前代码系统CPU花费的时间
uint64_t
skynet_thread_time(void) {
#if  !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	return (uint64_t)ti.tv_sec * MICROSEC + (uint64_t)ti.tv_nsec / (NANOSEC / MICROSEC);
#else
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t )&aTaskInfo, &aTaskInfoCount)) {
		return 0;
	}

	return (uint64_t)(aTaskInfo.user_time.seconds) + (uint64_t)aTaskInfo.user_time.microseconds;
#endif
}
