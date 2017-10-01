#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"
#include "atomic.h"
#include "spinlock.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16
#define MAX_EVENT 64						//epoll一次最多返回的事件数量
#define MIN_READ_BUFFER 64					//从套接字中一次性最少读取的字节数

//用于标记socket结构体的状态
#define SOCKET_TYPE_INVALID 0				//socket结构体未被使用
#define SOCKET_TYPE_RESERVE 1				//socket结构体已被分配，但是还没有实际进行网络连接
#define SOCKET_TYPE_PLISTEN 2				//已经绑定套接字监听端口号，但是没有添加到epoll监听事件，调用start_socket函数才会，变为SOCKET_TYPE_LISTEN状态
#define SOCKET_TYPE_LISTEN 3				//已经绑定套接字监听端口号, 并且已经添加到epoll监听事件
#define SOCKET_TYPE_CONNECTING 4			//套接字正在连接中, 但是还没有连接上, 此时还不能传送信息
#define SOCKET_TYPE_CONNECTED 5				//套接字连接成功, 可以发送信息
#define SOCKET_TYPE_HALFCLOSE 6				//半关闭状态, 虽然套接字本身没有关闭, 但是已经不能往里边添加信息了, 最终会在清空写缓冲的情况下关闭
#define SOCKET_TYPE_PACCEPT 7				//已经接受了客户端的连接, 但是没有添加到epoll监听事件, 当调用 start_socket 才变成 CONNECTED
#define SOCKET_TYPE_BIND 8					//绑定外部创建的套接字，监听可读事件

#define MAX_SOCKET (1<<MAX_SOCKET_P)		//2^16，套接字的最大数量

#define PRIORITY_HIGH 0						//标记为高优先级的写缓存
#define PRIORITY_LOW 1						//标记为低优先级的写缓存

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)

#define PROTOCOL_TCP 0						//标记为TCP协议类型
#define PROTOCOL_UDP 1						//标记为UDP IPv4协议类型
#define PROTOCOL_UDPv6 2					//标记为UDP IPv6协议类型

#define UDP_ADDRESS_SIZE 19	// ipv6 128bit + port 16bit + 1 byte type

#define MAX_UDP_PACKAGE 65535				//接收UDP数据包的最大长度

// EAGAIN and EWOULDBLOCK may be not the same value.
#if (EAGAIN != EWOULDBLOCK)
#define AGAIN_WOULDBLOCK EAGAIN : case EWOULDBLOCK
#else
#define AGAIN_WOULDBLOCK EAGAIN
#endif

#define WARNING_SIZE (1024*1024)

//socket 写入的缓存数据, 如果是 TCP 协议将不包含 udp_address 字段, 而仅仅是前面部分
struct write_buffer {
	struct write_buffer * next;				//处于 wb_list 中的下一个写缓存
	void *buffer;							//调用者传递过来的缓存, 从中可以提取出发送数据, 最后需要回收内存
	char *ptr;								//发送数据的起始指针, 会随着不断写入 socket 而向后移动
	int sz;									//发送数据的大小, 会随着不断写入 socket 而减小
	bool userobject;						//标记初始化发送对象时有没有用socket_server中的soi接口
	uint8_t udp_address[UDP_ADDRESS_SIZE];	//udp_address[0]存协议类型，udp_address[1]，udp_address[]存端口号，剩余部分存ip地址
};

#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))	//返回结构体write_buffer中成员udp_address[0]的偏移量
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))						//返回结构体write_buffer的大小

struct wb_list {				//写缓存队列
	struct write_buffer * head;	//队列头
	struct write_buffer * tail;	//尾队列
};

//每个套接字相关的信息
struct socket {							//套接字相关信息结构体
	uintptr_t opaque;					//一般用于存储定位服务的handle
	struct wb_list high;				//高优先级的写缓存
	struct wb_list low;					//低优先级的写缓存
	int64_t wb_size;					//套接字写入缓存的大小，会随着缓存的大小变化，为高优先级和低优先级的和
	int fd;								//套接字描述符
	int id;								//用于在socket_server结构体中定位存储套接字信息
	uint8_t protocol;					//协议类型
	uint8_t type;						//socket结构体所处的状态，绑定套接字时，即套接字的状态
	uint16_t udpconnecting;				//大于0标记该套接字正在进行关联ip地址操作，用于UDP协议
	int64_t warn_size;					//阈值，写缓存超过的阈值，每超过一次阈值就会翻倍
	union {
		int size;						//在 TCP 协议下使用, 表示一次性最多读取的字节数
		uint8_t udp_address[UDP_ADDRESS_SIZE];	//udp_address[0]存协议类型，udp_address[1]，udp_address[]存端口号，剩余部分存ip地址
	} p;
	struct spinlock dw_lock;			//写缓存锁
	int dw_offset;						//已经发送的数据偏移位置
	const void * dw_buffer;				//已发送一部分的全部数据缓存
	size_t dw_size;						//已发送一部分的全部数据的大小
};

//全局的信息
struct socket_server {
	int recvctrl_fd;					//读管道fd
	int sendctrl_fd;					//写管道fd
	int checkctrl;						//默认值为1，是否需要检查管道中的命令的标记
	poll_fd event_fd;					//epoll句柄
	int alloc_id;						//当前分配到的socket ID
	int event_n;						//epoll触发的事件数量
	int event_index;					//当前已经处理的epoll事件的数量
	struct socket_object_interface soi;	//初始化发送对象时用
	struct event ev[MAX_EVENT];			//事件的相关数据
	struct socket slot[MAX_SOCKET];		//所有套接字相关的信息
	char buffer[MAX_INFO];				//open_socket发起TCP连接时，用于保存套接字的对端IP地址，如果是客户端套接字保存客户端的ip地址和端口号
	uint8_t udpbuffer[MAX_UDP_PACKAGE];	//接收UDP数据
	fd_set rfds;						//select的读描述符集合
};

struct request_open {
	int id;					//存储保存套接字相关信息的id
	int port;				//服务端端口号
	uintptr_t opaque;		//一般用于存储定位服务的handle
	char host[1];			//服务端主机名
};

struct request_send {		//发送数据请求
	int id;					//用于定位数据发送给的套接字信息id
	int sz;					//数据长度
	char * buffer;			//数据内容
};

struct request_send_udp {				//发送UDP数据请求
	struct request_send send;			//发送数据
	uint8_t address[UDP_ADDRESS_SIZE];	//udp_address[0]存协议类型，udp_address[1]，udp_address[]存端口号，剩余部分存ip地址
};

struct request_setudp {
	int id;								//用于定位数据发送给的套接字信息id
	uint8_t address[UDP_ADDRESS_SIZE];	//udp_address[0]存协议类型，udp_address[1]，udp_address[]存端口号，剩余部分存ip地址
};

struct request_close {
	int id;
	int shutdown;
	uintptr_t opaque;
};

struct request_listen {		//发起绑定内部套接字监听端口请求
	int id;					//用于定位存储套接字信息的id
	int fd;					//套接字描述符
	uintptr_t opaque;		//定位服务的handle
	char host[1];			//绑定的主机名
};

struct request_bind {	//发起绑定一个外部套接字请求
	int id;				//用于定位存储套接字信息的id
	int fd;				//外部套接字描述符
	uintptr_t opaque;	//定位服务的handle
};

struct request_start {
	int id;
	uintptr_t opaque;
};

struct request_setopt {
	int id;
	int what;
	int value;
};

struct request_udp {
	int id;
	int fd;
	int family;
	uintptr_t opaque;
};

/*
	The first byte is TYPE

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
 */

struct request_package {
	uint8_t header[8];	// 6 bytes dummy	header[6]存储type  header[7]存储len
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;			//发送TCP数据请求
		struct request_send_udp send_udp;	//发送UDP数据请求
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp set_udp;
	} u;
	uint8_t dummy[256];
};

union sockaddr_all {		//各种类型的套接字地址
	struct sockaddr s;		//各种 socket 操作传入的 sock 地址
	struct sockaddr_in v4;	//ipv4 地址的结构定义
	struct sockaddr_in6 v6;	//ipv6 地址的结构定义
};

struct send_object {
	void * buffer;
	int sz;
	void (*free_func)(void *);
};

#define MALLOC skynet_malloc
#define FREE skynet_free

struct socket_lock {	//锁
	struct spinlock *lock;
	int count;
};

//sl锁引用s锁
static inline void
socket_lock_init(struct socket *s, struct socket_lock *sl) {
	sl->lock = &s->dw_lock;
	sl->count = 0;
}

