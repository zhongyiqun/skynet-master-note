#define LUA_LIB

#include "skynet_malloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include <lua.h>
#include <lauxlib.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "skynet_socket.h"

#define BACKLOG 32
// 2 ** 12 == 4096
#define LARGE_PAGE_NODE 12
#define BUFFER_LIMIT (256 * 1024)

struct buffer_node {			//存储套接字消息结点
	char * msg;					//套接字接收的消息
	int sz;						//套接字接收的消息大小
	struct buffer_node *next;	//下一个结点
};

//所有的套接字信息的缓存列表, 按照先到来的消息在前面, 后到来的消息在后面排列
struct socket_buffer {
	int size;					//所有消息的总大小, 随着到来而增加, 随着被处理而减少
	int offset;					//
	struct buffer_node *head;	//套接字接收信息的缓存头结点
	struct buffer_node *tail;	//套接字接收信息的缓存尾结点
};

/***************************
函数功能：释放内存块中每个结点中消息内容所占的内存
lua调用时需要传入的参数：
	1）数据块，
返回值：返回值的数量：0
	
***************************/
static int
lfreepool(lua_State *L) {
	struct buffer_node * pool = lua_touserdata(L, 1);	//获得第一个参数数据指针
	int sz = lua_rawlen(L,1) / sizeof(*pool);			//获得数据块中包含的结点数
	int i;
	for (i=0;i<sz;i++) {		//依次释放每个结点的消息内容所占的内存
		struct buffer_node *node = &pool[i];
		if (node->msg) {
			skynet_free(node->msg);
			node->msg = NULL;
		}
	}
	return 0;
}

/***************************
函数功能：在 lua 虚拟机中生成一个完全用户数据用作接收套接字信息的缓存队列，
		并设置 __gc 函数用以在回收内存时同时回收套接字消息的内存
参数：
	1）lua虚拟机，2）分配的结点的数量
返回值：返回值的数量：1
	1）userdata缓存列表
***************************/
static int
lnewpool(lua_State *L, int sz) {
	struct buffer_node * pool = lua_newuserdata(L, sizeof(struct buffer_node) * sz);	//在 lua 虚拟机中生成分配一块内存入栈
	int i;
	for (i=0;i<sz;i++) {	//初始化每个结点
		pool[i].msg = NULL;
		pool[i].sz = 0;
		pool[i].next = &pool[i+1];
	}
	pool[sz-1].next = NULL;
	if (luaL_newmetatable(L, "buffer_pool")) {	//如果注册表中已存在键 "buffer_pool"，返回0，否则，为用户数据的元表创建一张新表入栈
		lua_pushcfunction(L, lfreepool);		//将函数lfreepool指针压入栈
		lua_setfield(L, -2, "__gc");			//设置注册表中"__gc" = lfreepool
	}
	lua_setmetatable(L, -2);					//将注册表设置为分配的内存块的元表
	return 1;
}

/***************************
函数功能：在 lua 虚拟机中生成一个完全用户数据用作接收套接字信息的缓存列表
lua调用时需要传入的参数：
	无
返回值：返回值的数量：1
	1）userdata缓存列表
***************************/
static int
lnewbuffer(lua_State *L) {
	struct socket_buffer * sb = lua_newuserdata(L, sizeof(*sb));	
	sb->size = 0;
	sb->offset = 0;
	sb->head = NULL;
	sb->tail = NULL;
	
	return 1;
}

/*
	userdata send_buffer
	table pool
	lightuserdata msg
	int size

	return size

	Comment: The table pool record all the buffers chunk, 
	and the first index [1] is a lightuserdata : free_node. We can always use this pointer for struct buffer_node .
	The following ([2] ...)  userdatas in table pool is the buffer chunk (for struct buffer_node), 
	we never free them until the VM closed. The size of first chunk ([2]) is 16 struct buffer_node,
	and the second size is 32 ... The largest size of chunk is LARGE_PAGE_NODE (4096)

	lpushbbuffer will get a free struct buffer_node from table pool, and then put the msg/size in it.
	lpopbuffer return the struct buffer_node back to table pool (By calling return_free_node).
 */
