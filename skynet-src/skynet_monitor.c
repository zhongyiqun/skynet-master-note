#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"
#include "atomic.h"

#include <stdlib.h>
#include <string.h>

struct skynet_monitor {
	int version;			//没修改一次source和destination自加1
	int check_version;		//已经检测到的version只，用于和当前的version进行比较，防止线程卡死在某一条消息
	uint32_t source;		//消息源，定位到发消息的服务
	uint32_t destination;	//消息发往的目的地
};

struct skynet_monitor * 
skynet_monitor_new() {
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret));
	memset(ret, 0, sizeof(*ret));
	return ret;
}

void 
skynet_monitor_delete(struct skynet_monitor *sm) {
	skynet_free(sm);
}

//记录当前线程处理的消息，处理完后清零
void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	sm->source = source;		//消息源
	sm->destination = destination;	//消息需要发送的目的地
	ATOM_INC(&sm->version);		//增加1
}

//检测某个工作线程是否卡主在某一条消息
void 
skynet_monitor_check(struct skynet_monitor *sm) {
	if (sm->version == sm->check_version) {		//当前处理的消息和已经监测到的消息是否相等
		if (sm->destination) {			//判断消息是否已经被处理
			skynet_context_endless(sm->destination);	//标记相应的服务陷入死循环
			skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);
		}
	} else {
		sm->check_version = sm->version;
	}
}