//获得锁
static inline void
socket_lock(struct socket_lock *sl) {
	if (sl->count == 0) {
		spinlock_lock(sl->lock);
	}
	++sl->count;
}

//尝试获得锁
static inline int
socket_trylock(struct socket_lock *sl) {
	if (sl->count == 0) {
		if (!spinlock_trylock(sl->lock))
			return 0;	// lock failed
	}
	++sl->count;
	return 1;
}

//释放锁
static inline void
socket_unlock(struct socket_lock *sl) {
	--sl->count;
	if (sl->count <= 0) {
		assert(sl->count == 0);
		spinlock_unlock(sl->lock);
	}
}

//初始化发送对象，如果sz>=0则object为发送缓存，sz为发送缓存的大小，
//如果sz < 0则调用 socket_server 中的 soi 函数接口从 object 中提取发送对象
//返回: 是否调用了 soi 接口的标记
static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, void *object, int sz) {
	if (sz < 0) {
		so->buffer = ss->soi.buffer(object);
		so->sz = ss->soi.size(object);
		so->free_func = ss->soi.free;
		return true;
	} else {
		so->buffer = object;
		so->sz = sz;
		so->free_func = FREE;
		return false;
	}
}

//释放写缓存中的数据存储区
static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
	if (wb->userobject) {
		ss->soi.free(wb->buffer);
	} else {
		FREE(wb->buffer);
	}
	FREE(wb);
}

//设置套接字允许发送“保持活动”包
static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

//分配新的套接字相关信息存储的id,成功则返回id,否则返回-1
static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = ATOM_INC(&(ss->alloc_id));	//原子增加
		if (id < 0) {
			id = ATOM_AND(&(ss->alloc_id), 0x7fffffff);	//归零处理
		}
		struct socket *s = &ss->slot[HASH_ID(id)];	//根据分配的id获得存储套接字的信息结构体
		if (s->type == SOCKET_TYPE_INVALID) {
			if (ATOM_CAS(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {	//将分配的socket结构体标记为已分配的状态
				s->id = id;		//用于在数组中定位该结构体元素
				// socket_server_udp_connect may inc s->udpconncting directly (from other thread, before new_fd), 
				// so reset it to 0 here rather than in new_fd.
				s->udpconnecting = 0;
				s->fd = -1;		//暂未绑定套接字
				return id;		//返回分配的id
			} else {
				// retry
				--i;
			}
		}
	}
	return -1;
}

//清除写缓存队列
static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}

//初始化全局的套接字服务信息
struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];
	poll_fd efd = sp_create();		//创建一个epoll
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}
	if (pipe(fd)) {			//产生一个读管道和一个写管道
		sp_release(efd);
		fprintf(stderr, "socket-server: create socket pair failed.\n");
		return NULL;
	}
	if (sp_add(efd, fd[0], NULL)) {		//将读管道添加到epoll中进行可读事件监听
		// add recvctrl_fd to event poll
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

	struct socket_server *ss = MALLOC(sizeof(*ss));
	ss->event_fd = efd;				//epoll句柄
	ss->recvctrl_fd = fd[0];		//读管道fd
	ss->sendctrl_fd = fd[1];		//写管道fd
	ss->checkctrl = 1;

	for (i=0;i<MAX_SOCKET;i++) {	//MAX_SOCKET=2^16	对存储套接字的相关信息结构进行初始化
		struct socket *s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;		//状态初始化为无效状态
		clear_wb_list(&s->high);			//清空高优先级写缓存队列
		clear_wb_list(&s->low);				//清空低优先级写缓存队列
	}
	ss->alloc_id = 0;						//记录当前分配可以分配的套接字信息的位置
	ss->event_n = 0;						//epoll中监听到的事件数量
	ss->event_index = 0;					//当前处理到第几个事件
	memset(&ss->soi, 0, sizeof(ss->soi));
	FD_ZERO(&ss->rfds);						//清空描述符集合
	assert(ss->recvctrl_fd < FD_SETSIZE);	//读管道是否有效

	return ss;
}

//释放写缓存队列
static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

//释放发送对象缓存
static void
free_buffer(struct socket_server *ss, const void * buffer, int sz) {
	struct send_object so;
	send_object_init(ss, &so, (void *)buffer, sz);
	so.free_func((void *)buffer);
}

//不管套接字写缓存中有没有数据强制关闭套接字，分配的套接字信息回收
static void
force_close(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {	//如果套接字已经关闭return
		return;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);	//套接字状态不为 SOCKET_TYPE_RESERVE 该状态socket结构体已被分配，但是还没有实际进行网络连接
	free_wb_list(ss,&s->high);
	free_wb_list(ss,&s->low);
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {
		sp_del(ss->event_fd, s->fd);		//删除套接字的事件监听
	}
	socket_lock(l);	//所得锁
	if (s->type != SOCKET_TYPE_BIND) {
		if (close(s->fd) < 0) {
			perror("close socket:");
		}
	}
	s->type = SOCKET_TYPE_INVALID;
	if (s->dw_buffer) {
		free_buffer(ss, s->dw_buffer, s->dw_size);
		s->dw_buffer = NULL;
	}
	socket_unlock(l);	//释放锁
}

//释放所有的信息
void 
socket_server_release(struct socket_server *ss) {
	int i;
	struct socket_message dummy;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		struct socket_lock l;
		socket_lock_init(s, &l);
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s, &l, &dummy);
		}
	}
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);
	sp_release(ss->event_fd);
	FREE(ss);
}

//检查写缓存队列是否为空
static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}

//将产生的套接字添加到分配的套接字信息结构中
//add为true添加到epoll中可读事件监听，否则不添加，成功返回socket结构体
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[HASH_ID(id)];	//获得id对应的套接字信息结构体
	assert(s->type == SOCKET_TYPE_RESERVE);

	if (add) {
		if (sp_add(ss->event_fd, fd, s)) {	//添加到epoll对套接字的可读事件的监听
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;					//定位存储套接字信息id
	s->fd = fd;					//套接字描述符
	s->protocol = protocol;		//协议类型
	s->p.size = MIN_READ_BUFFER;//从套接字中一次性最少读取的字节数
	s->opaque = opaque;			//定位服务的handle
	s->wb_size = 0;				//写缓存大小
	s->warn_size = 0;			//写缓存阈值
	check_wb_list(&s->high);	//初始化高优先级写缓存
	check_wb_list(&s->low);		//初始化低优先级写缓存
	spinlock_init(&s->dw_lock);	//锁
	s->dw_buffer = NULL;		//保存未发送完，或不成功的数据
	s->dw_size = 0;				//发送不成功的数据的大小
	return s;
}

// return -1 when connecting
//发起一个TCP连接请求，request发起请求的信息，包括id，handle，服务端主机名host，端口号port
//创建套接字，设置套接字为非阻塞，发起连接服务端请求，将套接字添加到epoll中监听，
//如果正在连接中则设置套接字的状态为 SOCKET_TYPE_CONNECTING，并监听套接字的可读可写事件，否则保存IP地址
//返回-1表示正在连接中，返回SOCKET_OPEN表示已连接成功，返回SOCKET_ERR失败
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result) {
	int id = request->id;				//定位存储套接字信息的id
	result->opaque = request->opaque;	//定位服务的handle
	result->id = id;					//定位存储套接字信息的id
	result->ud = 0;
	result->data = NULL;
	struct socket *ns;
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	sprintf(port, "%d", request->port);		//端口号
	memset(&ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;			//协议簇类型
	ai_hints.ai_socktype = SOCK_STREAM;		//提供面向连接的稳定数据传输，即TCP协议
	ai_hints.ai_protocol = IPPROTO_TCP;		//TCP协议

	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {
		result->data = (void *)gai_strerror(status);
		goto _failed;
	}
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );	//创建套接字
		if ( sock < 0 ) {
			continue;
		}
		socket_keepalive(sock);		//设置套接字允许发送“保持活动”包
		sp_nonblocking(sock);		//设置套接字为非阻塞
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);	//连接服务端
		if ( status != 0 && errno != EINPROGRESS) {	//由于是非阻塞的，status为-1和errno=EINPROGRESS表示连接还在进行中
			close(sock);
			sock = -1;
			continue;
		}
		break;
	}

	if (sock < 0) {
		result->data = strerror(errno);	//获得错误信息
		goto _failed;
	}

	//将产生的套接字添加到分配的套接字信息结构中，并添加到epoll中监听
	ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	if(status == 0) {	//为0说明已经连接
		ns->type = SOCKET_TYPE_CONNECTED;	//套接字状态改为已经连接
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {	//保存套接字的对端的ip地址
			result->data = ss->buffer;	//保存IP地址
		}
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {		//正在连接中
		ns->type = SOCKET_TYPE_CONNECTING;	//套接字状态为正在连接中
		sp_write(ss->event_fd, ns->fd, ns, true);	//将套接字的监听事件改为可读可写
	}

	freeaddrinfo( ai_list );	//释放ai_list
	return -1;
_failed:
	freeaddrinfo( ai_list );	//释放ai_list
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;	//归还分配的套接字信息结构体
	return SOCKET_ERR;
}

