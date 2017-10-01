#ifndef skynet_socket_h
#define skynet_socket_h

struct skynet_context;

//skynet_socket_message中type套接字消息的类型
#define SKYNET_SOCKET_TYPE_DATA 1		//接收到的TCP数据
#define SKYNET_SOCKET_TYPE_CONNECT 2	//套接字已经可以进行正常的通信，例如绑定套接字成功，请求连接成功
#define SKYNET_SOCKET_TYPE_CLOSE 3		//套接字已被关闭
#define SKYNET_SOCKET_TYPE_ACCEPT 4		//客户端请求连接事件处理，表明连接成功
#define SKYNET_SOCKET_TYPE_ERROR 5		//出错返回
#define SKYNET_SOCKET_TYPE_UDP 6		//接收到UDP数据
#define SKYNET_SOCKET_TYPE_WARNING 7	//写缓存超出阈值

struct skynet_socket_message {	//发送到 skynet 各个服务去的套接字消息
	int type;		//套接字消息的类型，取上面的预定义值
	int id;			//定位存储套接字信息的id
	int ud;			//套接字消息的数据的大小
	char * buffer;	//套接字消息的数据
};

void skynet_socket_init();
void skynet_socket_exit();
void skynet_socket_free();
int skynet_socket_poll();

int skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz);
int skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz);
int skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog);
int skynet_socket_connect(struct skynet_context *ctx, const char *host, int port);
int skynet_socket_bind(struct skynet_context *ctx, int fd);
void skynet_socket_close(struct skynet_context *ctx, int id);
void skynet_socket_shutdown(struct skynet_context *ctx, int id);
void skynet_socket_start(struct skynet_context *ctx, int id);
void skynet_socket_nodelay(struct skynet_context *ctx, int id);

int skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port);
int skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port);
int skynet_socket_udp_send(struct skynet_context *ctx, int id, const char * address, const void *buffer, int sz);
const char * skynet_socket_udp_address(struct skynet_socket_message *, int *addrsz);

#endif
