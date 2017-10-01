#include "skynet.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_MESSAGE_SIZE 256

//将错误信息输出到logger服务，msg为输出的格式
void 
skynet_error(struct skynet_context * context, const char *msg, ...) {
	static uint32_t logger = 0;
	if (logger == 0) {
		logger = skynet_handle_findname("logger");
	}
	if (logger == 0) {
		return;
	}

	char tmp[LOG_MESSAGE_SIZE];
	char *data = NULL;

	va_list ap;		//指向可变参数

	va_start(ap,msg);		//将msg之后第一个变参的地址赋予ap
	int len = vsnprintf(tmp, LOG_MESSAGE_SIZE, msg, ap);	//将可变参数格式化输出到字符数组tmp,返回最终生成字符串的长度
	va_end(ap);		//清空ap，即结束变参的获取
	if (len >=0 && len < LOG_MESSAGE_SIZE) {
		data = skynet_strdup(tmp);
	} else {
		int max_size = LOG_MESSAGE_SIZE;
		for (;;) {
			max_size *= 2;
			data = skynet_malloc(max_size);
			va_start(ap,msg);
			len = vsnprintf(data, max_size, msg, ap);
			va_end(ap);
			if (len < max_size) {
				break;
			}
			skynet_free(data);
		}
	}
	if (len < 0) {
		skynet_free(data);
		perror("vsnprintf error :");
		return;
	}


	struct skynet_message smsg;
	if (context == NULL) {
		smsg.source = 0;
	} else {
		smsg.source = skynet_context_handle(context);
	}
	smsg.session = 0;
	smsg.data = data;
	smsg.sz = len | ((size_t)PTYPE_TEXT << MESSAGE_TYPE_SHIFT);
	skynet_context_push(logger, &smsg);		//将消息添加到logger的服务信息中的服务队列中
}