//TCP发送指定写缓存队列的数据，一个一个节点的数据进行发送，
//发送过程中碰到中断继续执行，如果一个节点只发送成功一部分数据，或者没发送成功返回-1
//发送不成功的其他情况则关闭套接字，返回SOCKET_CLOSE
static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;		//获得写缓存队列的头节点
		for (;;) {
			ssize_t sz = write(s->fd, tmp->ptr, tmp->sz);	//发送数据
			if (sz < 0) {						//数据发送不成功
				switch(errno) {
				case EINTR:						//中断
					continue;
				case AGAIN_WOULDBLOCK:			//说明发送数据的缓存区已满
					return -1;
				}
				force_close(ss,s,l,result);		//关闭套接字
				return SOCKET_CLOSE;
			}
			s->wb_size -= sz;					//随着发送出去的数据减少记录的缓存区数据大小
			if (sz != tmp->sz) {				//如果数据只发送出去一部分，说明发送数据的缓存区已满
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			break;
		}
		list->head = tmp->next;					//继续发送下一个节点数据
		write_buffer_free(ss,tmp);				//释放已经发送的节点缓存
	}
	list->tail = NULL;

	return -1;
}

///从 udp_address 中提取出套接字地址, 此函数支持 PROTOCOL_UDP 和 PROTOCOL_UDPv6 两种形式的地址.
//头字节是协议类型, 可能为 UDP 或者 UDPv6 两种, 接下来是两个字节是端口号, 剩下的为 ip , ip 可能是
//ipv4(32bit) 也可能是 ipv6(128bit).
//成功返回转化成标准形式后的地址长度,否则返回0
static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
	int type = (uint8_t)udp_address[0];	//获得协议类型
	if (type != s->protocol)
		return 0;
	uint16_t port = 0;
	memcpy(&port, udp_address+1, sizeof(uint16_t));	//复制端口号
	switch (s->protocol) {
	case PROTOCOL_UDP:
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));	// ipv4 address is 32 bits
		return sizeof(sa->v4);
	case PROTOCOL_UDPv6:
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr)); // ipv6 address is 128 bits
		return sizeof(sa->v6);
	}
	return 0;
}

//UDP发送写缓存队列中的数据
static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;	//获取节点数据
		union sockaddr_all sa;
		socklen_t sasz = udp_socket_address(s, tmp->udp_address, &sa);	//获得标准的地址长度，地址存于sa
		int err = sendto(s->fd, tmp->ptr, tmp->sz, 0, &sa.s, sasz);		//发送数据
		if (err < 0) {
			switch(errno) {
			case EINTR:
			case AGAIN_WOULDBLOCK:
				return -1;
			}
			fprintf(stderr, "socket-server : udp (%d) sendto error %s.\n",s->id, strerror(errno));
			return -1;
/*			// ignore udp sendto error
			
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = NULL;

			return SOCKET_ERR;
*/
		}

		s->wb_size -= tmp->sz;
		list->head = tmp->next;			//获得下一个节点数据
		write_buffer_free(ss,tmp);		//释放已经发送的数据
	}
	list->tail = NULL;

	return -1;
}

//发送写缓存队列中的数据，根据套接字信息中的协议类型来判断是TCP还UDP
static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result) {
	if (s->protocol == PROTOCOL_TCP) {
		return send_list_tcp(ss, s, list, l, result);	//TCP发送队列中的数据
	} else {
		return send_list_udp(ss, s, list, result);
	}
}

//用来判断写缓存队列的头节点数据是否只发送一部分，只发送一部分返回true,否则返回false
static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;	//获得写缓存队列的第一个节点数据
	if (wb == NULL)		
		return 0;	//如果第一个节点数据为NULL

	//判断发送数据的起始指针是否和调用者传递过来的数据缓存的起始指针相等
	return (void *)wb->ptr != wb->buffer;	
}

static void
raise_uncomplete(struct socket * s) {
	struct wb_list *low = &s->low;
	struct write_buffer *tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	struct wb_list *high = &s->high;
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

//检查套接字的写缓冲是否是空的, 写缓冲包括高优先级和低优先级两条队列
static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
//发送写缓存队列中的数据，检查低优先级队列的头节点是否只发送一部分数据
//发送高优先级写缓存队列中的数据，当高优先级的数据发送完则发送低优先级的，
//如果低优先级的头节点只发送一个部分数据，则将其移到高优先级队列中
//写缓存队列数据都发送完后将套接字可写监听事件去掉，如果套接字处于半关闭状态则关闭套接字 ，返回SOCKET_CLOSE
//写缓存超出阈值则返回SOCKET_WARNING
static int
send_buffer_(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	assert(!list_uncomplete(&s->low));	//检查低优先级队列的头节点是否只发送一部分数据
	// step 1
	if (send_list(ss,s,&s->high,l,result) == SOCKET_CLOSE) {	//发送高优先级写缓存队列中的数据，根据套接字信息中的协议类型来判断是TCP还UDP
		return SOCKET_CLOSE;	//套接字已经关闭
	}
	if (s->high.head == NULL) {
		// step 2 如果高优先级队列中的数据已经发送完毕，下一步发送低优先级的
		if (s->low.head != NULL) {
			if (send_list(ss,s,&s->low,l,result) == SOCKET_CLOSE) {	//发送低优先级写缓存队列中的数据，根据套接字信息中的协议类型来判断是TCP还UDP
				return SOCKET_CLOSE;	//套接字已经关闭
			}
			// step 3
			if (list_uncomplete(&s->low)) {	
				//如果低优先级队列的头节点只成功发送一部数据
				raise_uncomplete(s);	//将低优先级的头节点数据移到高优先级的头节点中
				return -1;
			}
		} 
		// step 4
		assert(send_buffer_empty(s) && s->wb_size == 0);	//检查写缓存队列的数据是否都发送完了
		sp_write(ss->event_fd, s->fd, s, false);			//修改套接字的监听事件为可读		

		if (s->type == SOCKET_TYPE_HALFCLOSE) {				//如果套接字状态为半关闭状态则关闭套接字
				force_close(ss, s, l, result);				//关闭套接字
				return SOCKET_CLOSE;						//返回套接字已关闭
		}
		if(s->warn_size > 0){								//如果写缓存超过阈值
				s->warn_size = 0;							//清零
				result->opaque = s->opaque;					
				result->id = s->id;
				result->ud = 0;
				result->data = NULL;
				return SOCKET_WARNING;						//返回超出阈值警告
		}
	}

	return -1;
}

//发送写缓存中的数据,先判断之前是否有数据未完全发送存入了s->dw_buffer，则将s->dw_buffer中的数据添加到高优先级队列的头部
//从写缓存队列中发送数据，按照先高优先级后低优先级
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	if (!socket_trylock(l))		//尝试获得锁
		return -1;	// blocked by direct write, send later. 获得锁失败
	if (s->dw_buffer) {		//判断套接字中是否有上一次未发送完，或者上一次发送失败的数据
		// add direct write buffer before high.head
		struct write_buffer * buf = MALLOC(SIZEOF_TCPBUFFER);	//分配写缓存
		struct send_object so;
		buf->userobject = send_object_init(ss, &so, (void *)s->dw_buffer, s->dw_size);	//初始化发送对象，返回是否使用socket_server中的soi接口
		buf->ptr = (char*)so.buffer+s->dw_offset;	//发送数据的起始指针, 会随着不断写入 socket 而向后移动
		buf->sz = so.sz - s->dw_offset;				//发送数据的大小
		buf->buffer = (void *)s->dw_buffer;			//调用者传递过来的缓存, 从中可以提取出发送数据, 最后需要回收内存
		s->wb_size+=buf->sz;						//增加增加写缓存的大小
		//将s->dw_buffer中的数据添加到写缓存的高优先级队列的头部位置
		if (s->high.head == NULL) {
			s->high.head = s->high.tail = buf;
			buf->next = NULL;
		} else {
			buf->next = s->high.head;
			s->high.head = buf;
		}
		s->dw_buffer = NULL;
	}
	int r = send_buffer_(ss,s,l,result);	//发送写缓存队列中的数据
	socket_unlock(l);		//释放锁

	return r;
}

