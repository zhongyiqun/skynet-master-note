#ifndef SKYNET_H
#define SKYNET_H

#include "skynet_malloc.h"

#include <stddef.h>
#include <stdint.h>

#define PTYPE_TEXT 0						//log文本消息类型
#define PTYPE_RESPONSE 1					//回应包消息类型
#define PTYPE_MULTICAST 2					//
#define PTYPE_CLIENT 3
#define PTYPE_SYSTEM 4						//重定向log输出到指定的文件的消息类型
#define PTYPE_HARBOR 5
#define PTYPE_SOCKET 6						//套接字消息
// read lualib/skynet.lua examples/simplemonitor.lua
#define PTYPE_ERROR 7	
// read lualib/skynet.lua lualib/mqueue.lua lualib/snax.lua
#define PTYPE_RESERVED_QUEUE 8
#define PTYPE_RESERVED_DEBUG 9
#define PTYPE_RESERVED_LUA 10
#define PTYPE_RESERVED_SNAX 11

#define PTYPE_TAG_DONTCOPY 0x10000			//该类型消息在发送时不需要进行拷贝，一般为服务发送给自己的消息
#define PTYPE_TAG_ALLOCSESSION 0x20000		//标记消息的session需要系统分配

struct skynet_context;

void skynet_error(struct skynet_context * context, const char *msg, ...);
const char * skynet_command(struct skynet_context * context, const char * cmd , const char * parm);
uint32_t skynet_queryname(struct skynet_context * context, const char * name);
int skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * msg, size_t sz);
int skynet_sendname(struct skynet_context * context, uint32_t source, const char * destination , int type, int session, void * msg, size_t sz);

int skynet_isremote(struct skynet_context *, uint32_t handle, int * harbor);

typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);
void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

uint32_t skynet_current_handle(void);
uint64_t skynet_now(void);
void skynet_debug_memory(const char *info);	// for debug use, output current service memory to stderr

#endif
