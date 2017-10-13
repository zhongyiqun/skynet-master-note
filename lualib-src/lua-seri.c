/*
	modify from https://github.com/cloudwu/lua-serialize
 */

#define LUA_LIB

#include "skynet_malloc.h"

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

//用于在内存块中接下来存储的数据类型，以及该字节中存入的数据类型
#define TYPE_NIL 0				//nil类型
#define TYPE_BOOLEAN 1			//boolean类型
// hibits 0 false 1 true
#define TYPE_NUMBER 2			//整数类型
// hibits 0 : 0 , 1: byte, 2:word, 4: dword, 6: qword, 8 : double
#define TYPE_NUMBER_ZERO 0		//整数类型值为0
#define TYPE_NUMBER_BYTE 1		//整数类型值小于256
#define TYPE_NUMBER_WORD 2		//2字节的整数类型
#define TYPE_NUMBER_DWORD 4		//4字节的整数类型
#define TYPE_NUMBER_QWORD 6		//8字节的整数类型
#define TYPE_NUMBER_REAL 8		//浮点数类型

#define TYPE_USERDATA 3			//指针类型
#define TYPE_SHORT_STRING 4		//短字符串类型
// hibits 0~31 : len
#define TYPE_LONG_STRING 5		//长字符串类型
#define TYPE_TABLE 6

#define MAX_COOKIE 32
#define COMBINE_TYPE(t,v) ((t) | (v) << 3)		//用于加入类型值

#define BLOCK_SIZE 128
#define MAX_DEPTH 32

struct block {					//数据块节点，可以存储128个字节数据
	struct block * next;		//指向下一个数据块
	char buffer[BLOCK_SIZE];	//数据块，128个字节
};

struct write_block {			//写数据缓冲队列，一块块的数据
	struct block * head;		//写缓存队列的头节点，第一个数据块
	struct block * current;		//写缓存队列的当前节点，即可以添加数据的块
	int len;					//整个队列存储的数据长度
	int ptr;					//记录当前数据块可以添加数据的位置
};

struct read_block {
	char * buffer;
	int len;
	int ptr;
};

//分配一块blcok结构的内存
inline static struct block *
blk_alloc(void) {
	struct block *b = skynet_malloc(sizeof(struct block));		//分配一块block的内存
	b->next = NULL;
	return b;
}

/***************************
函数功能：往写缓冲队列中添加数据
参数：
	1）需要添加的数据指针，2）需要添加的数据大小
返回值：无
***************************/
inline static void
wb_push(struct write_block *b, const void *buf, int sz) {
	const char * buffer = buf;	//获得需要添加的数据指针
	if (b->ptr == BLOCK_SIZE) {	//如果当前数据块已满
_again:
		b->current = b->current->next = blk_alloc();	//分配一个新的数据块
		b->ptr = 0;		//指向当前数据块的第一个字节
	}
	if (b->ptr <= BLOCK_SIZE - sz) {	//如果当前数据块可以存储下指定的数据
		memcpy(b->current->buffer + b->ptr, buffer, sz);
		b->ptr+=sz;		//指向下一个可以存储数据的位置
		b->len+=sz;		//相应增加整个队列的数据大小
	} else {		//如果当前的块不足以存下所有的数据
		int copy = BLOCK_SIZE - b->ptr;	//当前块剩余的空间
		memcpy(b->current->buffer + b->ptr, buffer, copy);	//将当前块剩余的部分存满，
		buffer += copy;		//移动到下一个需要存储的位置
		b->len += copy;		//相应增加整个队列的数据大小
		sz -= copy;			//减去已经存的大小
		goto _again;		//跳转
	}
}

/***************************
函数功能：初始化写数据缓存队列，队列的头结点和当前节点指向传入的数据块
参数：
	1）写数据缓存队列指针，2）数据块指针
返回值：无
***************************/
static void
wb_init(struct write_block *wb , struct block *b) {
	wb->head = b;
	assert(b->next == NULL);
	wb->len = 0;
	wb->current = wb->head;
	wb->ptr = 0;
}

/***************************
函数功能：释放写数据缓存队列
参数：
	1）写数据缓存队列指针
返回值：无
***************************/
static void
wb_free(struct write_block *wb) {
	struct block *blk = wb->head;
	blk = blk->next;	// the first block is on stack
	while (blk) {
		struct block * next = blk->next;
		skynet_free(blk);
		blk = next;
	}
	wb->head = NULL;
	wb->current = NULL;
	wb->ptr = 0;
	wb->len = 0;
}