//将发送数据添加到缓存队列中
static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size) {
	struct write_buffer * buf = MALLOC(size);
	struct send_object so;
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);	//初始化发送对象，返回是否使用socket_server中的soi接口
	buf->ptr = (char*)so.buffer;	//发送数据的起始指针, 会随着不断写入 socket 而向后移动
	buf->sz = so.sz;				//发送数据的大小, 会随着不断写入 socket 而减小
	buf->buffer = request->buffer;	//调用者传递过来的缓存, 从中可以提取出发送数据, 最后需要回收内存
	buf->next = NULL;				//处于 wb_list 中的下一个写缓存
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
	return buf;
}

//将一个 UDP 发送数据添加到相应的优先级队列中去, 相应的对端地址udp_address会被添加到写缓冲中去, 并增加写缓冲的大小s->wb_size
static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
	struct write_buffer *buf = append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER);
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);	//添加相应的对端地址udp_address
	s->wb_size += buf->sz;
}

//将发送TCP数据添加到高优先级的缓存队列中，增加socket结构中的写入缓存的大小s->wb_size
static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, SIZEOF_TCPBUFFER);
	s->wb_size += buf->sz;
}

//将发送TCP数据添加到低优先级的缓存队列中，增加socket结构中的写入缓存的大小s->wb_size
static inline void
append_sendbuffer_low(struct socket_server *ss,struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, SIZEOF_TCPBUFFER);
	s->wb_size += buf->sz;
}


/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
//向套接字中发送数据，TCP和UDP都可以处理，判断套接字是否可以发送数据，如果写缓存队列为空则将数据直接添加到高优先级队列
//否则根据优先级和协议类型添加到相应的写缓存队列，
//缓存超出阈值返回SOCKET_WARNING，否则返回-1，
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;		//定位数据给的套接字信息id
	struct socket * s = &ss->slot[HASH_ID(id)];	//获得数据发给的套接字信息
	struct send_object so;
	send_object_init(ss, &so, request->buffer, request->sz);	//初始化发送对象
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 	//无效的套接字信息
		|| s->type == SOCKET_TYPE_HALFCLOSE				//套接字已经处于半关闭状态
		|| s->type == SOCKET_TYPE_PACCEPT) {			//套接字还未进入事件监听状态
		so.free_func(request->buffer);					//释放发送对象
		return -1;
	}
	if (s->type == SOCKET_TYPE_PLISTEN || s->type == SOCKET_TYPE_LISTEN) {	//用于侦听端口的套接字是不会发送数据的
		fprintf(stderr, "socket-server: write to listen fd %d.\n", id);
		so.free_func(request->buffer);
		return -1;
	}
	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {	//检查套接字的写缓冲是否是空的,并且套接字连接成功, 可以发送信息
		if (s->protocol == PROTOCOL_TCP) {		//如果为TCP协议，
			append_sendbuffer(ss, s, request);	//如果两个优先级的缓存队列都为空，则不管优先级的高低，直接添加到高优先级缓存队列中
		} else {								//如果是UDP协议
			// udp
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;	//从套接字信息中获得地址信息
			}
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);	//将udp_address中的地址转换为标准形式存入sa，返回地址长度，否则为0
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);	//发送数据
			if (n != so.sz) {	//如果没发送完
				append_sendbuffer_udp(ss,s,priority,request,udp_address);	//添加到缓存
			} else {	//发送完了
				so.free_func(request->buffer);	//释放
				return -1;
			}
		}
		sp_write(ss->event_fd, s->fd, s, true);		//修改该套接字fd监听的事件为可读可写
	} else {	//缓存中有数据
		if (s->protocol == PROTOCOL_TCP) {	//TCP协议
			if (priority == PRIORITY_LOW) {	//添加到底优先级缓存队列
				append_sendbuffer_low(ss, s, request);
			} else {	//添加到高优先级缓存队列
				append_sendbuffer(ss, s, request);
			}
		} else {	//UDP协议
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;	//获得地址
			}
			append_sendbuffer_udp(ss,s,priority,request,udp_address);	//添加到对应优先级的缓存队列
		}
	}
	if (s->wb_size >= WARNING_SIZE && s->wb_size >= s->warn_size) {	//缓存超过阈值大小
		s->warn_size = s->warn_size == 0 ? WARNING_SIZE *2 : s->warn_size*2;	//阈值翻倍
		result->opaque = s->opaque;		//定位服务的handle
		result->id = s->id;				//定位存储套接字信息的id
		result->ud = s->wb_size%1024 == 0 ? s->wb_size/1024 : s->wb_size/1024 + 1;
		result->data = NULL;
		return SOCKET_WARNING;
	}
	return -1;
}

//对管道中'L'命令的处理，将已经监听的套接字添加到套接字信息结构中，但不添加到epoll中监听事件
//套接字的状态由SOCKET_TYPE_RESERVE变为SOCKET_TYPE_PLISTEN，成功返回-1，否则返回SOCKET_ERR
static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
	int id = request->id;			//定位存储套接字信息id
	int listen_fd = request->fd;	//套接字
	//将产生的套接字添加到分配的套接字信息结构中，不添加到epoll中监听，成功返回socket结构体
	struct socket *s = new_fd(ss, id, listen_fd, PROTOCOL_TCP, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	s->type = SOCKET_TYPE_PLISTEN;	//设置套接字状态为：已监听端口，但不监听套接字上的事件
	return -1;
_failed:							//失败的处理
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = "reach skynet socket number limit";
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERR;
}

//判断套接字中是否有数据要发送，没有则返回true，否则返回false
static inline int
nomore_send_data(struct socket *s) {
	return send_buffer_empty(s) && s->dw_buffer == NULL;
}

//关闭套接字，如果套接字信息不存在，则返回SOCKET_CLOSE
//如果套接字写缓存中还有数据，则先将数据发送完，如果不是强制关闭，则发送完数据后将套接字状态设为SOCKET_TYPE_HALFCLOSE
//如果是强制关闭，或者写缓存中没有数据则直接关闭，返回SOCKET_CLOSE
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;	//定位存储套接字信息的id
	struct socket * s = &ss->slot[HASH_ID(id)];	//根据id获得相应的套接字信息
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {	//如果套接字信息已经不存在，则说明套接字已经关闭
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}
	struct socket_lock l;
	socket_lock_init(s, &l);
	if (!nomore_send_data(s)) {		//判断套接字中是否还有数据要发送
		//如果套接字中还有数据要发送
		int type = send_buffer(ss,s,&l,result);	//发送写缓存中的数据
		// type : -1 or SOCKET_WARNING or SOCKET_CLOSE, SOCKET_WARNING means nomore_send_data
		if (type != -1 && type != SOCKET_WARNING)
			return type;	//说明还有数据没发送完
	}
	if (request->shutdown || nomore_send_data(s)) {	//如果是强制关闭，或者写缓存中没有数据，则直接关闭
		force_close(ss,s,&l,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	s->type = SOCKET_TYPE_HALFCLOSE;	//标记套接字为半关闭状态, 虽然套接字本身没有关闭, 但是已经不能往里边添加信息了, 最终会在清空写缓冲的情况下关闭

	return -1;
}

//绑定一个由外部生成的套接字，并添加到epoll监听读事件，设置套接字为非阻塞模式，改变状态为SOCKET_TYPE_BIND
//成功返回SOCKET_OPEN，否则为SOCKET_ERR
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;				//定位存储套接字信息的id
	result->id = id;
	result->opaque = request->opaque;	//定位服务的handle
	result->ud = 0;
	//将指定套接字添加到分配的套接字信息结构中，并添加到epoll中监听
	struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
	if (s == NULL) {
		result->data = "reach skynet socket number limit";
		return SOCKET_ERR;
	}
	sp_nonblocking(request->fd);		//将套接字设置为非阻塞模式
	s->type = SOCKET_TYPE_BIND;			//套接字的状态为绑定
	result->data = "binding";
	return SOCKET_OPEN;
}

