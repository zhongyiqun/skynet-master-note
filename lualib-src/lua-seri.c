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
#define COMBINE_TYPE(t,v) ((t) | (v) << 3)		//低3位为值得大类型，即nil，boolean，整数，指针，短字符串，长字符串，TYPE_TABLE
												//高5位为具体的小的类型，即TYPE_NUMBER_ZERO，TYPE_NUMBER_BYTE，TYPE_NUMBER_WORD等
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

struct read_block {				//读数据缓存，
	char * buffer;				//数据缓存指针
	int len;					//存储的数据大小
	int ptr;					//当前读取到的位置
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

/***************************
函数功能：初始化读数据缓存
参数：
	1）读数据缓存结构体指针，2）缓存指针，3）缓存大小
返回值：无
***************************/
static void
rball_init(struct read_block * rb, char * buffer, int size) {
	rb->buffer = buffer;
	rb->len = size;
	rb->ptr = 0;
}

/***************************
函数功能：每次读取数据，修改读数据缓存
参数：
	1）读数据缓存结构体指针，2）每次读取的大小
返回值：当前读取到数据缓存的位置
***************************/
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

/***************************
函数功能：将不含有元方法__pairs的表写入缓存队列中，并返回表的大小
		写表的过程为先写入表的类型，分为两种：一种为表的大小大于等于31的。一种为小于31的
		通过lua_rawgeti方法依次获得表中元素，并将其入栈
参数：
	1）lua虚拟机，2）b写缓存队列，3）index栈中的第几个元素，4）depth递归调用的层次
返回值：array_size表的大小
***************************/
static int
wb_table_array(lua_State *L, struct write_block * wb, int index, int depth) {
	int array_size = lua_rawlen(L,index);	//获得表的大小，不会触发元方法
	if (array_size >= MAX_COOKIE-1) { //表的大小大于等于31
		uint8_t n = COMBINE_TYPE(TYPE_TABLE, MAX_COOKIE-1);	
		wb_push(wb, &n, 1);		//向写缓存队列中写入一个字节的表类型
		wb_integer(wb, array_size);	//向写缓存中写入一个整数类型的数表示表的大小
	} else {			//表的大小小于31
		uint8_t n = COMBINE_TYPE(TYPE_TABLE, array_size);
		wb_push(wb, &n, 1);		//向写缓存队列中写入一个字节的表类型
	}

	int i;
	for (i=1;i<=array_size;i++) {
		lua_rawgeti(L,index,i);		//将index处的表的i的值压入栈
		pack_one(L, wb, -1, depth);	//将栈顶的元素写入队列中
		lua_pop(L,1);	//将栈顶元素出栈
	}

	return array_size;	//返回表的大小
}

/***************************
函数功能：将不含有元方法__pairs的表写入缓存队列中，
		写表的过程为通过依次调用next函数获得表中的键值对，并剔除掉键为1到array_size的元素
		将键值对写缓存队列，该函数需在wb_table_array函数之后调用
参数：
	1）lua虚拟机，2）b写缓存队列，3）index栈中的第几个元素，4）depth递归调用的层次，5）array_size之前写入的元素个数
返回值：无
***************************/
static void
wb_table_hash(lua_State *L, struct write_block * wb, int index, int depth, int array_size) {
	lua_pushnil(L);		//将nil值入栈
	while (lua_next(L, index) != 0) {	//从栈顶弹出一个键，然后把索引指定的表中的一个键值对压栈 
		if (lua_type(L,-2) == LUA_TNUMBER) {
			if (lua_isinteger(L, -2)) {		//剔除键为1到array_size的元素
				lua_Integer x = lua_tointeger(L,-2);
				if (x>0 && x<=array_size) {
					lua_pop(L,1);
					continue;
				}
			}
		}
		pack_one(L,wb,-2,depth);	//将键写入缓存队列
		pack_one(L,wb,-1,depth);	//将键对应的值写入缓存队列
		lua_pop(L, 1);		//将栈顶出栈
	}
	wb_nil(wb);		//表的结尾写一个nil到写缓存队列
}

/***************************
函数功能：将有元方法__pairs的表写入缓存队列中，并在表的结尾写入一个nil值
		写表的过程为通过不断的调用迭代函数获得相应的键值对，将相应的键值对写入缓存
参数：
	1）lua虚拟机，2）b写缓存队列，3）index栈中的第几个元素，4）depth递归调用的层次
返回值：无
***************************/
static void
wb_table_metapairs(lua_State *L, struct write_block *wb, int index, int depth) {
	uint8_t n = COMBINE_TYPE(TYPE_TABLE, 0);
	wb_push(wb, &n, 1);		//将一个字节的表类型写入缓存队列中
	lua_pushvalue(L, index);	//将栈中要写入的表拷贝压入栈顶
	lua_call(L, 1, 3);		//调用元方法__pairs，返回的三个值压入栈顶：迭代函数、调用的第一个参数、调用的第二个参数
							//每次调用之后需要更新新的第二个参数为第一个返回值.
	for(;;) {
		lua_pushvalue(L, -2);	//将第2个返回值拷贝压入栈
		lua_pushvalue(L, -2);	//将第3个返回值拷贝压入栈
		lua_copy(L, -5, -3);	//将第1个返回值拷贝覆盖第3个返回值
		lua_call(L, 2, 2);		//调用迭代函数
		int type = lua_type(L, -2);	//返回第一个返回值得类型
		if (type == LUA_TNIL) {	//为nil类型
			lua_pop(L, 4);
			break;
		}
		pack_one(L, wb, -2, depth);	//将表中的键写缓存队列
		pack_one(L, wb, -1, depth);	//将键对应的值写入缓存队列
		lua_pop(L, 1);	//将栈顶元素出栈
	}
	wb_nil(wb);	//结束在写入一个nil值到写缓存队列
}


/***************************
函数功能：将指定索引出栈中的表写入缓存队列，分为两种情况写入：一种为含有元方法__pairs的表，
		一种为不含有元方法__pairs的表
参数：
	1）lua虚拟机，2）wb写缓存队列，3）index栈中的第几个元素，4）depth递归调用的层次
返回值：无
***************************/
static void
wb_table(lua_State *L, struct write_block *wb, int index, int depth) {
	luaL_checkstack(L, LUA_MINSTACK, NULL);		//将栈的空间扩展到top+LUA_MINSTACK
	if (index < 0) {
		index = lua_gettop(L) + index + 1;
	}
	if (luaL_getmetafield(L, index, "__pairs") != LUA_TNIL) {  	//如果表中有元方法 __pairs ，则将其压入栈
		wb_table_metapairs(L, wb, index, depth);
	} else {	//不含有元方法__pairs的表
		int array_size = wb_table_array(L, wb, index, depth);
		wb_table_hash(L, wb, index, depth, array_size);
	}
}

/***************************
函数功能：将栈中的指定元素，存入写缓存中，
		
参数：
	1）lua虚拟机，2）b写缓存队列，3）index栈中的第几个元素，4）depth递归调用的层次
返回值：无
***************************/
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
	case LUA_TBOOLEAN: 		//向写缓存队列中添加一个值为布尔类型的值
		wb_boolean(b, lua_toboolean(L,index));
		break;
	case LUA_TSTRING: {		//向写缓存队列中添加一个字符串类型的值
		size_t sz = 0;
		const char *str = lua_tolstring(L,index,&sz);
		wb_string(b, str, (int)sz);
		break;
	}
	case LUA_TLIGHTUSERDATA:	//向写缓存队列中添加一个指针类型的值
		wb_pointer(b, lua_touserdata(L,index));
		break;
	case LUA_TTABLE: {			//向写缓存队列中添加一个表
		if (index < 0) {
			index = lua_gettop(L) + index + 1;
		}
		wb_table(L, b, index, depth+1);
		break;
	}
	default:
		wb_free(b);		//释放写缓存队列
		luaL_error(L, "Unsupport type %s to serialize", lua_typename(L, type));
	}
}