static void
rball_init(struct read_block * rb, char * buffer, int size) {
	rb->buffer = buffer;
	rb->len = size;
	rb->ptr = 0;
}

static void *
rb_read(struct read_block *rb, int sz) {
	if (rb->len < sz) {
		return NULL;
	}

	int ptr = rb->ptr;
	rb->ptr += sz;
	rb->len -= sz;
	return rb->buffer + ptr;
}

/***************************
函数功能：向写缓存队列中添加一个值为TYPE_NIL的字节，低四位为类型值
参数：
	1）写数据缓存队列指针
返回值：无
***************************/
static inline void
wb_nil(struct write_block *wb) {
	uint8_t n = TYPE_NIL;
	wb_push(wb, &n, 1);
}

/***************************
函数功能：向写缓存队列中添加一个值为布尔类型的值，低四位的为类型值
参数：
	1）一个整数，如果为1则布尔值为true,否则布尔值为false
返回值：无
***************************/
static inline void
wb_boolean(struct write_block *wb, int boolean) {
	uint8_t n = COMBINE_TYPE(TYPE_BOOLEAN , boolean ? 1 : 0);
	wb_push(wb, &n, 1);
}

/***************************
函数功能：向写缓存队列中添加一个整数类型的值，将整数分为这几种情况存入：
		1）为0的整数，只需一个字节
		2）超过4字节的整数，则先存入一个字节记录整数占的字节数，然后将该整数存入
		3）负整数，用4字节存储，则先存入一个字节记录整数占的字节数，然后将该整数存入
		4）小于256的整数，则先存入一个字节记录整数占的字节数，然后将该整数存入
		5）占用两字节的整数，则先存入一个字节记录整数占的字节数，然后将该整数存入
		6）其他整数，则先存入一个字节记录整数占的字节数，然后将该整数存入
参数：
	1）一个整数类型的值
返回值：无
***************************/
static inline void
wb_integer(struct write_block *wb, lua_Integer v) {
	int type = TYPE_NUMBER; 		//整数类型
	if (v == 0) {					
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_ZERO);	//值为0的整数
		wb_push(wb, &n, 1);			//存入该整数，只占用一个字节
	} else if (v != (int32_t)v) {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_QWORD);	//超过4字节的整数
		int64_t v64 = v;
		wb_push(wb, &n, 1);					//先存入一个字节记录整数占的长度			
		wb_push(wb, &v64, sizeof(v64));		//存入占8字节的整数
	} else if (v < 0) {
		int32_t v32 = (int32_t)v;
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_DWORD);	//4字节的整数类型
		wb_push(wb, &n, 1);					//先存入一个字节记录整数占的长度
		wb_push(wb, &v32, sizeof(v32));		//存入占4字节的负整数
	} else if (v<0x100) {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_BYTE);	//整数类型值小于256
		wb_push(wb, &n, 1);					//先存入一个字节记录整数占得长度
		uint8_t byte = (uint8_t)v;
		wb_push(wb, &byte, sizeof(byte));	//存入占1字节的整数
	} else if (v<0x10000) {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_WORD);	//值为大于0x100小于0x10000的整数
		wb_push(wb, &n, 1);					//先存入一个字节记录整数占得长度
		uint16_t word = (uint16_t)v;
		wb_push(wb, &word, sizeof(word));	//存入占两个字节大小的整数
	} else {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_DWORD);  //4字节的整数类型
		wb_push(wb, &n, 1);					//先存入一个字节记录整数占得长度
		uint32_t v32 = (uint32_t)v;
		wb_push(wb, &v32, sizeof(v32));		//存入占4字节的正整数
	}
}

/***************************
函数功能：向写缓存队列中添加一个浮点数类型的值	
参数：
	1）一个浮点数类型的值
返回值：无
***************************/
static inline void
wb_real(struct write_block *wb, double v) {
	uint8_t n = COMBINE_TYPE(TYPE_NUMBER , TYPE_NUMBER_REAL);
	wb_push(wb, &n, 1);			//先存入一个字节记录浮点数占得长度
	wb_push(wb, &v, sizeof(v));	//存入浮点数
}

