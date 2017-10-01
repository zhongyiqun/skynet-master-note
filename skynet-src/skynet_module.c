#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32

struct modules {
	int count;									//记录已经加载的动态库的数量
	struct spinlock lock;						//锁
	const char * path;							//需要加载的动态库的路径
	struct skynet_module m[MAX_MODULE_TYPE];	//加载的动态库的信息，最多可以加载32个
};

static struct modules * M = NULL;

//从m模块数组中，打开指定名称为name的动态库
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;		//指定动态库实际路径字符串大小
	//search path
	void * dl = NULL;
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);
		while (*path == ';') path++;	//跳过path所指向的位置所有的';'指向第一个非';'的位置
		if (*path == '\0') break;
		l = strchr(path, ';');		//返回‘;’第一次出现的位置
		if (l == NULL) l = path + strlen(path);		//如果没';',则返回最后一个字符位置
		int len = l - path;			//获得字符串的长度
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];		//复制'?'前面的字符
		}
		memcpy(tmp+i,name,name_size);	//将？替换成实际的动态库名
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);	//将？后面的进行拷贝
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);		//加载指定的动态库
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

//从保存加载动态连接库信息的数组中查找指定名字的动态链接库文件信息指针
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

//更加api函数的名字从加载的动态连接库中，获取到相应的api函数指针
static void *
get_api(struct skynet_module *mod, const char *api_name) {
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	char tmp[name_size + api_size + 1];
	memcpy(tmp, mod->name, name_size);
	memcpy(tmp+name_size, api_name, api_size+1);
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}
	return dlsym(mod->module, ptr);
}

//将相应的动态库中的api函数指针存到指定的变量
static int
open_sym(struct skynet_module *mod) {
	mod->create = get_api(mod, "_create");	//获得动态库名+_create的函数指针
	mod->init = get_api(mod, "_init");		
	mod->release = get_api(mod, "_release");
	mod->signal = get_api(mod, "_signal");

	return mod->init == NULL;
}

//查询指定文件名的动态连接库信息
struct skynet_module * 
skynet_module_query(const char * name) {
	struct skynet_module * result = _query(name);		//查找相应名称的动态库是否已经加载
	if (result)
		return result;

	SPIN_LOCK(M)

	result = _query(name); // double check	查找相应名称的动态库是否已经加载

	if (result == NULL && M->count < MAX_MODULE_TYPE) {		//检测是否加载以及加载的数量是否达到上线值
		int index = M->count;
		void * dl = _try_open(M,name);
		if (dl) {
			M->m[index].name = name;	//将加载成功的动态库名保存到数组
			M->m[index].module = dl;	//将加载成功的动态库库引用保存到数组

			if (open_sym(&M->m[index]) == 0) {	//获得动态库中指定的_create、_init、_release、_signal函数指针
				M->m[index].name = skynet_strdup(name);	//拷贝动态库名
				M->count ++;	//已经加载的动态库数量加1
				result = &M->m[index];	//返回加载的动态库信息
			}
		}
	}

	SPIN_UNLOCK(M)

	return result;
}

//插入动态加载库的信息
void 
skynet_module_insert(struct skynet_module *mod) {
	SPIN_LOCK(M)

	struct skynet_module * m = _query(mod->name);
	assert(m == NULL && M->count < MAX_MODULE_TYPE);
	int index = M->count;
	M->m[index] = *mod;
	++M->count;

	SPIN_UNLOCK(M)
}

//调用相应动态库的库文件名_create的API函数
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

//调用相应动态库的库文件名_init的API函数
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

//调用相应动态库的库文件名_release的API函数
void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

//调用相应动态库的库文件名_signal的API函数
void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

//初始化需要加载的动态库的路径
void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;
	m->path = skynet_strdup(path);	//copy一份path

	SPIN_INIT(m)

	M = m;
}