/***************************
函数功能：将一个套接字消息插入到缓存列表中去，每次从缓存节点池中的第一个元素取节点，如果节点池中的第一个元素没有节点可取，
		则分配一个新的节点列表放入节点池的末尾，取出新分配的第一节点，并将后续的列表放入第一个元素中，第一次分配节点列表中节点的数量为16，
		依次32，64...2^12，节点池的第一个元素即记录当前可用节点的指针
lua调用时需要传入的参数：
	1）缓冲套接字接收的消息的队列，2）为缓存节点池，3）需要插入的消息内容，4）需要插入的消息的大小
返回值：返回值的数量：1
	1）消息队列中记录的消息的总长度
***************************/
static int
lpushbuffer(lua_State *L) {
	struct socket_buffer *sb = lua_touserdata(L,1);		//获得第一个参数一个缓冲套接字接收的消息的队列
	if (sb == NULL) {
		return luaL_error(L, "need buffer object at param 1");
	}
	char * msg = lua_touserdata(L,3);		//第三个参数 消息内容
	if (msg == NULL) {
		return luaL_error(L, "need message block at param 3");
	}
	int pool_index = 2;
	luaL_checktype(L,pool_index,LUA_TTABLE);	//检查第二个参数是否是表类型
	int sz = luaL_checkinteger(L,4);			//检查第四个参数是否为整数，并获得这个整数，即消息内容的大小
	lua_rawgeti(L,pool_index,1);			//将第二参数的表中的第一个元素入栈
	struct buffer_node * free_node = lua_touserdata(L,-1);	// sb poolt msg size free_node 获得这个元素
	lua_pop(L,1);
	if (free_node == NULL) {
		int tsz = lua_rawlen(L,pool_index);		//获得第二个参数表中的元素个数
		if (tsz == 0)
			tsz++;
		int size = 8;
		if (tsz <= LARGE_PAGE_NODE-3) {		//tsz小于等于9
			size <<= tsz;					//size = 2^(tsz+3)
		} else {
			size <<= LARGE_PAGE_NODE-3;		//size = 2^12
		}
		lnewpool(L, size);	//分配含有size个结点的接收套接字信息的缓存队列
		free_node = lua_touserdata(L,-1);	//获得分配的接收套接字信息的缓存队列
		lua_rawseti(L, pool_index, tsz+1);	//将第二个参数表中的第tsz+1个元素设置为上面分配的缓存队列
	}
	lua_pushlightuserdata(L, free_node->next);	//将当前节点的下一个结点入栈
	lua_rawseti(L, pool_index, 1);	// sb poolt msg size 将第二个参数表中的第一个元素设置为当前节点的下一个结点开始的队列
	free_node->msg = msg;	//记录消息内存
	free_node->sz = sz;		//记录消息大小
	free_node->next = NULL;	

	if (sb->head == NULL) {		//将指向的结点放入第一个参数消息队列中
		assert(sb->tail == NULL);
		sb->head = sb->tail = free_node;
	} else {
		sb->tail->next = free_node;
		sb->tail = free_node;
	}
	sb->size += sz;

	lua_pushinteger(L, sb->size);	//返回消息队列中记录的消息的总长度

	return 1;
}

/***************************
函数功能：回收缓存消息队列的头节点，并将回收的头节点插入节点池中，同时重置缓存消息队列的offset
参数：
	1）lua虚拟机，2）为缓存节点池在栈中的位置，3）缓存消息的队列
返回值：返回值的数量：0
	
***************************/
static void
return_free_node(lua_State *L, int pool, struct socket_buffer *sb) {
	struct buffer_node *free_node = sb->head; //获得消息缓存队列的头节点
	sb->offset = 0;
	sb->head = free_node->next;		//设置消息缓存队列的头节点指向下一个结点
	if (sb->head == NULL) {
		sb->tail = NULL;
	}
	lua_rawgeti(L,pool,1);			//获得节点池的第一个元素的列表
	free_node->next = lua_touserdata(L,-1);		//将当前的头节点插入节点池的第一个元素的头部
	lua_pop(L,1);
	skynet_free(free_node->msg);	//释放头结点中的消息内容
	free_node->msg = NULL;

	free_node->sz = 0;
	lua_pushlightuserdata(L, free_node);
	lua_rawseti(L, pool, 1);		//设置节点池的第一个元素的列表为插入头节点之后的列表
}