//开始添加套接字到epoll进行可读事件监听，改变套接字的状态为 SOCKET_TYPE_CONNECTED 或 SOCKET_TYPE_LISTEN
//成功返回SOCKET_OPEN，否则返回-1或者SOCKET_ERR
static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;		//定位套接字相关信息id
	result->id = id;			
	result->opaque = request->opaque;	//定位服务的handle
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[HASH_ID(id)];	//获得套接字相关的信息
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {	//如果套接字信息的状态为未分配或id不相同
		result->data = "invalid socket";
		return SOCKET_ERR;	//放回错误类型
	}
	struct socket_lock l;
	socket_lock_init(s, &l);	//锁l引用s中的锁
	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {	//如果套接字为没添加到epoll进行事件监听
		if (sp_add(ss->event_fd, s->fd, s)) {	//添加套接字s->fd到epoll进行可读事件的监听，成功返回0，失败返回1
			force_close(ss, s, &l, result);
			result->data = strerror(errno);
			return SOCKET_ERR;
		}
		//套接字状态改变SOCKET_TYPE_PACCEPT->SOCKET_TYPE_CONNECTED状态，否则SOCKET_TYPE_PLISTEN->SOCKET_TYPE_LISTEN状态
		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;
	} else if (s->type == SOCKET_TYPE_CONNECTED) {
		// todo: maybe we should send a message SOCKET_TRANSFER to s->opaque
		s->opaque = request->opaque;
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	// if s->type == SOCKET_TYPE_HALFCLOSE , SOCKET_CLOSE message will send later
	return -1;
}

//设置套接字的选项，选项的层次在 IPPROTO_TCP 上 , 设置的键和值都是 int 类型的, 
//目前仅用于设置套接字的 TCP_NODELAY 选项，request->what为1禁止发送合并的Nagle算法
static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return;
	}
	int v = request->value;
	setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));	//设置套接字的 TCP_NODELAY 选项，request->what为1禁止发送合并的Nagle算法
}

//读取读管道中指定长度的内容，例如读命令，以及读命令携带的数据
static void
block_readpipe(int pipefd, void *buffer, int sz) {
	for (;;) {
		int n = read(pipefd, buffer, sz);
		if (n<0) {
			if (errno == EINTR)	//中断
				continue;
			fprintf(stderr, "socket-server : read pipe error %s.\n",strerror(errno));
			return;
		}
		// must atomic read from a pipe
		assert(n == sz);
		return;
	}
}

//用于检查是否有命令，通过一个select监听读管道fd是否有可读事件，有则返回1，否则返回0，
static int
has_cmd(struct socket_server *ss) {
	struct timeval tv = {0,0};
	int retval;

	FD_SET(ss->recvctrl_fd, &ss->rfds);		//将管道读fd加入读描述符集合

	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);	//创建一个select，不阻塞
	if (retval == 1) {
		return 1;
	}
	return 0;
}

//添加产生的套接字到分配的套接字信息结构中，并添加可读事件的监听，修改套接字的状态为 SOCKET_TYPE_CONNECTED
//添加成功后不关联对端ip地址信息
static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
	int id = udp->id;	//定位套接字信息的id
	int protocol;
	if (udp->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}
	//将产生的套接字添加到分配的套接字信息结构中，并添加到epoll中进行可读事件监听
	struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
	if (ns == NULL) {
		close(udp->fd);
		ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
		return;
	}
	ns->type = SOCKET_TYPE_CONNECTED;	//改变套接字的状态为 SOCKET_TYPE_CONNECTED
	memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}

//设置指定套接字信息中的ip地址，前提是套接字信息有效及协议类型匹配，此过程中s->udpconnecting大于0
//协议不匹配返回SOCKET_ERR，否则返回-1
static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
	int id = request->id;	//定位套接字信息id
	struct socket *s = &ss->slot[HASH_ID(id)];	//获得套接字信息
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {		//判断套接字信息是否有效
		return -1;
	}
	int type = request->address[0];		//获得请求信息中的协议类型
	if (type != s->protocol) {			//对比套接字信息中的协议类型和请求信息中的协议类型是否相同
		// protocol mismatch
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		result->data = "protocol mismatch";

		return SOCKET_ERR;
	}
	if (type == PROTOCOL_UDP) {			//根据不同类型的UDP协议关联ip地址
		memcpy(s->p.udp_address, request->address, 1+2+4);	// 1 type, 2 port, 4 ipv4
	} else {
		memcpy(s->p.udp_address, request->address, 1+2+16);	// 1 type, 2 port, 16 ipv6
	}
	ATOM_DEC(&s->udpconnecting);	//递减udpconnecting
	return -1;
}

// return type
//从读管道中取出相应的命令及附带的数据进行处理，result保存各个命令处理的结果信息，
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
	int fd = ss->recvctrl_fd;
	// the length of message is one byte, so 256+8 buffer size is enough.
	uint8_t buffer[256];	//数据内容缓存
	uint8_t header[2];		//命令缓存
	block_readpipe(fd, header, sizeof(header));	//读取读管道中指定长度的命令内容
	int type = header[0];	//命令的类型
	int len = header[1];	//命令附带的数据长度
	block_readpipe(fd, buffer, len);	//读取读管道中指定长度的数据内容
	// ctrl command only exist in local fd, so don't worry about endian.
	switch (type) {
	case 'S':	//开始添加套接字到epoll进行可读事件监听，改变套接字的状态为 SOCKET_TYPE_CONNECTED 或 SOCKET_TYPE_LISTEN
		return start_socket(ss,(struct request_start *)buffer, result);	//成功返回SOCKET_OPEN，否则返回-1或者SOCKET_ERR
	case 'B':	//绑定一个由外部生成的套接字，并添加到epoll监听读事件，设置套接字为非阻塞模式，改变状态为SOCKET_TYPE_BIND
		return bind_socket(ss,(struct request_bind *)buffer, result);	//成功返回SOCKET_OPEN，否则为SOCKET_ERR
	case 'L':	//将已经监听的套接字添加到套接字信息结构中，但不添加到epoll中监听事件，套接字的状态由SOCKET_TYPE_RESERVE变为SOCKET_TYPE_PLISTEN
		return listen_socket(ss,(struct request_listen *)buffer, result);	//成功返回-1，否则返回SOCKET_ERR
	case 'K':	//关闭套接字，如果是强制关闭或没有数据则直接关闭，否则如果有数据则先发送完数据，再将套接字状态设置为SOCKET_TYPE_HALFCLOSE，
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':	//发起TCP连接服务端请求
		return open_socket(ss, (struct request_open *)buffer, result);	//返回-1表示正在连接中，返回SOCKET_OPEN表示已连接成功，返回SOCKET_ERR失败
	case 'X':	//整个套接字服务退出
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':	//向套接字发送数据，将数据添加到高优先级写缓存中
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_HIGH, NULL);
	case 'P':	//向套接字发送数据，将数据添加到低优先级写缓存中
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_LOW, NULL);
	case 'A': {	//UDP协议，向套接字发送数据，未发送完添加到高优先级写缓存队列中
		struct request_send_udp * rsu = (struct request_send_udp *)buffer;
		return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
	}
	case 'C':	//设置指定套接字信息中的ip地址，前提是套接字信息有效及协议类型匹配，此过程中s->udpconnecting大于0
		return set_udp_address(ss, (struct request_setudp *)buffer, result);	//协议不匹配返回SOCKET_ERR，否则返回-1
	case 'T':	//设置套接字的选项，选项的层次在 IPPROTO_TCP 上 , 设置的键和值都是 int 类型的, 
		setopt_socket(ss, (struct request_setopt *)buffer);	//目前仅用于设置套接字的 TCP_NODELAY 选项，request->what为1禁止发送合并的Nagle算法
		return -1;
	case 'U':	//添加产生的套接字到分配的套接字信息结构中，并添加可读事件的监听，修改套接字的状态为 SOCKET_TYPE_CONNECTED
		add_udp_socket(ss, (struct request_udp *)buffer);	//添加成功后不关联对端ip地址信息
		return -1;
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

