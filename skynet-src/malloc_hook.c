#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <lua.h>
#include <stdio.h>

#include "malloc_hook.h"
#include "skynet.h"
#include "atomic.h"

// turn on MEMORY_CHECK can do more memory check, such as double free
// #define MEMORY_CHECK

#define MEMORY_ALLOCTAG 0x20140605
#define MEMORY_FREETAG 0x0badf00d

static size_t _used_memory = 0;		//记录所有服务分配的内存大小
static size_t _memory_block = 0;	//记录所有服务分配的内存块数

struct mem_data {		//用于记录每个服务占用内存的结构
	uint32_t handle;	//服务编号
	ssize_t allocated;	//记录该服务分配的内存大小
};

struct mem_cookie {		//分配内存时记录服务的编号
	uint32_t handle;	//服务编号
#ifdef MEMORY_CHECK		//内存的检查标记
	uint32_t dogtag;	//记录该内存块的检查标记
#endif
};

#define SLOT_SIZE 0x10000
#define PREFIX_SIZE sizeof(struct mem_cookie)	//在分配内存时时需要附件的内存大小

static struct mem_data mem_stats[SLOT_SIZE];	//所有服务占用内存的情况，此处SLOT_SIZE的大小为0x10000，但handle的大小是0x1000000，？


#ifndef NOUSE_JEMALLOC

#include "jemalloc.h"

// for skynet_lalloc use
#define raw_realloc je_realloc
#define raw_free je_free

//获得指定服务的记录服务分配内存大小的引用，返回0则表示没有记录的结构体
static ssize_t*
get_allocated_field(uint32_t handle) {
	int h = (int)(handle & (SLOT_SIZE - 1));	//获得当前服务的内存分配大小记录的位置
	struct mem_data *data = &mem_stats[h];		//获得当前服务的记录占用内存的结构
	uint32_t old_handle = data->handle;			//当前结构体中的handle
	ssize_t old_alloc = data->allocated;		//当前结构体中记录的内存大小
	if(old_handle == 0 || old_alloc <= 0) {		//如果这个结构未被占用，则用于当前的服务
		// data->allocated may less than zero, because it may not count at start.
		if(!ATOM_CAS(&data->handle, old_handle, handle)) {
			return 0;
		}
		if (old_alloc < 0) {
			ATOM_CAS(&data->allocated, old_alloc, 0);
		}
	}
	if(data->handle != handle) {
		return 0;
	}
	return &data->allocated;					//返回服务对应记录该服务分配内存大小的引用
}

//记录服务分配的内存大小
inline static void 
update_xmalloc_stat_alloc(uint32_t handle, size_t __n) {
	ATOM_ADD(&_used_memory, __n);	//记录分配的内存大小
	ATOM_INC(&_memory_block); 		//递增分配的内存块数	
	ssize_t* allocated = get_allocated_field(handle);	//获得记录服务分配内存大小的变量
	if(allocated) {
		ATOM_ADD(allocated, __n);	//记录已经刚才该服务分配的内存大小
	}
}

//每次释放服务中的内存时，递减记录该服务分配内存信息的变量
inline static void
update_xmalloc_stat_free(uint32_t handle, size_t __n) {
	ATOM_SUB(&_used_memory, __n);
	ATOM_DEC(&_memory_block);
	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		ATOM_SUB(allocated, __n);
	}
}

//填充分配的内存中附件的服务相关的信息
inline static void*
fill_prefix(char* ptr) {
	uint32_t handle = skynet_current_handle();	//获得当前线程处理的服务的handle
	size_t size = je_malloc_usable_size(ptr);	//获得分配的内存大小
	struct mem_cookie *p = (struct mem_cookie *)(ptr + size - sizeof(struct mem_cookie));	//获得存储附件信息的地址
	memcpy(&p->handle, &handle, sizeof(handle));	//填充handle的信息
#ifdef MEMORY_CHECK
	uint32_t dogtag = MEMORY_ALLOCTAG;			//记录为分配状态
	memcpy(&p->dogtag, &dogtag, sizeof(dogtag));
#endif
	update_xmalloc_stat_alloc(handle, size);	//刷新记录该服务分配内存大小的结构体
	return ptr;
}

//清除掉需要释放的内存记录在服务分配的内存结构中的信息
inline static void*
clean_prefix(char* ptr) {
	size_t size = je_malloc_usable_size(ptr);
	struct mem_cookie *p = (struct mem_cookie *)(ptr + size - sizeof(struct mem_cookie));
	uint32_t handle;
	memcpy(&handle, &p->handle, sizeof(handle));
#ifdef MEMORY_CHECK
	uint32_t dogtag;
	memcpy(&dogtag, &p->dogtag, sizeof(dogtag));
	if (dogtag == MEMORY_FREETAG) {
		fprintf(stderr, "xmalloc: double free in :%08x\n", handle);
	}
	assert(dogtag == MEMORY_ALLOCTAG);	// memory out of bounds
	dogtag = MEMORY_FREETAG;
	memcpy(&p->dogtag, &dogtag, sizeof(dogtag));
#endif
	update_xmalloc_stat_free(handle, size);
	return ptr;
}

