#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);
typedef void (*skynet_dl_signal)(void * inst, int signal);

struct skynet_module {
	const char * name;			//动态库名
	void * module;				//打开的库文件指针
	skynet_dl_create create;	//存储动态连接库中以库文件名_create的API函数指针
	skynet_dl_init init;		//存储动态连接库中以库文件名_init的API函数指针
	skynet_dl_release release;	//存储动态连接库中以库文件名_release的API函数指针
	skynet_dl_signal signal;	//存储动态连接库中以库文件名_signal的API函数指针
};

void skynet_module_insert(struct skynet_module *mod);
struct skynet_module * skynet_module_query(const char * name);
void * skynet_module_instance_create(struct skynet_module *);
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
void skynet_module_instance_release(struct skynet_module *, void *inst);
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);

void skynet_module_init(const char *path);

#endif
