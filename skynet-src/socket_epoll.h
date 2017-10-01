#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

//是否是无效的epoll句柄
static bool 
sp_invalid(int efd) {
	return efd == -1;
}

//创建一个epoll的句柄
static int
sp_create() {
	return epoll_create(1024);
}

//关闭epoll
static void
sp_release(int efd) {
	close(efd);
}

//添加对sock描述符的可读事件的监听
static int 
sp_add(int efd, int sock, void *ud) {
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = ud;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}

//删除对sock描述符的事件的监听
static void 
sp_del(int efd, int sock) {
	epoll_ctl(efd, EPOLL_CTL_DEL, sock , NULL);
}

//修改sock描述符的是否可写事件的监听，enable为true可读可写，否则可读不可写
static void 
sp_write(int efd, int sock, void *ud, bool enable) {
	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev);
}

//等待efd上的事件
static int 
sp_wait(int efd, struct event *e, int max) {
	struct epoll_event ev[max];
	int n = epoll_wait(efd , ev, max, -1);
	int i;
	for (i=0;i<n;i++) {
		e[i].s = ev[i].data.ptr;	//事件附带的数据
		unsigned flag = ev[i].events;
		e[i].write = (flag & EPOLLOUT) != 0;
		e[i].read = (flag & (EPOLLIN | EPOLLHUP)) != 0;
		e[i].error = (flag & EPOLLERR) != 0;
	}

	return n;
}

//设置fd描述符为非阻塞模式
static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