static void
pop_lstring(lua_State *L, struct socket_buffer *sb, int sz, int skip) {
	struct buffer_node * current = sb->head;	//获得消息缓存队列的头节点
	if (sz < current->sz - sb->offset) {
		lua_pushlstring(L, current->msg + sb->offset, sz-skip);
		sb->offset+=sz;
		return;
	}
	if (sz == current->sz - sb->offset) {
		lua_pushlstring(L, current->msg + sb->offset, sz-skip);
		return_free_node(L,2,sb);
		return;
	}

	luaL_Buffer b;
	luaL_buffinit(L, &b);
	for (;;) {
		int bytes = current->sz - sb->offset;
		if (bytes >= sz) {
			if (sz > skip) {
				luaL_addlstring(&b, current->msg + sb->offset, sz - skip);
			} 
			sb->offset += sz;
			if (bytes == sz) {
				return_free_node(L,2,sb);
			}
			break;
		}
		int real_sz = sz - skip;
		if (real_sz > 0) {
			luaL_addlstring(&b, current->msg + sb->offset, (real_sz < bytes) ? real_sz : bytes);
		}
		return_free_node(L,2,sb);
		sz-=bytes;
		if (sz==0)
			break;
		current = sb->head;
		assert(current);
	}
	luaL_pushresult(&b);
}

static int
lheader(lua_State *L) {
	size_t len;
	const uint8_t * s = (const uint8_t *)luaL_checklstring(L, 1, &len);
	if (len > 4 || len < 1) {
		return luaL_error(L, "Invalid read %s", s);
	}
	int i;
	size_t sz = 0;
	for (i=0;i<(int)len;i++) {
		sz <<= 8;
		sz |= s[i];
	}

	lua_pushinteger(L, (lua_Integer)sz);

	return 1;
}

/*
	userdata send_buffer
	table pool
	integer sz 
 */
static int
lpopbuffer(lua_State *L) {
	struct socket_buffer * sb = lua_touserdata(L, 1);
	if (sb == NULL) {
		return luaL_error(L, "Need buffer object at param 1");
	}
	luaL_checktype(L,2,LUA_TTABLE);
	int sz = luaL_checkinteger(L,3);
	if (sb->size < sz || sz == 0) {
		lua_pushnil(L);
	} else {
		pop_lstring(L,sb,sz,0);
		sb->size -= sz;
	}
	lua_pushinteger(L, sb->size);

	return 2;
}

/*
	userdata send_buffer
	table pool
 */
static int
lclearbuffer(lua_State *L) {
	struct socket_buffer * sb = lua_touserdata(L, 1);
	if (sb == NULL) {
		if (lua_isnil(L, 1)) {
			return 0;
		}
		return luaL_error(L, "Need buffer object at param 1");
	}
	luaL_checktype(L,2,LUA_TTABLE);
	while(sb->head) {
		return_free_node(L,2,sb);
	}
	sb->size = 0;
	return 0;
}

static int
lreadall(lua_State *L) {
	struct socket_buffer * sb = lua_touserdata(L, 1);
	if (sb == NULL) {
		return luaL_error(L, "Need buffer object at param 1");
	}
	luaL_checktype(L,2,LUA_TTABLE);
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	while(sb->head) {
		struct buffer_node *current = sb->head;
		luaL_addlstring(&b, current->msg + sb->offset, current->sz - sb->offset);
		return_free_node(L,2,sb);
	}
	luaL_pushresult(&b);
	sb->size = 0;
	return 1;
}

static int
ldrop(lua_State *L) {
	void * msg = lua_touserdata(L,1);
	luaL_checkinteger(L,2);
	skynet_free(msg);
	return 0;
}

static bool
check_sep(struct buffer_node * node, int from, const char *sep, int seplen) {
	for (;;) {
		int sz = node->sz - from;
		if (sz >= seplen) {
			return memcmp(node->msg+from,sep,seplen) == 0;
		}
		if (sz > 0) {
			if (memcmp(node->msg + from, sep, sz)) {
				return false;
			}
		}
		node = node->next;
		sep += sz;
		seplen -= sz;
		from = 0;
	}
}

