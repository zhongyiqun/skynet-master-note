#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

struct logger {
	FILE * handle;		//用于存储log的输出，默认是控制台，否则为文档
	char * filename;	//如果是输出文件则保存文件名
	int close;			//标记是否需要关闭文档
};

//创建一个logger
struct logger *
logger_create(void) {
	struct logger * inst = skynet_malloc(sizeof(*inst));
	inst->handle = NULL;
	inst->close = 0;
	inst->filename = NULL;

	return inst;
}

void
logger_release(struct logger * inst) {
	if (inst->close) {
		fclose(inst->handle);
	}
	skynet_free(inst->filename);
	skynet_free(inst);
}

//处理该服务的消息回调函数
static int
logger_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct logger * inst = ud;
	switch (type) {
	case PTYPE_SYSTEM:		//logger服务的系统消息，以追加的形式重定向log输出到inst->filename指定的文件
		if (inst->filename) {
			inst->handle = freopen(inst->filename, "a", inst->handle);
		}
		break;
	case PTYPE_TEXT:		//正常的log输出
		fprintf(inst->handle, "[:%08x] ",source);	//以“:”+8位十六进制的形式输出服务发送消息的服务handle
		fwrite(msg, sz , 1, inst->handle);			//输出消息内容
		fprintf(inst->handle, "\n");				//回车
		fflush(inst->handle);						//刷新
		break;
	}

	return 0;
}

//初始化给定的inst，如果parm不为NULL则创建一个文件用于存储，否则输出到标准输出
//将服务命名为logger
int
logger_init(struct logger * inst, struct skynet_context *ctx, const char * parm) {
	if (parm) {		//如果参数不为NULL
		inst->handle = fopen(parm,"w");	//打开parm指定的文件，返回文件句柄
		if (inst->handle == NULL) {
			return 1;
		}
		inst->filename = skynet_malloc(strlen(parm)+1);
		strcpy(inst->filename, parm);	//拷贝文件名
		inst->close = 1;				//标记问需要关闭文档
	} else {
		inst->handle = stdout;			//否则为标准输出，即输出到控制台
	}
	if (inst->handle) {
		skynet_callback(ctx, inst, logger_cb);	//注册该服务实例的回调函数
		skynet_command(ctx, "REG", ".logger");	//将服务命名为logger
		return 0;
	}
	return 1;
}