// return -1 (ignore) when error
//TCP套接字接收数据，读取数据不成功进行相应的处理，根据套接字读取的数据长度对下次可读取的最大数据长度进行调整
//成功返回SOCKET_DATA，失败返回SOCKET_ERR或SOCKET_CLOSE，返回-1表示忽略
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message * result) {
	int sz = s->p.size;		//获取本次读取数据的最大长度
	char * buffer = MALLOC(sz);	//分配缓存数据内存
	int n = (int)read(s->fd, buffer, sz);	//读取数据
	if (n<0) {			//如果读取不成功
		FREE(buffer);	//释放内存
		switch(errno) {	//出错原因
		case EINTR:		//中断
			break;
		case AGAIN_WOULDBLOCK:	//说明接收数据的缓存区已满
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:	//其他错误，强制关闭套接字
			// close when error
			force_close(ss, s, l, result);	//强制关闭套接字
			result->data = strerror(errno);
			return SOCKET_ERR;
		}
		return -1;
	}
	if (n==0) {		//没有数据可读
		FREE(buffer);
		force_close(ss, s, l, result);
		return SOCKET_CLOSE;
	}

	if (s->type == SOCKET_TYPE_HALFCLOSE) {	//套接字处于半关闭状态，则丢弃数据
		// discard recv data
		FREE(buffer);
		return -1;
	}

	if (n == sz) {			//读取的数据达到分配的缓存最大值
		s->p.size *= 2;		//本套接字下次读取数据的最大长度翻倍
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {	//读取到的数据长度比最大长度的1/2还要小，但大于最小限制
		s->p.size /= 2;		//缩小套接字下次读取数据的最大长度为本次的1/2
	}

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;
	return SOCKET_DATA;
}

//将套接字地址的存放在udp_address数组中，并返回地址的长度
static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t * udp_address) {
	int addrsz = 1;
	udp_address[0] = (uint8_t)protocol;
	if (protocol == PROTOCOL_UDP) {
		memcpy(udp_address+addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
		addrsz += sizeof(sa->v4.sin_port);
		memcpy(udp_address+addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
		addrsz += sizeof(sa->v4.sin_addr);
	} else {
		memcpy(udp_address+addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
		addrsz += sizeof(sa->v6.sin6_port);
		memcpy(udp_address+addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
		addrsz += sizeof(sa->v6.sin6_addr);
	}
	return addrsz;
}

//接收UDP数据，数据存入ss->udpbuffer中，result->data返回数据+IP地址的信息
//接收成功返回SOCKET_UDP，错误返回SOCKET_ERR，返回-1忽略
static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message * result) {
	union sockaddr_all sa;
	socklen_t slen = sizeof(sa);
	int n = recvfrom(s->fd, ss->udpbuffer,MAX_UDP_PACKAGE,0,&sa.s,&slen);	//接收数据
	if (n<0) {			//错误处理
		switch(errno) {
		case EINTR:
		case AGAIN_WOULDBLOCK:
			break;
		default:
			// close when error
			force_close(ss, s, l, result);
			result->data = strerror(errno);
			return SOCKET_ERR;
		}
		return -1;
	}
	uint8_t * data;
	if (slen == sizeof(sa.v4)) {
		if (s->protocol != PROTOCOL_UDP)
			return -1;
		data = MALLOC(n + 1 + 2 + 4);
		gen_udp_address(PROTOCOL_UDP, &sa, data + n);
	} else {
		if (s->protocol != PROTOCOL_UDPv6)
			return -1;
		data = MALLOC(n + 1 + 2 + 16);
		gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
	}
	memcpy(data, ss->udpbuffer, n);

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = (char *)data;	//数据+IP地址

	return SOCKET_UDP;
}

//有错误则先处理错误，将套接字的状态改为 SOCKET_TYPE_CONNECTING -> SOCKET_TYPE_CONNECTED，监听事件改为可读事件
//保存套接字对端的ip地址，成功返回SOCKET_OPEN，否则返回SOCKET_ERR
static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	int error;
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  //获得套接字s->fd上的获取错误状态并清除，无错误发生返回0
	if (code < 0 || error) {  
		force_close(ss,s,l, result);	//关闭套接字，回收分配的套接字信息
		if (code >= 0)
			result->data = strerror(error);
		else
			result->data = strerror(errno);
		return SOCKET_ERR;
	} else {
		s->type = SOCKET_TYPE_CONNECTED;	//改变套接字的状态为 SOCKET_TYPE_CONNECTING -> SOCKET_TYPE_CONNECTED
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		if (nomore_send_data(s)) {			//检查写缓存中有没有数据发送
			sp_write(ss->event_fd, s->fd, s, false);	//没有数据，将套接字的事件监听改为监听可读事件
		}
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if (getpeername(s->fd, &u.s, &slen) == 0) {	//获取与该套接字相连的IP地址
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;	//根据不同的协议获得地址
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {	//保存套接字的对端ip地址到ss->buffer
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

// return 0 when failed, or -1 when file limit
//监听服务服务端套接字等待客户端的连接请求，分配新的套接字信息存储id用于存储客户端套接字信息，
//设置客户端套接字选项允许发送"保持活动"包，并将其设置为非阻塞，将客户端套接字添加到分配的套接字信息中，设置为不监听事件
//此时客户端套接字的状态为 SOCKET_TYPE_PACCEPT 并将客户端的ip和端口号存入socket_server中
//成功返回1，返回-1说明打开的描述符超出限制，返回0表示未能正常连接
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int client_fd = accept(s->fd, &u.s, &len);	//等待客户端的连接请求
	if (client_fd < 0) {						//发生错误
		if (errno == EMFILE || errno == ENFILE) {	//表示打开的描述符超出限制
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = strerror(errno);
			return -1;
		} else {
			return 0;
		}
	}
	int id = reserve_id(ss);	//分配新的套接字相关信息存储的id,成功则返回id,否则返回-1
	if (id < 0) {
		close(client_fd);
		return 0;
	}
	socket_keepalive(client_fd);	//设置套接字允许发送“保持活动”包
	sp_nonblocking(client_fd);		//设置套接字为非阻塞
	//将产生的套接字添加到分配的套接字信息结构中，但不添加到epoll中监听
	struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);	
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}
	ns->type = SOCKET_TYPE_PACCEPT;		//已经接受了客户端的连接, 但是没有添加到epoll监听事件, 当调用 start_socket 才变成 CONNECTED
	result->opaque = s->opaque;			//定位服务的handle
	result->id = s->id;					//定位存储套接字信息的id
	result->ud = id;
	result->data = NULL;

	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];
	if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		snprintf(ss->buffer, sizeof(ss->buffer), "%s:%d", tmp, sin_port);	//保存客户端的ip地址和端口号
		result->data = ss->buffer;
	}

	return 1;
}

//如果处理完命令后的返回值为关闭套接字或错误，则将该套接字相关的监听事件清除
static inline void 
clear_closed_event(struct socket_server *ss, struct socket_message * result, int type) {
	if (type == SOCKET_CLOSE || type == SOCKET_ERR) {	//如果处理命令后的返回值为SOCKET_CLOSE或SOCKET_ERR
		int id = result->id;
		int i;
		for (i=ss->event_index; i<ss->event_n; i++) {
			struct event *e = &ss->ev[i];
			struct socket *s = e->s;
			if (s) {
				if (s->type == SOCKET_TYPE_INVALID && s->id == id) {
					e->s = NULL;
					break;
				}
			}
		}
	}
}

// return type
//检查读管道中的命令，有命令则读取命令及携带的数据进行处理，如果各个套接字上没有事件需要处理则
//监听所有套接字注册的事件，等待事件触发，如果有事件需要处理，则一个一个事件进行处理，
//more为1表示上次的事件还没处理完，0表示上次的事件都处理完了
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {
	for (;;) {
		if (ss->checkctrl) {	//判断是否需要检查读管道中的命令，默认需要
			if (has_cmd(ss)) {	//检查读管道上是否有命令可读取，有则返回1，否则返回0
				int type = ctrl_cmd(ss, result);	//从读管道上读取相应的命令，并对其数据进行处理，返回相应的处理结果
				if (type != -1) {
					clear_closed_event(ss, result, type);	//清除掉该套接字相关的监听事件
					return type;
				} else 		//type=-1说明是一个过渡状态
					continue;
			} else {
				ss->checkctrl = 0;
			}
		}
		if (ss->event_index == ss->event_n) {
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);	//等待epoll上监听的事件触发，阻塞，返回触发事件的数量
			ss->checkctrl = 1;
			if (more) {
				*more = 0;			//标记上一次的事件都处理完了
			}
			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				if (errno == EINTR) {	//判断是否是中断
					continue;
				}
				return -1;
			}
		}
		struct event *e = &ss->ev[ss->event_index++];	//从监听到的事件中取出一个事件
		struct socket *s = e->s;	//取出事件附带的socket信息
		if (s == NULL) {			//开始时发送的是管道消息
			// dispatch pipe message at beginning
			continue;
		}
		struct socket_lock l;
		socket_lock_init(s, &l);	//锁l引用套接字数据中的锁
		switch (s->type) {
		case SOCKET_TYPE_CONNECTING:	//套接字正在连接中, 但是还没有连接上, 此时还不能传送信息
			return report_connect(ss, s, &l, result);	//改变套接字的状态为SOCKET_TYPE_CONNECTED ，监听事件改为可读事件，保存对端ip地址
		case SOCKET_TYPE_LISTEN: {		//套接字处于监听状态，说明有客户端发送连接请求
			int ok = report_accept(ss, s, result);		//等待客户端的连接请求
			if (ok > 0) {
				return SOCKET_ACCEPT;	//正常连接
			} if (ok < 0 ) {
				return SOCKET_ERR;		//描述符超出限制
			}
			// when ok == 0, retry
			break;
		}
		case SOCKET_TYPE_INVALID:		//套接字信息不存在
			fprintf(stderr, "socket-server: invalid socket\n");
			break;
		default:		//如果套接字信息存储着，已经正常连接好，并且不是客户端请求连接服务端，则进行下列处理
			if (e->read) {		//可读事件
				int type;
				if (s->protocol == PROTOCOL_TCP) {	//如果是TCP通信
					type = forward_message_tcp(ss, s, &l, result);	//读取数据
				} else {							//如果是UDP通信
					type = forward_message_udp(ss, s, &l, result);	//接收UDP数据
					if (type == SOCKET_UDP) {		//如果接收到UDP数据
						// try read again
						--ss->event_index;			//下次还会尝试去读取一次数据
						return SOCKET_UDP;
					}
				}
				if (e->write && type != SOCKET_CLOSE && type != SOCKET_ERR) {
					// Try to dispatch write message next step if write flag set.
					e->read = false;
					--ss->event_index;				//下次会再尝试去写数据
				}
				if (type == -1)
					break;				
				return type;
			}
			if (e->write) {		//可写事件
				int type = send_buffer(ss, s, &l, result);	//发送套接字中写缓存中的数据
				if (type == -1)
					break;
				return type;
			}
			if (e->error) {		//错误事件
				// close when error
				int error;
				socklen_t len = sizeof(error);  
				int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  //获得套接字s->fd上的获取错误状态并清除，无错误发生返回0
				const char * err = NULL;
				if (code < 0) {
					err = strerror(errno);
				} else if (error != 0) {
					err = strerror(error);
				} else {
					err = "Unknown error";
				}
				force_close(ss, s, &l, result);		//强制关闭套接字
				result->data = (char *)err;			//记录错误原因
				return SOCKET_ERR;					//返回错误
			}
			break;
		}
	}
}

