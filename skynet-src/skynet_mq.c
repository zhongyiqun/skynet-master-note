#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000		//暂无调用

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

struct message_queue {
	struct spinlock lock;			//锁
	uint32_t handle;				//服务handle，用于定位服务，高8位为节点的编号
	int cap;						//服务队列中消息队列的容量
	int head;						//服务队列中消息队列的头
	int tail;						//服务队列中消息队列的尾
	int release;					//标记是否释放
	int in_global;					//标记该服务是否在全局队列中
	int overload;					//记录服务队列中消息超过阈值时的数量
	int overload_threshold;			//服务队列中消息的上限值，超过将会翻倍
	struct skynet_message *queue;	//消息队列
	struct message_queue *next;		//指向下一个服务
};

struct global_queue {
	struct message_queue *head;		//全局队列头
	struct message_queue *tail;		//全局队列尾
	struct spinlock lock;			//锁
};

static struct global_queue *Q = NULL;

//将服务队列添加到全局队列
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;

	SPIN_LOCK(q)
	assert(queue->next == NULL);
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

//从全局队列中取出服务并删除
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	if(mq) {
		q->head = mq->next;
		if(q->head == NULL) {
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	SPIN_UNLOCK(q)

	return mq;
}

//创建服务队列
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;		//通过此变量来定位服务，相当于服务的地址，高8位为节点的编号
	q->cap = DEFAULT_QUEUE_SIZE;	//默认服务队列容量大小为64
	q->head = 0;					//初始化队列的头
	q->tail = 0;					//初始化队列的尾
	SPIN_INIT(q)
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.
	q->in_global = MQ_IN_GLOBAL;	//标记是否在全服队列中，默认为在
	q->release = 0;					//标记是否是否服务队列
	q->overload = 0;				//记录服务队列中消息超过阈值时的数量
	q->overload_threshold = MQ_OVERLOAD;	//服务队列中加载消息数量的阈值
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}

//释放服务队列
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}

//获取服务的handle
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

//获得服务队列中消息队列的长度
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)
	
	if (head <= tail) {
		return tail - head;
	}
	return tail + cap - head;
}

//获得消息数量超出阈值时的消息数量，并清零记录值
int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}

//从服务队列中取出消息，取出返回0，否则为1
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	SPIN_LOCK(q)

	if (q->head != q->tail) {
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		if (head >= cap) {
			q->head = head = 0;
		}
		int length = tail - head;	//记录服务队列中消息的数量
		if (length < 0) {
			length += cap;
		}
		while (length > q->overload_threshold) {	//消息数量超过阈值
			q->overload = length;		//记录超过阈值时的消息数量
			q->overload_threshold *= 2;		//阈值翻倍
		}
	} else {
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}

	if (ret) {			//服务队列中没有消息时，则将该服务队列从全局队列中踢出，设置标志位
		q->in_global = 0;
	}
	
	SPIN_UNLOCK(q)

	return ret;
}

//扩充服务队列的容量
static void
expand_queue(struct message_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	
	skynet_free(q->queue);
	q->queue = new_queue;
}

//将消息添加到服务队列
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q)

	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	if (q->head == q->tail) {
		expand_queue(q);	//扩充消息队列的容量
	}

	if (q->in_global == 0) {	//如果服务队列不在全局队列中，则将其添加到全局队列
		q->in_global = MQ_IN_GLOBAL;	//标记为在全局队列中
		skynet_globalmq_push(q);	//将服务队列添加到全局队列中
	}
	
	SPIN_UNLOCK(q)
}

//初始化全局队列
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));		//为全局队列分配内存
	memset(q,0,sizeof(*q));
	SPIN_INIT(q);
	Q=q;
}

//标记服务队列为释放
void 
skynet_mq_mark_release(struct message_queue *q) {
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

//删除服务队列
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	_release(q);
}

//将服务队列释放
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	
	if (q->release) {	//如果服务队列标记为释放，则将服务队列释放
		SPIN_UNLOCK(q)
		_drop_queue(q, drop_func, ud);
	} else {
		skynet_globalmq_push(q);	//将服务队列添加到全局队列
		SPIN_UNLOCK(q)
	}
}