/***************************
函数功能：向写缓存队列中添加一个指针类型的值	
参数：
	1）一个指针数类型的值
返回值：无
***************************/
static inline void
wb_pointer(struct write_block *wb, void *v) {
	uint8_t n = TYPE_USERDATA;
	wb_push(wb, &n, 1);
	wb_push(wb, &v, sizeof(v));
}

/***************************
函数功能：向写缓存队列中添加一个字符串类型的值，按照以下规则存入
		1）字符串长度不超过32，一个字节记录类型和长度，接着存入字符串
		2）字符串长度大于32小于0x10000，一个字节记录类型和长度类型，2字节记录实际的字符串长度，接着存入字符串
		3）字符串长度大于0x10000，一个字节记录类型和长度类型，4字节记录实际的字符串长度，接着存入字符串
参数：
	1）一个字符串类型的值，2）字符串的长度
返回值：无
***************************/
static inline void
wb_string(struct write_block *wb, const char *str, int len) {
	if (len < MAX_COOKIE) {		//字符串长度不超过32
		uint8_t n = COMBINE_TYPE(TYPE_SHORT_STRING, len);
		wb_push(wb, &n, 1);		//先存入记录字符串类型和长度的字节
		if (len > 0) {
			wb_push(wb, str, len);	//存入字符串
		}
	} else {
		uint8_t n;
		if (len < 0x10000) {	//大于32小于0x10000长度的字符串
			n = COMBINE_TYPE(TYPE_LONG_STRING, 2);
			wb_push(wb, &n, 1);	//先存入记录字符串类型的和长度类型的字节
			uint16_t x = (uint16_t) len;
			wb_push(wb, &x, 2);	//存入字符串的长度
		} else {
			n = COMBINE_TYPE(TYPE_LONG_STRING, 4);
			wb_push(wb, &n, 1);		//先存入记录字符串类型的和长度类型的字节
			uint32_t x = (uint32_t) len;
			wb_push(wb, &x, 4);		//存入字符串的长度
		}
		wb_push(wb, str, len);		//存入字符串
	}
}

static void pack_one(lua_State *L, struct write_block *b, int index, int depth);

static int
wb_table_array(lua_State *L, struct write_block * wb, int index, int depth) {
	int array_size = lua_rawlen(L,index);
	if (array_size >= MAX_COOKIE-1) {
		uint8_t n = COMBINE_TYPE(TYPE_TABLE, MAX_COOKIE-1);
		wb_push(wb, &n, 1);
		wb_integer(wb, array_size);
	} else {
		uint8_t n = COMBINE_TYPE(TYPE_TABLE, array_size);
		wb_push(wb, &n, 1);
	}

	int i;
	for (i=1;i<=array_size;i++) {
		lua_rawgeti(L,index,i);
		pack_one(L, wb, -1, depth);
		lua_pop(L,1);
	}

	return array_size;
}

static void
wb_table_hash(lua_State *L, struct write_block * wb, int index, int depth, int array_size) {
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		if (lua_type(L,-2) == LUA_TNUMBER) {
			if (lua_isinteger(L, -2)) {
				lua_Integer x = lua_tointeger(L,-2);
				if (x>0 && x<=array_size) {
					lua_pop(L,1);
					continue;
				}
			}
		}
		pack_one(L,wb,-2,depth);
		pack_one(L,wb,-1,depth);
		lua_pop(L, 1);
	}
	wb_nil(wb);
}

static void
wb_table_metapairs(lua_State *L, struct write_block *wb, int index, int depth) {
	uint8_t n = COMBINE_TYPE(TYPE_TABLE, 0);
	wb_push(wb, &n, 1);
	lua_pushvalue(L, index);
	lua_call(L, 1, 3);
	for(;;) {
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_copy(L, -5, -3);
		lua_call(L, 2, 2);
		int type = lua_type(L, -2);
		if (type == LUA_TNIL) {
			lua_pop(L, 4);
			break;
		}
		pack_one(L, wb, -2, depth);
		pack_one(L, wb, -1, depth);
		lua_pop(L, 1);
	}
	wb_nil(wb);
}