//将套接字的命令写入到管道中去, 通过 recvctrl_fd 文件描述符可以从管道中读取数据并执行相应的命令
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;		//请求的类型
	request->header[7] = (uint8_t)len;		//不包含类型的大小，请求内容的长度
	for (;;) {
		ssize_t n = write(ss->sendctrl_fd, &request->header[6], len+2);	//写管道，然后通过读管道recvctrl_fd进行读取
		if (n<0) {	//判断是否写成功
			if (errno != EINTR) {	//判断不成功是否是中断的原因
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}

//生成一个TCP连接请求的包的信息，包括分配存储套接字信息
//成功返回主机名的长度，否则返回-1
static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);
	if (len + sizeof(req->u.open) >= 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return -1;
	}
	int id = reserve_id(ss);	//分配一个存储套接字信息的id
	if (id < 0)
		return -1;
	req->u.open.opaque = opaque;	//存储定位服务的handle
	req->u.open.id = id;			//存储定位套接字相关信息的id
	req->u.open.port = port;		//端口号
	memcpy(req->u.open.host, addr, len);	//将addr拷贝到host所指的内存地址
	req->u.open.host[len] = '\0';

	return len;
}

//发送命令'O'，发起连接服务器主机名addr，端口号port，发起的服务句柄为opaque，成功返回存储套接字信息的id,否则返回-1
int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	int len = open_request(ss, &request, opaque, addr, port);	//生成一个TCP连接请求的包的信息，包括分配存储套接字信息
	if (len < 0)
		return -1;
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);	//将套接字的命令‘O’写入到管道中去
	return request.u.open.id;	//返回存储套接字信息的id
}

//判断是否可以直接写数据，如果套接字信息正确，套接字中没有数据要发送，套接字类型为SOCKET_TYPE_CONNECTED，以及
static inline int
can_direct_write(struct socket *s, int id) {
	return s->id == id && nomore_send_data(s) && s->type == SOCKET_TYPE_CONNECTED && s->udpconnecting == 0;
}

// return -1 when error, 0 when success
//发送高优先级数据，先判断套接字是否可以发送数据，判断套接字是否可以直接发送数据，
//如果可以直接发送数据，则对不同的协议进行发送，成功返回0，否则，将数据存入套接字信息s->dw_buffer，s->dw_size，s->dw_offset
//不能直接发送，则通过发送命令‘D’，将数据放入高优先级缓存队列中。
//成功返回0，否则返回-1
int 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];			//获得存储的套接字信息
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {	//不存在则释放
		free_buffer(ss, buffer, sz);
		return -1;
	}

	struct socket_lock l;
	socket_lock_init(s, &l);	//锁l引用s锁

	if (can_direct_write(s,id) && socket_trylock(&l)) {	//判断是否直接可以发送数据，并获得锁
		// may be we can send directly, double check
		if (can_direct_write(s,id)) {
			// send directly
			struct send_object so;
			send_object_init(ss, &so, (void *)buffer, sz);	//初始化发送对象
			ssize_t n;
			if (s->protocol == PROTOCOL_TCP) {		//TCP协议
				n = write(s->fd, so.buffer, so.sz);	//发送数据
			} else {	//UDP协议
				union sockaddr_all sa;
				socklen_t sasz = udp_socket_address(s, s->p.udp_address, &sa);	//获得标准的地址长度，地址存于sa
				n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);	//发送数据
			}
			if (n<0) {
				// ignore error, let socket thread try again
				n = 0;
			}
			if (n == so.sz) {
				// write done
				socket_unlock(&l);	//释放锁
				so.free_func((void *)buffer);	//释放数据内存
				return 0;
			}
			// write failed, put buffer into s->dw_* , and let socket thread send it. see send_buffer()
			s->dw_buffer = buffer;
			s->dw_size = sz;
			s->dw_offset = n;

			sp_write(ss->event_fd, s->fd, s, true);	//修改套接字的监听事件为可读可写

			socket_unlock(&l);	//释放锁
			return 0;
		}
		socket_unlock(&l);		//释放锁
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'D', sizeof(request.u.send));	//将套接字的命令‘D’写入到管道中去
	return 0;
}

// return -1 when error, 0 when success
//向写管道中发起一个发送低优先级的数据的命令'P'，成功返回0，失败返回-1
int 
socket_server_send_lowpriority(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return -1;
	}

	struct request_package request;
	request.u.send.id = id;			//定位存储套接字信息的id
	request.u.send.sz = sz;			//低优先级数据的长度
	request.u.send.buffer = (char *)buffer;	//低优先级数据内容

	send_request(ss, &request, 'P', sizeof(request.u.send));	////将套接字的命令‘P’写入到管道中去
	return 0;
}

//退出整个套接字服务器命令, 调用此函数并不是真正销毁套接字服务器而是以异步的方式给处理线程返回一个 SOCKET_EXIT 状态.
//这样处理线程可以安全的退出, 从而不再处理套接字事件. 真正销毁内存实际上是在整个 skynet 系统退出时
void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

//向写管道中发起一个关闭某个指定套接字的命令'K'，通过id可以定位的套接字的信息
void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;		//定位存储套接字信息的id
	request.u.close.shutdown = 0;	//
	request.u.close.opaque = opaque;//定位服务的handle
	send_request(ss, &request, 'K', sizeof(request.u.close));	//将套接字的命令‘K’写入到管道中去
}

