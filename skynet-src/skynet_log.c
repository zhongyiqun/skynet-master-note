#include "skynet_log.h"
#include "skynet_timer.h"
#include "skynet.h"
#include "skynet_socket.h"
#include <string.h>
#include <time.h>

//为服务打开一个log文件，文件名为指定服务编号的十六进制形式
FILE * 
skynet_log_open(struct skynet_context * ctx, uint32_t handle) {
	const char * logpath = skynet_getenv("logpath");	//获得log文件的存放路径，配置中配置的
	if (logpath == NULL)
		return NULL;
	size_t sz = strlen(logpath);
	char tmp[sz + 16];
	sprintf(tmp, "%s/%08x.log", logpath, handle);	//以指定服务编号的十六进制形式的log文件名
	FILE *f = fopen(tmp, "ab");	//打开log文件
	if (f) {
		uint32_t starttime = skynet_starttime();	//获得服务的开始时间，精确到秒
		uint64_t currenttime = skynet_now();		//当前时间
		time_t ti = starttime + currenttime/100;	//当前的UTC时间
		skynet_error(ctx, "Open log file %s", tmp);
		fprintf(f, "open time: %u %s", (uint32_t)currenttime, ctime(&ti));	//记录log打开的起始时间
		fflush(f);
	} else {
		skynet_error(ctx, "Open log file %s fail", tmp);
	}
	return f;
}

//关闭指定服务的log文件
void
skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle) {
	skynet_error(ctx, "Close log file :%08x", handle);
	fprintf(f, "close time: %u\n", (uint32_t)skynet_now());
	fclose(f);
}

static void
log_blob(FILE *f, void * buffer, size_t sz) {
	size_t i;
	uint8_t * buf = buffer;
	for (i=0;i!=sz;i++) {
		fprintf(f, "%02x", buf[i]);
	}
}

//消息类型为PTYPE_SOCKET的日志输出
static void
log_socket(FILE * f, struct skynet_socket_message * message, size_t sz) {
	fprintf(f, "[socket] %d %d %d ", message->type, message->id, message->ud);

	if (message->buffer == NULL) {
		const char *buffer = (const char *)(message + 1);
		sz -= sizeof(*message);
		const char * eol = memchr(buffer, '\0', sz);
		if (eol) {
			sz = eol - buffer;
		}
		fprintf(f, "[%*s]", (int)sz, (const char *)buffer);
	} else {
		sz = message->ud;
		log_blob(f, message->buffer, sz);
	}
	fprintf(f, "\n");
	fflush(f);
}

//将消息输出到指定的日志文件
void 
skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz) {
	if (type == PTYPE_SOCKET) {
		log_socket(f, buffer, sz);
	} else {
		uint32_t ti = (uint32_t)skynet_now();
		fprintf(f, ":%08x %d %d %u ", source, type, session, ti);
		log_blob(f, buffer, sz);
		fprintf(f,"\n");
		fflush(f);
	}
}