/*
	userdata send_buffer
	table pool , nil for check
	string sep
 */
static int
lreadline(lua_State *L) {
	struct socket_buffer * sb = lua_touserdata(L, 1);
	if (sb == NULL) {
		return luaL_error(L, "Need buffer object at param 1");
	}
	// only check
	bool check = !lua_istable(L, 2);
	size_t seplen = 0;
	const char *sep = luaL_checklstring(L,3,&seplen);
	int i;
	struct buffer_node *current = sb->head;
	if (current == NULL)
		return 0;
	int from = sb->offset;
	int bytes = current->sz - from;
	for (i=0;i<=sb->size - (int)seplen;i++) {
		if (check_sep(current, from, sep, seplen)) {
			if (check) {
				lua_pushboolean(L,true);
			} else {
				pop_lstring(L, sb, i+seplen, seplen);
				sb->size -= i+seplen;
			}
			return 1;
		}
		++from;
		--bytes;
		if (bytes == 0) {
			current = current->next;
			from = 0;
			if (current == NULL)
				break;
			bytes = current->sz;
		}
	}
	return 0;
}

static int
lstr2p(lua_State *L) {
	size_t sz = 0;
	const char * str = luaL_checklstring(L,1,&sz);
	void *ptr = skynet_malloc(sz);
	memcpy(ptr, str, sz);
	lua_pushlightuserdata(L, ptr);
	lua_pushinteger(L, (int)sz);
	return 2;
}

// for skynet socket

/*
	lightuserdata msg
	integer size

	return type n1 n2 ptr_or_string
*/
static int
lunpack(lua_State *L) {
	struct skynet_socket_message *message = lua_touserdata(L,1);
	int size = luaL_checkinteger(L,2);

	lua_pushinteger(L, message->type);
	lua_pushinteger(L, message->id);
	lua_pushinteger(L, message->ud);
	if (message->buffer == NULL) {
		lua_pushlstring(L, (char *)(message+1),size - sizeof(*message));
	} else {
		lua_pushlightuserdata(L, message->buffer);
	}
	if (message->type == SKYNET_SOCKET_TYPE_UDP) {
		int addrsz = 0;
		const char * addrstring = skynet_socket_udp_address(message, &addrsz);
		if (addrstring) {
			lua_pushlstring(L, addrstring, addrsz);
			return 5;
		}
	}
	return 4;
}

/***************************
函数功能：首先判断栈中的第port_index个参数是否为nil，为nil则从addr字符串中获得host和port
		对于ipv6 addr的格式为"[host]:port"，对于ipv4 addr的格式为"host:port"
		如果栈中的第port_index个参数不为nil，则host = addr，port为栈中的第port_index个参数
lua调用时需要传入的参数：
	1）L lua虚拟机，2）缓存，3）地址信息的字符串addr，4）栈中的第port_index个参数，5）返回端口号port
返回值：返回值的数量：1
	1）主机名host
***************************/
static const char *
address_port(lua_State *L, char *tmp, const char * addr, int port_index, int *port) {
	const char * host;
	if (lua_isnoneornil(L,port_index)) { //当给定索引port_index无效或其值是 nil 时，返回 1，否则返回 0 
										//即判断是否有第port_index个参数
		host = strchr(addr, '[');	//查找地址字符串中首次出现'['的位置，返回其指针
		if (host) {
			// is ipv6 addr的格式为"[host]:port"
			++host;
			const char * sep = strchr(addr,']');
			if (sep == NULL) {
				luaL_error(L, "Invalid address %s.",addr);
			}
			memcpy(tmp, host, sep-host);
			tmp[sep-host] = '\0';
			host = tmp;
			sep = strchr(sep + 1, ':');
			if (sep == NULL) {
				luaL_error(L, "Invalid address %s.",addr);
			}
			*port = strtoul(sep+1,NULL,10);
		} else {
			// is ipv4 addr的格式为"host:port"
			const char * sep = strchr(addr,':');
			if (sep == NULL) {
				luaL_error(L, "Invalid address %s.",addr);
			}
			memcpy(tmp, addr, sep-addr);
			tmp[sep-addr] = '\0';
			host = tmp;
			*port = strtoul(sep+1,NULL,10);
		}
	} else {
		host = addr;
		*port = luaL_optinteger(L,port_index, 0);
	}
	return host;
}