//当无法继续分配内存时, 写入错误消息, 并退出进程
static void malloc_oom(size_t size) {
	fprintf(stderr, "xmalloc: Out of memory trying to allocate %zu bytes\n",
		size);
	fflush(stderr);
	abort();
}

//以人类可读的方式向标准误 stderr 中输出当前的 jemalloc 统计信息
void 
memory_info_dump(void) {
	je_malloc_stats_print(0,0,0);
}

size_t 
mallctl_int64(const char* name, size_t* newval) {
	size_t v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(size_t));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	// skynet_error(NULL, "name: %s, value: %zd\n", name, v);
	return v;
}

int 
mallctl_opt(const char* name, int* newval) {
	int v = 0;
	size_t len = sizeof(v);
	if(newval) {
		int ret = je_mallctl(name, &v, &len, newval, sizeof(int));
		if(ret == 0) {
			skynet_error(NULL, "set new value(%d) for (%s) succeed\n", *newval, name);
		} else {
			skynet_error(NULL, "set new value(%d) for (%s) failed: error -> %d\n", *newval, name, ret);
		}
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}

	return v;
}

// hook : malloc, realloc, free, calloc
//分配内存
void *
skynet_malloc(size_t size) {
	void* ptr = je_malloc(size + PREFIX_SIZE);	//分配内存，并在该分配的内存尾部存入服务编号的信息
	if(!ptr) malloc_oom(size);	//分配失败
	return fill_prefix(ptr);
}

//为ptr指向的内存块分配一块更大的内存，即ptr指向的内存块的大小+size
void *
skynet_realloc(void *ptr, size_t size) {
	if (ptr == NULL) return skynet_malloc(size);

	void* rawptr = clean_prefix(ptr);
	void *newptr = je_realloc(rawptr, size+PREFIX_SIZE);
	if(!newptr) malloc_oom(size);
	return fill_prefix(newptr);
}

//释放内存
void
skynet_free(void *ptr) {
	if (ptr == NULL) return;
	void* rawptr = clean_prefix(ptr);
	je_free(rawptr);
}

void *
skynet_calloc(size_t nmemb,size_t size) {
	void* ptr = je_calloc(nmemb + ((PREFIX_SIZE+size-1)/size), size );
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

void *
skynet_memalign(size_t alignment, size_t size) {
	void* ptr = je_memalign(alignment, size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

#else

// for skynet_lalloc use
#define raw_realloc realloc
#define raw_free free

void 
memory_info_dump(void) {
	skynet_error(NULL, "No jemalloc");
}

size_t 
mallctl_int64(const char* name, size_t* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_int64 %s.", name);
	return 0;
}

int 
mallctl_opt(const char* name, int* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_opt %s.", name);
	return 0;
}

#endif

//获得所有服务分配的内存大小
size_t
malloc_used_memory(void) {
	return _used_memory;
}

//获得所有服务分配的内存块数
size_t
malloc_memory_block(void) {
	return _memory_block;
}

//输出所有服务的内存分配信息
void
dump_c_mem() {
	int i;
	size_t total = 0;
	skynet_error(NULL, "dump all service mem:");
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			total += data->allocated;
			skynet_error(NULL, "0x%x -> %zdkb", data->handle, data->allocated >> 10);
		}
	}
	skynet_error(NULL, "+total: %zdkb",total >> 10);
}

char *
skynet_strdup(const char *str) {
	size_t sz = strlen(str);
	char * ret = skynet_malloc(sz+1);
	memcpy(ret, str, sz+1);
	return ret;
}

void * 
skynet_lalloc(void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		raw_free(ptr);
		return NULL;
	} else {
		return raw_realloc(ptr, nsize);
	}
}

int
dump_mem_lua(lua_State *L) {
	int i;
	lua_newtable(L);
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			lua_pushinteger(L, data->allocated);
			lua_rawseti(L, -2, (lua_Integer)data->handle);
		}
	}
	return 1;
}

//获得当前服务的内存分配大小
size_t
malloc_current_memory(void) {
	uint32_t handle = skynet_current_handle();
	int i;
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle == (uint32_t)handle && data->allocated != 0) {
			return (size_t) data->allocated;
		}
	}
	return 0;
}

void
skynet_debug_memory(const char *info) {
	// for debug use
	uint32_t handle = skynet_current_handle();
	size_t mem = malloc_current_memory();
	fprintf(stderr, "[:%08x] %s %p\n", handle, info, (void *)mem);
}