//发送命令'K'，强制关闭套接字
void
socket_server_shutdown(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 1;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

// return -1 means failed
// or return AF_INET or AF_INET6
//创建套接字，绑定套接字到主机名host，端口port，成功返回套接字，否则返回-1
static int
do_bind(const char *host, int port, int protocol, int *family) {
	int fd;
	int status;
	int reuse = 1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	if (host == NULL || host[0] == 0) {
		host = "0.0.0.0";	// INADDR_ANY
	}
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	if (protocol == IPPROTO_TCP) {	//如果为TCP协议
		ai_hints.ai_socktype = SOCK_STREAM;
	} else {
		assert(protocol == IPPROTO_UDP);	//如果为UDP协议
		ai_hints.ai_socktype = SOCK_DGRAM;
	}
	ai_hints.ai_protocol = protocol;

	status = getaddrinfo( host, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	*family = ai_list->ai_family;
	fd = socket(*family, ai_list->ai_socktype, 0);	//创建套接字
	if (fd < 0) {
		goto _failed_fd;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {	//允许套接字和一个已在使用中的地址捆绑
		goto _failed;
	}
	status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);	//绑定套接字
	if (status != 0)
		goto _failed;

	freeaddrinfo( ai_list );	//释放ai_list
	return fd;					//返回套接字
_failed:
	close(fd);
_failed_fd:
	freeaddrinfo( ai_list );
	return -1;
}

//绑定主机名host的port端口，监听套接字，连接请求队列的最大长度为backlog
//成功返回套接字，否则返回-1
static int
do_listen(const char * host, int port, int backlog) {
	int family = 0;
	int listen_fd = do_bind(host, port, IPPROTO_TCP, &family);	//创建及绑定套接字
	if (listen_fd < 0) {
		return -1;
	}
	if (listen(listen_fd, backlog) == -1) {	//监听套接字，连接请求队列的最大长度为backlog
		close(listen_fd);
		return -1;
	}
	return listen_fd;
}

//发送命令'L'，发起绑定主机名addr，端口号port，并监听端口，成功返回存储套接字信息的id,否则返回-1
int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	int fd = do_listen(addr, port, backlog);	//绑定主机名host的port端口，监听套接字，连接请求队列的最大长度为backlog
	if (fd < 0) {
		return -1;
	}
	struct request_package request;
	int id = reserve_id(ss);	//分配新的套接字相关信息存储的id,成功则返回id,否则返回-1
	if (id < 0) {
		close(fd);
		return id;
	}
	request.u.listen.opaque = opaque;	//定位服务的handle
	request.u.listen.id = id;			//定位套接字信息的id
	request.u.listen.fd = fd;			//套接字
	send_request(ss, &request, 'L', sizeof(request.u.listen));	//将套接字的命令‘L’写入到管道中去
	return id;
}

//发送命令'B'，绑定一个外部生成的套接字fd，成功返回存储套接字信息的id,否则返回-1
int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);	//分配新的套接字相关信息存储的id,成功则返回id,否则返回-1
	if (id < 0)
		return -1;
	request.u.bind.opaque = opaque;	//定位服务的handle
	request.u.bind.id = id;			//定位套接字信息的id
	request.u.bind.fd = fd;			//套接字
	send_request(ss, &request, 'B', sizeof(request.u.bind));	//将套接字的命令‘B’写入到管道中去
	return id;
}

//发送命令'S'，开始将套接字添加到epoll进行监听事件，可以是客户端请求连接的套接字，也可以是服务端的监听连接的套接字
void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.start.id = id;			//定位套接字信息的id
	request.u.start.opaque = opaque;	//定位服务的handle
	send_request(ss, &request, 'S', sizeof(request.u.start));	//发送命令'S'，
}

//发送指令'T'，请求设置套接字的选项，选项的层次在 IPPROTO_TCP 上 , 设置的键和值都是 int 类型的, 
//目前仅用于设置套接字的 TCP_NODELAY 选项，禁止发送合并的Nagle算法
void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

void 
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

// UDP
//如果主机名addr，端口port有不为空的，则创建套接字，绑定套接字到主机名addr，端口port，
//否则创建一个UDP套接字
//设置套接字为非阻塞模式，并分配新的套接字相关信息存储的id,发送命令'U'
//成功返回分配新的套接字相关信息存储的id，否则为-1
int 
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	int fd;
	int family;
	if (port != 0 || addr != NULL) {
		//创建套接字，绑定套接字到主机名addr，端口port，成功返回套接字，否则返回-1
		fd = do_bind(addr, port, IPPROTO_UDP, &family);
		if (fd < 0) {
			return -1;
		}
	} else {	//如果ip地址和端口号都为空，则只创建一个UDP套接字
		family = AF_INET;
		fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0) {
			return -1;
		}
	}
	sp_nonblocking(fd);		//设置套接字为非阻塞模式

	int id = reserve_id(ss);	//分配新的套接字相关信息存储的id,成功则返回id,否则返回-1
	if (id < 0) {
		close(fd);
		return -1;
	}
	struct request_package request;
	request.u.udp.id = id;
	request.u.udp.fd = fd;
	request.u.udp.opaque = opaque;
	request.u.udp.family = family;

	send_request(ss, &request, 'U', sizeof(request.u.udp));	//发送命令'U'，添加UDP套接字信息
	return id;
}

//发送UDP数据，如果可以直接发送则直接发送出去，否则，通过命令‘A’，将数据写入缓存
//成功返回0，否则返回-1
int 
socket_server_udp_send(struct socket_server *ss, int id, const struct socket_udp_address *addr, const void *buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];		//获得套接字相关信息
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {	//判断套接字信息是否有效
		free_buffer(ss, buffer, sz);
		return -1;
	}

	const uint8_t *udp_address = (const uint8_t *)addr;	//获得地址信息
	int addrsz;
	switch (udp_address[0]) {	//判断地址的协议类型
	case PROTOCOL_UDP:
		addrsz = 1+2+4;		// 1 type, 2 port, 4 ipv4	//地址的长度
		break;
	case PROTOCOL_UDPv6:
		addrsz = 1+2+16;	// 1 type, 2 port, 16 ipv6	//地址的长度
		break;
	default:
		free_buffer(ss, buffer, sz);
		return -1;
	}

	struct socket_lock l;
	socket_lock_init(s, &l);	//锁l引用锁s

	if (can_direct_write(s,id) && socket_trylock(&l)) {	//判断套接字是否可以直接发送数据，并获得锁
		// may be we can send directly, double check
		if (can_direct_write(s,id)) {	//判断套接字是否可以直接发送数据
			// send directly
			struct send_object so;
			send_object_init(ss, &so, (void *)buffer, sz);	//初始化发送对象
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);	//获得标准地址长度，标准地址信息存入sa
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);	//发送数据
			if (n >= 0) {	//发送成功
				// sendto succ
				socket_unlock(&l);
				so.free_func((void *)buffer);
				return 0;
			}
		}
		socket_unlock(&l);
		// let socket thread try again, udp doesn't care the order
	}

	struct request_package request;
	request.u.send_udp.send.id = id;
	request.u.send_udp.send.sz = sz;
	request.u.send_udp.send.buffer = (char *)buffer;

	memcpy(request.u.send_udp.address, udp_address, addrsz);

	send_request(ss, &request, 'A', sizeof(request.u.send_udp.send)+addrsz);	//通过命令‘A’，将数据写入缓存中
	return 0;
}

//发送命令'C'，将指定的套接字信息关联ip地址，前提是套接字信息中的协议类型相同，成功返回0，否则返回-1
int
socket_server_udp_connect(struct socket_server *ss, int id, const char * addr, int port) {
	struct socket * s = &ss->slot[HASH_ID(id)];		//获得套接字信息
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {	//判断套接字信息是否有效
		return -1;
	}
	struct socket_lock l;
	socket_lock_init(s, &l);	//锁l引用锁s
	socket_lock(&l);			//获得锁
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {	//再次判断套接字信息是否有效
		socket_unlock(&l);
		return -1;
	}
	ATOM_INC(&s->udpconnecting);	//增加套接字信息中s->udpconnecting计数
	socket_unlock(&l);				//释放锁

	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;

	status = getaddrinfo(addr, portstr, &ai_hints, &ai_list );	//获得ip地址信息，适用于IPv4和IPv6
	if ( status != 0 ) {
		return -1;
	}
	struct request_package request;
	request.u.set_udp.id = id;
	int protocol;

	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo( ai_list );
		return -1;
	}
	//将套接字地址的存放在request.u.set_udp.address数组中，并返回地址的长度
	int addrsz = gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, request.u.set_udp.address);

	freeaddrinfo( ai_list );

	//发送命令'C'，将指定的套接字信息关联ip地址
	send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) +addrsz);

	return 0;
}

//UDP通信时，ip地址的转换
const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
	uint8_t * address = (uint8_t *)(msg->data + msg->ud);
	int type = address[0];
	switch(type) {
	case PROTOCOL_UDP:
		*addrsz = 1+2+4;
		break;
	case PROTOCOL_UDPv6:
		*addrsz = 1+2+16;
		break;
	default:
		return NULL;
	}
	return (const struct socket_udp_address *)address;
}