/***************************
函数功能：发起连接服务器主机名host，端口号port
lua调用时需要传入的参数：
	1）套接字的信息存储的id
返回值：返回值的数量：1
	1）成功返回存储套接字信息的id,否则返回-1
***************************/
static int
lconnect(lua_State *L) {
	size_t sz = 0;
	const char * addr = luaL_checklstring(L,1,&sz); 	//检查函数的第 1 个参数是否是一个字符串，并返回该字符串，将字符串的长度填入sz 
	char tmp[sz];
	int port = 0;
	const char * host = address_port(L, tmp, addr, 2, &port);	//从addr中获得host和port
	if (port == 0) {
		return luaL_error(L, "Invalid port");
	}
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1)); 	//获得服务信息指针
	//发送命令'O'，发起连接服务器主机名host，端口号port，成功返回存储套接字信息的id,否则返回-1
	int id = skynet_socket_connect(ctx, host, port);
	lua_pushinteger(L, id);

	return 1;
}

/***************************
函数功能：关闭指定的套接字
lua调用时需要传入的参数：
	1）套接字的信息存储的id
返回值：返回值的数量：0
	
***************************/
static int
lclose(lua_State *L) {
	int id = luaL_checkinteger(L,1); 	//第一个参数，套接字的信息存储的id
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1)); 	//获得服务信息指针
	//发送命令'K'，发起一个关闭某个指定套接字的请求，通过id可以定位的套接字的信息
	skynet_socket_close(ctx, id);
	return 0;
}

/***************************
函数功能：强制关闭套接字
lua调用时需要传入的参数：
	1）套接字的信息存储的id
返回值：返回值的数量：0
	
***************************/
static int
lshutdown(lua_State *L) {
	int id = luaL_checkinteger(L,1); 	//第一个参数，套接字的信息存储的id
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1)); //获得服务信息指针
	//发送命令'K'，强制关闭套接字
	skynet_socket_shutdown(ctx, id);
	return 0;
}

/***************************
函数功能：发送命令'L'，发起绑定主机名host,端口号port，并监听命令
		连接请求队列的最大长度为backlog，成功返回存储套接字信息的id
lua调用时需要传入的参数：
	1）主机名host，2）端口号port，3）backlog，如果为nil则默认为32
返回值：返回值的数量：1
	1）成功返回存储套接字信息的id
***************************/
static int
llisten(lua_State *L) {
	const char * host = luaL_checkstring(L,1); 	//检查函数的第 1 个参数是否是一个字符串并返回这个字符串
	int port = luaL_checkinteger(L,2);			//检查函数的第 2 个参数是否是一个整数并返回这个整数
	int backlog = luaL_optinteger(L,3,BACKLOG);	//如果函数的第 3 个参数是一个整数，返回该整数。若该参数不存在或是nil，返回 BACKLOG=32
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息的指针
	//发送命令'L'，发起绑定主机名host，端口号port,并监听命令
	//连接请求队列的最大长度为backlog，成功返回存储套接字信息的id,否则返回-1
	int id = skynet_socket_listen(ctx, host,port,backlog);
	if (id < 0) {
		return luaL_error(L, "Listen error");
	}

	lua_pushinteger(L,id);
	return 1;
}

/***************************
函数功能：获得lua表中元素的总长度，通过将每个元素转换成字符串的形式来计算
	
参数：
	1）lua虚拟机环境，2）表在栈中的位置index
返回值：返回值的数量：1
	1）总长度
***************************/
static size_t
count_size(lua_State *L, int index) {
	size_t tlen = 0;
	int i;
	for (i=1;lua_geti(L, index, i) != LUA_TNIL; ++i) { //将表中的元素依次入栈
		size_t len;
		luaL_checklstring(L, -1, &len);	//获得每个元素的长度
		tlen += len;
		lua_pop(L,1);	//将入栈的元素出栈
	}
	lua_pop(L,1);
	return tlen;
}

