#include "skynet.h"

#include "skynet_socket.h"
#include "socket_server.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_harbor.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static struct socket_server * SOCKET_SERVER = NULL;			//全局的套接字服务信息

//初始化全局的套接字服务信息
void 
skynet_socket_init() {
	SOCKET_SERVER = socket_server_create();
}

//向套接字服务器发送退出命令, 这将导致主循环函数 skynet_socket_poll 返回 0 , 从而令 socket 线程退出,
//整个过程是一个异步的过程. 需要注意的是, 退出函数并没有销毁套接字服务器的内存. 
void
skynet_socket_exit() {
	socket_server_exit(SOCKET_SERVER);
}

void
skynet_socket_free() {
	socket_server_release(SOCKET_SERVER);
	SOCKET_SERVER = NULL;
}

// mainloop thread
//将套接字上的消息发送给对应的服务，type套接字消息的类型，padding是否需要填充内容
static void
forward_message(int type, bool padding, struct socket_message * result) {
	struct skynet_socket_message *sm;
	size_t sz = sizeof(*sm);
	if (padding) {	//是否需要填充，填充的内容大小不能超过128
		if (result->data) {
			size_t msg_sz = strlen(result->data);
			if (msg_sz > 128) {
				msg_sz = 128;
			}
			sz += msg_sz;
		} else {
			result->data = "";
		}
	}
	sm = (struct skynet_socket_message *)skynet_malloc(sz);
	sm->type = type;		//套接字消息的类型
	sm->id = result->id;	//定位存储套接字信息的id
	sm->ud = result->ud;	//一般为套接字消息的大小
	if (padding) {
		sm->buffer = NULL;
		memcpy(sm+1, result->data, sz - sizeof(*sm));	//填充的内容放在skynet_socket_message消息的下一个地址上
	} else {
		sm->buffer = result->data;		//不需要填充的内容
	}

	struct skynet_message message;	//skynet消息
	message.source = 0;
	message.session = 0;
	message.data = sm;		//skynet消息内容包含skynet_socket_message已经如果需要填充的内容
	message.sz = sz | ((size_t)PTYPE_SOCKET << MESSAGE_TYPE_SHIFT);	//消息的类型PTYPE_SOCKET及大小
	
	if (skynet_context_push((uint32_t)result->opaque, &message)) {	//添加消息到对应的服务队列
		// todo: report somewhere to close socket
		// don't call skynet_socket_close here (It will block mainloop)
		skynet_free(sm->buffer);
		skynet_free(sm);
	}
}

//处理所有套接字上的事件，返回处理的结果，将处理的结果及结果信息转发给对应的服务
int 
skynet_socket_poll() {
	struct socket_server *ss = SOCKET_SERVER;
	assert(ss);
	struct socket_message result;
	int more = 1;
	int type = socket_server_poll(ss, &result, &more);	//处理所有套接字上的事件，返回处理结果及信息
	switch (type) {
	case SOCKET_EXIT:
		return 0;			//整个套接字服务退出
	case SOCKET_DATA:		//接收到TCP数据 result中存有定位服务的handle，定位套接字信息的id，接收到的数据大小ud以及数据data
		forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result); 
		break;
	case SOCKET_CLOSE:		//套接字已被关闭
		forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
		break;
	case SOCKET_OPEN:		//说明套接字已经可以进行正常的通信，例如绑定套接字成功，请求连接成功
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		break;
	case SOCKET_ERR:		//出错返回
		forward_message(SKYNET_SOCKET_TYPE_ERROR, true, &result);
		break;
	case SOCKET_ACCEPT:		//客户端请求连接事件处理，表明连接成功
		forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);	//result->data保存客户端的ip地址和端口号
		break;
	case SOCKET_UDP:		//接收到UDP数据 result中存有定位服务的handle，定位套接字信息的id，接收到的数据大小ud以及数据+IP地址data
		forward_message(SKYNET_SOCKET_TYPE_UDP, false, &result);
		break;
	case SOCKET_WARNING:	//写缓存超出阈值
		forward_message(SKYNET_SOCKET_TYPE_WARNING, false, &result);
		break;
	default:
		skynet_error(NULL, "Unknown socket message type %d.",type);
		return -1;
	}
	if (more) {
		return -1;
	}
	return 1;
}