/***************************
函数功能：将栈中的指定范围的元素，存入写缓存中，
		
参数：
	1）lua虚拟机，2）b写缓存队列，3）from从栈的哪个位置开始到栈顶
返回值：无
***************************/
static void
pack_from(lua_State *L, struct write_block *b, int from) {
	int n = lua_gettop(L) - from;	//返回从from到栈顶的元素个数
	int i;
	for (i=1;i<=n;i++) {	//依次将栈中的元素写入缓存队列
		pack_one(L, b , from + i, 0);
	}
}

//无效数据流
static inline void
invalid_stream_line(lua_State *L, struct read_block *rb, int line) {
	int len = rb->len;
	luaL_error(L, "Invalid serialize stream %d (line:%d)", len, line);
}

#define invalid_stream(L,rb) invalid_stream_line(L,rb,__LINE__)

/***************************
函数功能：从读缓存的指定位置读取整数
		
参数：
	1）lua虚拟机，2）rb读缓存，3）cookie小类型
返回值：整数
***************************/
static lua_Integer
get_integer(lua_State *L, struct read_block *rb, int cookie) {
	switch (cookie) {	
	case TYPE_NUMBER_ZERO:	//值为0的整数
		return 0;
	case TYPE_NUMBER_BYTE: {	//只占一个字节的整数
		uint8_t n;
		uint8_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		n = *pn;
		return n;
	}
	case TYPE_NUMBER_WORD: {	//占两个字节的整数
		uint16_t n;
		uint16_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_DWORD: {	//占4个字节的整数
		int32_t n;
		int32_t * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_QWORD: {	//超过4个字节的整数
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

/***************************
函数功能：从读缓存的指定位置读取实数
		
参数：
	1）lua虚拟机，2）rb读缓存，
返回值：实数值
***************************/
static double
get_real(lua_State *L, struct read_block *rb) {
	double n;
	double * pn = rb_read(rb,sizeof(n));	//获得实数的指针
	if (pn == NULL)
		invalid_stream(L,rb);
	memcpy(&n, pn, sizeof(n));		//将读缓存中指定位置的内容转换实数
	return n;	//返回实数
}

/***************************
函数功能：从读缓存的指定位置读取指针
		
参数：
	1）lua虚拟机，2）rb读缓存，
返回值：指针
***************************/
static void *
get_pointer(lua_State *L, struct read_block *rb) {
	void * userdata = 0;
	void ** v = (void **)rb_read(rb,sizeof(userdata));	//获得指针的指针
	if (v == NULL) {	//如果指针为NULL
		invalid_stream(L,rb);
	}
	memcpy(&userdata, v, sizeof(userdata));	//将读缓存中指定位置的内容转换指针
	return userdata;	//返回读取到的指针
}

/***************************
函数功能：从读缓存的指定位置读取字符串
		
参数：
	1）lua虚拟机，2）rb读缓存，3）字符串的长度
返回值：无
***************************/
static void
get_buffer(lua_State *L, struct read_block *rb, int len) {
	char * p = rb_read(rb,len);
	if (p == NULL) {
		invalid_stream(L,rb);
	}
	lua_pushlstring(L,p,len);	//将字符串压入栈
}

static void unpack_one(lua_State *L, struct read_block *rb);

/***************************
函数功能：从读缓存中解析出一个表的数据
		
参数：
	1）lua虚拟机，2）rb读缓存，3）array_size表的大小
返回值：无
***************************/
static void
unpack_table(lua_State *L, struct read_block *rb, int array_size) {
	if (array_size == MAX_COOKIE-1) {	//表的大小为大于或等于31
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
		array_size = get_integer(L,rb,cookie);	//获得表的大小
	}
	luaL_checkstack(L,LUA_MINSTACK,NULL);	//将栈空间扩展到 top + LUA_MINSTACK 个元素
	lua_createtable(L,array_size,0);		//创建一张新的空表压栈
	int i;
	for (i=1;i<=array_size;i++) {
		unpack_one(L,rb);		//从读缓存读取表中的一个元素
		lua_rawseti(L,-2,i);	//将该元素赋值给表的索引值为i的键
	}
	for (;;) {					//将读缓存中的键值对存入表中
		unpack_one(L,rb);
		if (lua_isnil(L,-1)) {
			lua_pop(L,1);
			return;
		}
		unpack_one(L,rb);
		lua_rawset(L,-3);
	}
}

/***************************
函数功能：根据传入的大类型和小类型从读缓存中读取相应类型的数据

参数：
	1）lua虚拟机，2）rb读缓存，3）type大类型，4）cookie小类型
返回值：无
***************************/
static void
push_value(lua_State *L, struct read_block *rb, int type, int cookie) {
	switch(type) {
	case TYPE_NIL:			//读取到的大类型为nil类型
		lua_pushnil(L);		//将一个nil值压入栈
		break;
	case TYPE_BOOLEAN:		//读取到的大类型为boolean类型
		lua_pushboolean(L,cookie);	//将小类型即布尔值压入栈
		break;
	case TYPE_NUMBER:		//读取到的大类型为数值类型
		if (cookie == TYPE_NUMBER_REAL) {	//如果小类型为实数类型
			lua_pushnumber(L,get_real(L,rb));	
		} else {			//非实数类型的整数
			lua_pushinteger(L, get_integer(L, rb, cookie));	//将读取的整数压入栈
		}
		break;
	case TYPE_USERDATA:		//读取到的大类型为指针类型
		lua_pushlightuserdata(L,get_pointer(L,rb));	//将读取到的指针压入栈
		break;
	case TYPE_SHORT_STRING:	//读取到的大类型为短字符串类型
		get_buffer(L,rb,cookie);
		break;
	case TYPE_LONG_STRING: {	//读取到的大类型为长字符串类型
		if (cookie == 2) {		//小类型为2
			uint16_t *plen = rb_read(rb, 2);	//读取字符串的长度
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint16_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		} else {				//小类型为4
			if (cookie != 4) {
				invalid_stream(L,rb);
			}
			uint32_t *plen = rb_read(rb, 4);	//读取字符串的长度
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint32_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		}
		break;
	}
	case TYPE_TABLE: {	//读取到的大类型为表
		unpack_table(L,rb,cookie);
		break;
	}
	default: {
		invalid_stream(L,rb);
		break;
	}
	}
}

/***************************
函数功能：从读缓存中读取出某个类型的值，首先读取类型，再根据类型读取值

参数：
	1）lua虚拟机，2）rb读缓存
返回值：无
***************************/
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

/***************************
函数功能：将写缓存队列中的内容拷贝到一块缓存中，将该缓存指针和大小入栈
		即实现数据的序列化
参数：
	1）lua虚拟机，2）b写缓存队列，3）len写缓存队列中写入的内容大小
返回值：无
***************************/
static void
seri(lua_State *L, struct block *b, int len) {
	uint8_t * buffer = skynet_malloc(len);	//分配写缓存队列写入的字节大小的内存
	uint8_t * ptr = buffer;
	int sz = len;
	while(len>0) {			//将写缓存队列中的内容拷贝到分配的缓存中
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
	
	lua_pushlightuserdata(L, buffer);	//将缓存的指针入栈
	lua_pushinteger(L, sz);				//将缓存的大小入栈
}

/***************************
函数功能：将读缓存中的数据进行解析入栈
		即实现数据的反序列化
参数：
	1）缓存数据
返回值：解析出的数据占栈空间的大小
***************************/
int
luaseri_unpack(lua_State *L) {
	if (lua_isnoneornil(L,1)) {	//栈中的第一个元素为nil
		return 0;
	}
	void * buffer;		//缓存
	int len;
	if (lua_type(L,1) == LUA_TSTRING) {	//如果栈中的第一个元素为字符串类型
		size_t sz;
		 buffer = (void *)lua_tolstring(L,1,&sz);	//将第一个元素转换为一个 C 字符串存入缓存，并将字符串长度存入sz
		len = (int)sz;
	} else {
		buffer = lua_touserdata(L,1);	//将第一个元素转换为指针赋值给buffer
		len = luaL_checkinteger(L,2);	//指针所指内容的大小
	}
	if (len == 0) {	//栈中没有数据
		return 0;
	}
	if (buffer == NULL) {
		return luaL_error(L, "deserialize null pointer");
	}

	lua_settop(L,1);	//把栈上除第一个元素全部移除
	struct read_block rb;
	rball_init(&rb, buffer, len);	//初始化读数据缓存

	int i;
	for (i=0;;i++) {
		if (i%8==7) {
			luaL_checkstack(L,LUA_MINSTACK,NULL);	//将栈空间扩展到 top + LUA_MINSTACK 个元素
		}
		uint8_t type = 0;
		uint8_t *t = rb_read(&rb, sizeof(type));	//从缓存中读取一个字节的数据，即接下来的数据类型
		if (t==NULL)
			break;
		type = *t;
		push_value(L, &rb, type & 0x7, type>>3);	//根据大小类型从读缓存中解析出对应的值
	}

	// Need not free buffer

	return lua_gettop(L) - 1;	//获取解析出的数据存入栈的大小	
}

/***************************
函数功能：将栈中的内容序列化打包入栈
		
lua调用时需要传入的参数：需要序列化的内容
	
返回值：返回值的数量：2
	1）已经序列化打包好的指针，2）已经序列化打包好的内容大小
***************************/
LUAMOD_API int
luaseri_pack(lua_State *L) {
	struct block temp;
	temp.next = NULL;
	struct write_block wb;
	//初始化写数据缓存队列wb，队列的头结点和当前节点指向传入的数据块temp
	wb_init(&wb, &temp);
	pack_from(L,&wb,0);		//将栈中的所有元素写入缓存队列
	assert(wb.head == &temp);
	seri(L, &temp, wb.len);	//将写缓存队列中的内容序列化入栈

	wb_free(&wb);	//释放写缓存队列

	return 2;
}