/***************************
函数功能：将lua表中元素依次拷贝到缓存buffer中
	
参数：
	1）lua虚拟机环境，2）表在栈中的位置index，3）缓存buffer，4）表中元素的总长度tlen
返回值：返回值的数量：0
	
***************************/
static void
concat_table(lua_State *L, int index, void *buffer, size_t tlen) {
	char *ptr = buffer;
	int i;
	for (i=1;lua_geti(L, index, i) != LUA_TNIL; ++i) {
		size_t len;
		const char * str = lua_tolstring(L, -1, &len);
		if (str == NULL || tlen < len) {
			break;
		}
		memcpy(ptr, str, len);
		ptr += len;
		tlen -= len;
		lua_pop(L,1);
	}
	if (tlen != 0) {
		skynet_free(buffer);
		luaL_error(L, "Invalid strings table");
	}
	lua_pop(L,1);
}

/***************************
函数功能：获得栈中的第index个元素的数据，可以是以下几种类型的数据
	1）指针类型的，则第index+1个元素为数据的大小
	2）表类型，
	3）默认情况为字符串类型，
lua调用时需要传入的参数：
	1）lua虚拟机，2）栈中第index个元素，3）数据的长度sz
返回值：返回值的数量：1
	1）指向数据的指针
***************************/
static void *
get_buffer(lua_State *L, int index, int *sz) {
	void *buffer;
	switch(lua_type(L, index)) { 	//获得栈中索引处的值得类型
		const char * str;
		size_t len;
	case LUA_TUSERDATA:
	case LUA_TLIGHTUSERDATA:
		buffer = lua_touserdata(L,index);	//获得数据的指针
		*sz = luaL_checkinteger(L,index+1);	//获得数据的大小
		break;
	case LUA_TTABLE:
		// concat the table as a string
		len = count_size(L, index);		//获得表中元素的总长度
		buffer = skynet_malloc(len);	//分配内存
		concat_table(L, index, buffer, len); //将lua表中元素依次拷贝到缓存buffer中
		*sz = (int)len;		//表中元素的总长度
		break;
	default:
		str =  luaL_checklstring(L, index, &len);	
		buffer = skynet_malloc(len);
		memcpy(buffer, str, len);
		*sz = (int)len;
		break;
	}
	return buffer;
}

/***************************
函数功能：向指定套接字发送高优先级数据
	
lua调用时需要传入的参数：
	1）存储套接字信息的id，2）待发送的数据，3）如果第二个参数为指针则需要传入数据的大小
返回值：返回值的数量：1
	1）成功返回true，否则为false
***************************/
static int
lsend(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息的指针
	int id = luaL_checkinteger(L, 1);	//检查函数的第 1 个参数是否是一个整数并返回这个整数
	int sz = 0;
	void *buffer = get_buffer(L, 2, &sz); 	//获得栈中的第2个参数的数据，放入缓存buffer
	int err = skynet_socket_send(ctx, id, buffer, sz);	//发送高优先级数据，
	lua_pushboolean(L, !err);	//成功返回0，否则返回-1
	return 1;
}

/***************************
函数功能：向指定套接字发送低优先级数据
	
lua调用时需要传入的参数：
	1）存储套接字信息的id，2）待发送的数据，3）如果第二个参数为指针则需要传入数据的大小
返回值：返回值的数量：1
	1）成功返回true，否则为false
***************************/
static int
lsendlow(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息的指针
	int id = luaL_checkinteger(L, 1);	//检查函数的第 1 个参数是否是一个整数并返回这个整数
	int sz = 0;
	void *buffer = get_buffer(L, 2, &sz);	//获得栈中的第2个参数的数据，放入缓存buffer
	int err = skynet_socket_send_lowpriority(ctx, id, buffer, sz);
	lua_pushboolean(L, !err);
	return 1;
}