//发送命令‘D’，发送高优先级数据，先判断套接字是否可以发送数据，判断套接字是否可以直接发送数据，
//如果可以直接发送数据，则对不同的协议进行发送，成功返回0，否则，将数据存入套接字信息s->dw_buffer，s->dw_size，s->dw_offset
//不能直接发送，则通过发送命令‘D’，将数据放入高优先级缓存队列中。
//成功返回0，否则返回-1
int
skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz) {
	return socket_server_send(SOCKET_SERVER, id, buffer, sz);
}

//发送命令'P'，向套接字发送低优先级的数据，buffer数据的内容，sz数据的长度
//成功返回0，否则为-1
int
skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz) {
	return socket_server_send_lowpriority(SOCKET_SERVER, id, buffer, sz);
}

//发送命令'L'，发起绑定主机名host，端口号port,并监听命令
//连接请求队列的最大长度为backlog，成功返回存储套接字信息的id,否则返回-1
int 
skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_listen(SOCKET_SERVER, source, host, port, backlog);
}

//发送命令'O'，发起连接服务器主机名host，端口号port，成功返回存储套接字信息的id,否则返回-1
int 
skynet_socket_connect(struct skynet_context *ctx, const char *host, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_connect(SOCKET_SERVER, source, host, port);
}

//发送命令'B'，绑定外部生成的套接字fd，成功返回存储套接字信息的id,否则返回-1
int 
skynet_socket_bind(struct skynet_context *ctx, int fd) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_bind(SOCKET_SERVER, source, fd);
}

//发送命令'K'，发起一个关闭某个指定套接字的请求，通过id可以定位的套接字的信息
void 
skynet_socket_close(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_close(SOCKET_SERVER, source, id);
}

//发送命令'K'，强制关闭套接字
void 
skynet_socket_shutdown(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_shutdown(SOCKET_SERVER, source, id);
}

//发送命令'S'，开始将套接字添加到epoll进行监听事件，可以是客户端请求连接的套接字，也可以是服务端的监听连接的套接字
//id用于定位存储套接字相关信息
void 
skynet_socket_start(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_start(SOCKET_SERVER, source, id);
}

//发送指令'T'，请求设置套接字的选项，选项的层次在 IPPROTO_TCP 上 , 设置的键和值都是 int 类型的, 
//目前仅用于设置套接字的 TCP_NODELAY 选项，禁止发送合并的Nagle算法
void
skynet_socket_nodelay(struct skynet_context *ctx, int id) {
	socket_server_nodelay(SOCKET_SERVER, id);
}

//发送命令'U'，如果主机名addr，端口port有不为空的，则创建套接字，绑定套接字到主机名addr，端口port，
//否则创建一个UDP套接字
//设置套接字为非阻塞模式，并分配新的套接字相关信息存储的id,发送命令'U'
//成功返回分配新的套接字相关信息存储的id，否则为-1
int 
skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp(SOCKET_SERVER, source, addr, port);
}

//发送命令'C'，将指定的套接字信息关联ip地址，前提是套接字信息中的协议类型相同，成功返回0，否则返回-1
int 
skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port) {
	return socket_server_udp_connect(SOCKET_SERVER, id, addr, port);
}

//发送命令'A'，发送UDP数据，如果可以直接发送则直接发送出去，否则，通过命令‘A’，将数据写入缓存
//成功返回0，否则返回-1
int 
skynet_socket_udp_send(struct skynet_context *ctx, int id, const char * address, const void *buffer, int sz) {
	return socket_server_udp_send(SOCKET_SERVER, id, (const struct socket_udp_address *)address, buffer, sz);
}

//从 skynet_socket_message 中提取出 socket_server 格式的地址, 地址的长度由 addrsz 接收
//返回: 地址内存的起点
const char *
skynet_socket_udp_address(struct skynet_socket_message *msg, int *addrsz) {
	if (msg->type != SKYNET_SOCKET_TYPE_UDP) {
		return NULL;
	}
	struct socket_message sm;
	sm.id = msg->id;
	sm.opaque = 0;
	sm.ud = msg->ud;
	sm.data = msg->buffer;
	return (const char *)socket_server_udp_address(SOCKET_SERVER, &sm, addrsz);
}