static void
wb_table(lua_State *L, struct write_block *wb, int index, int depth) {
	luaL_checkstack(L, LUA_MINSTACK, NULL);
	if (index < 0) {
		index = lua_gettop(L) + index + 1;
	}
	if (luaL_getmetafield(L, index, "__pairs") != LUA_TNIL) {
		wb_table_metapairs(L, wb, index, depth);
	} else {
		int array_size = wb_table_array(L, wb, index, depth);
		wb_table_hash(L, wb, index, depth, array_size);
	}
}

static void
pack_one(lua_State *L, struct write_block *b, int index, int depth) {
	if (depth > MAX_DEPTH) {	//如果depth大于32
		wb_free(b);			//释放写缓存队列
		luaL_error(L, "serialize can't pack too depth table");
	}
	int type = lua_type(L,index);		//获得栈index处的值类型
	switch(type) {
	case LUA_TNIL:		//nil类型
		wb_nil(b);		//向写缓存队列中添加一个值为TYPE_NIL的字节
		break;
	case LUA_TNUMBER: {	//number类型
		if (lua_isinteger(L, index)) {	//如果栈index处是个整数
			lua_Integer x = lua_tointeger(L,index);		//获得该整数
			wb_integer(b, x);	//将整数存入缓存
		} else {
			lua_Number n = lua_tonumber(L,index);	//如果是实数
			wb_real(b,n);	//将浮点数存入缓存
		}
		break;
	}
	case LUA_TBOOLEAN: 
		wb_boolean(b, lua_toboolean(L,index));
		break;
	case LUA_TSTRING: {
		size_t sz = 0;
		const char *str = lua_tolstring(L,index,&sz);
		wb_string(b, str, (int)sz);
		break;
	}
	case LUA_TLIGHTUSERDATA:
		wb_pointer(b, lua_touserdata(L,index));
		break;
	case LUA_TTABLE: {
		if (index < 0) {
			index = lua_gettop(L) + index + 1;
		}
		wb_table(L, b, index, depth+1);
		break;
	}
	default:
		wb_free(b);
		luaL_error(L, "Unsupport type %s to serialize", lua_typename(L, type));
	}
}


static void
pack_from(lua_State *L, struct write_block *b, int from) {
	int n = lua_gettop(L) - from;
	int i;
	for (i=1;i<=n;i++) {
		pack_one(L, b , from + i, 0);
	}
}

static inline void
invalid_stream_line(lua_State *L, struct read_block *rb, int line) {
	int len = rb->len;
	luaL_error(L, "Invalid serialize stream %d (line:%d)", len, line);
}

#define invalid_stream(L,rb) invalid_stream_line(L,rb,__LINE__)