/***************************
函数功能：绑定外部生成的套接字fd
	
lua调用时需要传入的参数：
	1）绑定外部生成的套接字fd
返回值：返回值的数量：1
	1）成功返回存储套接字信息的id,否则返回-1
***************************/
static int
lbind(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息的指针
	int fd = luaL_checkinteger(L, 1);	//检查函数的第 1 个参数是否是一个整数并返回这个整数
	int id = skynet_socket_bind(ctx,fd);	//绑定外部生成的套接字fd，成功返回存储套接字信息的id,否则返回-1
	lua_pushinteger(L,id);
	return 1;
}

/***************************
函数功能：开始将指定套接字添加到epoll进行监听事件，
		可以是客户端请求连接的套接字，也可以是服务端的监听连接的套接字
	
lua调用时需要传入的参数：
	1）存储套接字信息的id
返回值：返回值的数量：0
	
***************************/
static int
lstart(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息的指针
	int id = luaL_checkinteger(L, 1);	//检查函数的第 1 个参数是否是一个整数并返回这个整数
	skynet_socket_start(ctx,id);	//开始将套接字添加到epoll进行监听事件，可以是客户端请求连接的套接字，也可以是服务端的监听连接的套接字
	return 0;
}

/***************************
函数功能：请求设置套接字的选项，选项的层次在 IPPROTO_TCP 上 , 设置的键和值都是 int 类型的,
		目前仅用于设置套接字的 TCP_NODELAY 选项，禁止发送合并的Nagle算法
	
lua调用时需要传入的参数：
	1）存储套接字信息的id
返回值：返回值的数量：0
	
***************************/
static int
lnodelay(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息的指针
	int id = luaL_checkinteger(L, 1);	//检查函数的第 1 个参数是否是一个整数并返回这个整数
	skynet_socket_nodelay(ctx,id);	//请求设置套接字的选项，选项的层次在 IPPROTO_TCP 上 , 设置的键和值都是 int 类型的,
									//目前仅用于设置套接字的 TCP_NODELAY 选项，禁止发送合并的Nagle算法
	return 0;
}


/***************************
函数功能：如果主机名host或端口port有不为空的，则创建UDP套接字，绑定套接字到主机名host，端口port，
		否则创建一个UDP套接字
	
lua调用时需要传入的参数：
	1）如果第2个参数为nil则从第1个参数中获得host和port
		对于ipv6 addr的格式为"[host]:port"，对于ipv4 addr的格式为"host:port"
		如果栈中的第2个参数不为nil，则host = addr，port为栈中的第2个参数
返回值：返回值的数量：1
	1）存储套接字信息的id
***************************/
static int
ludp(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息的指针
	size_t sz = 0;
	const char * addr = lua_tolstring(L,1,&sz);	//将第一个参数转换为字符串形式，并获得字符串的长度
	char tmp[sz];
	int port = 0;
	const char * host = NULL;
	if (addr) {
		host = address_port(L, tmp, addr, 2, &port);	//从addr中获得host和port
	}

	int id = skynet_socket_udp(ctx, host, port);	//如果主机名host或端口port有不为空的，则创建UDP套接字，绑定套接字到主机名host，端口port，
													//否则创建一个UDP套接字
	if (id < 0) {
		return luaL_error(L, "udp init failed");
	}
	lua_pushinteger(L, id);
	return 1;
}


/***************************
函数功能：将指定的套接字信息关联ip地址，前提是套接字信息中的协议类型相同
	
lua调用时需要传入的参数：
	1）存储套接字信息的id，2）如果第3个参数为nil则从第2个参数中获得host和port
		对于ipv6 addr的格式为"[host]:port"，对于ipv4 addr的格式为"host:port"
		如果栈中的第3个参数不为nil，则host = addr，port为栈中的第3个参数
返回值：返回值的数量：0
	
***************************/
static int
ludp_connect(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));	//获得服务信息的指针
	int id = luaL_checkinteger(L, 1); 	//检查函数的第 1 个参数是否是一个整数并返回这个整数
	size_t sz = 0;
	const char * addr = luaL_checklstring(L,2,&sz);	//检查函数的第2个参数是否是一个字符串并返回这个字符串以及这个字符串的长度
	char tmp[sz];
	int port = 0;
	const char * host = NULL;
	if (addr) {
		host = address_port(L, tmp, addr, 3, &port);	//从addr中获得host和port
	}

	if (skynet_socket_udp_connect(ctx, id, host, port)) { //将指定的套接字信息关联ip地址
		return luaL_error(L, "udp connect failed");
	}

	return 0;
}

/***************************
函数功能：给指定的套接字发送UDP数据
	
lua调用时需要传入的参数：
	1）存储套接字信息的id，2）ip地址信息，如果是ipv4则为：1 type + 2 port + 4 ipv4 = 7字节
	如果是ipv6则为：1 type + 2 port + 16 ipv4 = 19字节，3）待发送的数据
返回值：返回值的数量：1
	1）发生是否成功：成功返回true，否则为false
***************************/
static int
ludp_send(lua_State *L) {
	struct skynet_context * ctx = lua_touserdata(L, lua_upvalueindex(1));
	int id = luaL_checkinteger(L, 1);	//检查函数的第 1 个参数是否是一个整数并返回这个整数
	const char * address = luaL_checkstring(L, 2);	//检查函数的第2个参数是否是一个字符串并返回这个字符串
	int sz = 0;
	void *buffer = get_buffer(L, 3, &sz);	//获得栈中的第3个参数的数据，放入缓存buffer
	int err = skynet_socket_udp_send(ctx, id, address, buffer, sz); 	//发送UDP数据，成功返回0，否则返回-1

	lua_pushboolean(L, !err);

	return 1;
}

/***************************
函数功能：从指定的数据中获得ip地址和端口号
	
lua调用时需要传入的参数：
	1）包含ip地址和端口号的信息，如果是ipv4则为：1 type + 2 port + 4 ipv4 = 7字节
	如果是ipv6则为：1 type + 2 port + 16 ipv4 = 19字节
返回值：返回值的数量：2
	1）ip地址，2）端口号
***************************/
static int
ludp_address(lua_State *L) {
	size_t sz = 0;
	//检查函数的第 1 个参数是否是一个字符串并返回这个字符串以及这个字符串的长度
	const uint8_t * addr = (const uint8_t *)luaL_checklstring(L, 1, &sz);
	uint16_t port = 0;
	memcpy(&port, addr+1, sizeof(uint16_t));
	port = ntohs(port);		//将端口号由网络字节序转换为主机字节序
	const void * src = addr+3;
	char tmp[256];
	int family;
	if (sz == 1+2+4) {
		family = AF_INET;
	} else {
		if (sz != 1+2+16) {
			return luaL_error(L, "Invalid udp address");
		}
		family = AF_INET6;
	}
	if (inet_ntop(family, src, tmp, sizeof(tmp)) == NULL) {	//将“二进制整数” 转换为“点分十进制”
		return luaL_error(L, "Invalid udp address");
	}
	lua_pushstring(L, tmp);		//返回ip
	lua_pushinteger(L, port);	//返回端口号port
	return 2;
}

LUAMOD_API int
luaopen_skynet_socketdriver(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "buffer", lnewbuffer },
		{ "push", lpushbuffer },
		{ "pop", lpopbuffer },
		{ "drop", ldrop },
		{ "readall", lreadall },
		{ "clear", lclearbuffer },
		{ "readline", lreadline },
		{ "str2p", lstr2p },
		{ "header", lheader },
		{ "unpack", lunpack },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);	//创建一张新的表，并把列表 l 中的函数注册进去
	luaL_Reg l2[] = {
		{ "connect", lconnect },
		{ "close", lclose },
		{ "shutdown", lshutdown },
		{ "listen", llisten },
		{ "send", lsend },
		{ "lsend", lsendlow },
		{ "bind", lbind },
		{ "start", lstart },
		{ "nodelay", lnodelay },
		{ "udp", ludp },
		{ "udp_connect", ludp_connect },
		{ "udp_send", ludp_send },
		{ "udp_address", ludp_address },
		{ NULL, NULL },
	};
	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context"); //将服务信息指针入栈
	struct skynet_context *ctx = lua_touserdata(L,-1);	//获得服务信息指针
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");
	}

	luaL_setfuncs(L,l2,1);	//将表l2中的所有函数注册到栈顶，这些函数共享服务信息指针这个upvalue

	return 1;
}