static lua_Integer
get_integer(lua_State *L, struct read_block *rb, int cookie) {
	switch (cookie) {
	case TYPE_NUMBER_ZERO:
		return 0;
	case TYPE_NUMBER_BYTE: {
		uint8_t n;
		uint8_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		n = *pn;
		return n;
	}
	case TYPE_NUMBER_WORD: {
		uint16_t n;
		uint16_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_DWORD: {
		int32_t n;
		int32_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_QWORD: {
		int64_t n;
		int64_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	default:
		invalid_stream(L,rb);
		return 0;
	}
}

static double
get_real(lua_State *L, struct read_block *rb) {
	double n;
	double * pn = rb_read(rb,sizeof(n));
	if (pn == NULL)
		invalid_stream(L,rb);
	memcpy(&n, pn, sizeof(n));
	return n;
}

static void *
get_pointer(lua_State *L, struct read_block *rb) {
	void * userdata = 0;
	void ** v = (void **)rb_read(rb,sizeof(userdata));
	if (v == NULL) {
		invalid_stream(L,rb);
	}
	memcpy(&userdata, v, sizeof(userdata));
	return userdata;
}

static void
get_buffer(lua_State *L, struct read_block *rb, int len) {
	char * p = rb_read(rb,len);
	if (p == NULL) {
		invalid_stream(L,rb);
	}
	lua_pushlstring(L,p,len);
}

static void unpack_one(lua_State *L, struct read_block *rb);

static void
unpack_table(lua_State *L, struct read_block *rb, int array_size) {
	if (array_size == MAX_COOKIE-1) {
		uint8_t type;
		uint8_t *t = rb_read(rb, sizeof(type));
		if (t==NULL) {
			invalid_stream(L,rb);
		}
		type = *t;
		int cookie = type >> 3;
		if ((type & 7) != TYPE_NUMBER || cookie == TYPE_NUMBER_REAL) {
			invalid_stream(L,rb);
		}
		array_size = get_integer(L,rb,cookie);
	}
	luaL_checkstack(L,LUA_MINSTACK,NULL);
	lua_createtable(L,array_size,0);
	int i;
	for (i=1;i<=array_size;i++) {
		unpack_one(L,rb);
		lua_rawseti(L,-2,i);
	}
	for (;;) {
		unpack_one(L,rb);
		if (lua_isnil(L,-1)) {
			lua_pop(L,1);
			return;
		}
		unpack_one(L,rb);
		lua_rawset(L,-3);
	}
}

static void
push_value(lua_State *L, struct read_block *rb, int type, int cookie) {
	switch(type) {
	case TYPE_NIL:
		lua_pushnil(L);
		break;
	case TYPE_BOOLEAN:
		lua_pushboolean(L,cookie);
		break;
	case TYPE_NUMBER:
		if (cookie == TYPE_NUMBER_REAL) {
			lua_pushnumber(L,get_real(L,rb));
		} else {
			lua_pushinteger(L, get_integer(L, rb, cookie));
		}
		break;
	case TYPE_USERDATA:
		lua_pushlightuserdata(L,get_pointer(L,rb));
		break;
	case TYPE_SHORT_STRING:
		get_buffer(L,rb,cookie);
		break;
	case TYPE_LONG_STRING: {
		if (cookie == 2) {
			uint16_t *plen = rb_read(rb, 2);
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint16_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		} else {
			if (cookie != 4) {
				invalid_stream(L,rb);
			}
			uint32_t *plen = rb_read(rb, 4);
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint32_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		}
		break;
	}
	case TYPE_TABLE: {
		unpack_table(L,rb,cookie);
		break;
	}
	default: {
		invalid_stream(L,rb);
		break;
	}
	}
}

static void
unpack_one(lua_State *L, struct read_block *rb) {
	uint8_t type;
	uint8_t *t = rb_read(rb, sizeof(type));
	if (t==NULL) {
		invalid_stream(L, rb);
	}
	type = *t;
	push_value(L, rb, type & 0x7, type>>3);
}

static void
seri(lua_State *L, struct block *b, int len) {
	uint8_t * buffer = skynet_malloc(len);
	uint8_t * ptr = buffer;
	int sz = len;
	while(len>0) {
		if (len >= BLOCK_SIZE) {
			memcpy(ptr, b->buffer, BLOCK_SIZE);
			ptr += BLOCK_SIZE;
			len -= BLOCK_SIZE;
			b = b->next;
		} else {
			memcpy(ptr, b->buffer, len);
			break;
		}
	}
	
	lua_pushlightuserdata(L, buffer);
	lua_pushinteger(L, sz);
}

int
luaseri_unpack(lua_State *L) {
	if (lua_isnoneornil(L,1)) {
		return 0;
	}
	void * buffer;
	int len;
	if (lua_type(L,1) == LUA_TSTRING) {
		size_t sz;
		 buffer = (void *)lua_tolstring(L,1,&sz);
		len = (int)sz;
	} else {
		buffer = lua_touserdata(L,1);
		len = luaL_checkinteger(L,2);
	}
	if (len == 0) {
		return 0;
	}
	if (buffer == NULL) {
		return luaL_error(L, "deserialize null pointer");
	}

	lua_settop(L,1);
	struct read_block rb;
	rball_init(&rb, buffer, len);

	int i;
	for (i=0;;i++) {
		if (i%8==7) {
			luaL_checkstack(L,LUA_MINSTACK,NULL);
		}
		uint8_t type = 0;
		uint8_t *t = rb_read(&rb, sizeof(type));
		if (t==NULL)
			break;
		type = *t;
		push_value(L, &rb, type & 0x7, type>>3);
	}

	// Need not free buffer

	return lua_gettop(L) - 1;
}

LUAMOD_API int
luaseri_pack(lua_State *L) {
	struct block temp;
	temp.next = NULL;
	struct write_block wb;
	//初始化写数据缓存队列wb，队列的头结点和当前节点指向传入的数据块temp
	wb_init(&wb, &temp);
	pack_from(L,&wb,0);
	assert(wb.head == &temp);
	seri(L, &temp, wb.len);

	wb_free(&wb);

	return 2;
}
