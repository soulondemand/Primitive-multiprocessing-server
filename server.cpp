#include <iostream>
#include <stdio.h>
#include <string>
#include <cstdlib>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/wait.h>

#define DEBUG
#include "serv_def.h"

#define NUM_CHILD 2
#define STR1 "How are you?"
#define STR2 "I'm ok, thank you."
#define BUF_SIZE 1024
#define MAX_RECEIVED_PACKET 100
#define MAX_EVENTS 10


in_addr_t ip;
unsigned int listen_port;

struct children_data {
	int pid;
	int fd_ch2child;
	int fd_ch2parent;
	char master;
} child[NUM_CHILD];

//защита от зомби
void reap_child(int signum){
	pid_t pid;
	int wait_status;
	while((pid = waitpid (-1, &wait_status, WNOHANG)) > 0) {
	}
}

void child_event_loop(int i, int childListenSocket);

using namespace std;


int main(int argc, char **argv) {

	int pid, i, j;
	int channel[2];
	ssize_t size;

	memset(child, 0, sizeof(struct children_data) * NUM_CHILD);
	signal(SIGCHLD, reap_child);

	//--------------------------------
	//TODO: разбор командной строки
	//--------------------------------
	//parce_cmd_line(argc, argv, ip_str, port_str, root_directory);

	//--------------------------------
	//TODO: Демонизация
	//--------------------------------


	//--------------------------------
	// Порождение потомков
	//--------------------------------
	//for {
	//		socketpair
	//		fork
	//}
	for(i = 0; i < NUM_CHILD; ++i) {

		CHK( socketpair(AF_UNIX, SOCK_STREAM, 0, channel));
		child[i].fd_ch2child  = channel[0];
		child[i].fd_ch2parent = channel[1];

		pid = fork();
		if (pid != 0) {     //parent
			child[i].pid = pid;
			close(child[i].fd_ch2parent);
		} else {			//child				fire_and_forget
			close(child[i].fd_ch2child);
			//закрываем унаследованные дескрипторы-дубликаты на других детей
			if(i != 0) {
				for(j = 0; j < i; ++j) {
					close(child[j].fd_ch2child);
				}
			}
			child_event_loop(i, child[i].fd_ch2parent);
		}

	}
	//---------------------------------------
	// Прием и передача сетевых соединиений
	//---------------------------------------
	string ip_str = "127.0.0.1";
	string port_str = "12345";
	string root_directory = "sites";

	ip = inet_addr(ip_str.c_str());
	listen_port = (unsigned int) atol(port_str.c_str());

	int MasterSocket = socket(
			AF_INET /* IPv4*/,
			SOCK_STREAM /* TCP */,
			IPPROTO_TCP);
	struct sockaddr_in SockAddr;
	SockAddr.sin_family = AF_INET;
	//SockAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	SockAddr.sin_addr.s_addr = ip;
	//SockAddr.sin_port   = htons( atoi( port_str.c_str() ));
	SockAddr.sin_port   = htons(listen_port);


	CHK2( MasterSocket , socket(PF_INET, SOCK_STREAM, 0));
	DBG( printf("master: Main listener(fd=%d) created! \n", MasterSocket);)
	
	CHK( bind(MasterSocket, (struct sockaddr *) (&SockAddr), sizeof(SockAddr)));
	DBG( printf("master: Listener binded to: %s\n", port_str.c_str());)
	
	CHK( listen(MasterSocket, SOMAXCONN));

	int conn_sock;
	//event loop for listen socket
	while(true) {
		// принимаем сокет и передаем дескриптор worker'у
		CHK2( conn_sock, accept(MasterSocket, NULL, NULL));
		DBG( printf("master: New client (fd = %d)\n", conn_sock);)
		set_nonblock(conn_sock);
		char ch[] = "1";
		size = sock_fd_write(child[0].fd_ch2child, ch, 1, conn_sock);
		DBG( printf ("master: wrote %d byte to worker (fd passing)\n", size);)
			close(conn_sock);
	}

	return 0;
}


//-------------------------
// цикл событий worker'ов
//-------------------------
void child_event_loop(int children_i, int fd_ch2parent) {
	int epfd;
	struct epoll_event Event;
	struct epoll_event Events[MAX_EVENTS]; 
	int i;

	ssize_t size;
	char buf[BUF_SIZE];
	memset(buf, 0, BUF_SIZE);

	CHK2(epfd, epoll_create1(0));
	DBG(printf("children %d: epoll created\n", children_i);)

	Event.data.fd = fd_ch2parent;
	Event.events = EPOLLIN;
	CHK(epoll_ctl(epfd, EPOLL_CTL_ADD, fd_ch2parent, &Event) == -1)
	DBG(printf("children %d: listen socket added to epoll\n", children_i);)
	int conn_sock;
	int epoll_events_count;
	//event loop of child 
	while(true) {
		CHK2(epoll_events_count, epoll_wait(epfd, Events, MAX_EVENTS,-1));
		DBG(printf("children %d: Epoll events count: %d\n", children_i, epoll_events_count);)
			for(i = 0; i < epoll_events_count ; ++i) {
				int currfd = Events[i].data.fd;
				// Сообщение с канала от родителя - принимаем дескриптор нового соединения
				if (Events[i].data.fd == fd_ch2parent) {
					size = sock_fd_read(fd_ch2parent, buf, sizeof(buf), &conn_sock);
					if (size <= 0)
						break;
					DBG(printf("children %d: read %d byte (fd passing)\n", children_i, size);)
					DBG(printf("children %d: New client (fd = %d)\n", children_i,  conn_sock);)
					Event.events = EPOLLIN;// | EPOLLET;
					Event.data.fd = conn_sock;
					CHK( epoll_ctl(epfd, EPOLL_CTL_ADD, conn_sock,	&Event) == -1) 
					DBG( printf("children %d: Added new client (fd = %d) to epoll\n", children_i,  conn_sock);)
						write(Events[i].data.fd, "#Hello, client!\n", 16);
				} else {
					DBG(printf("children %d: Try to read from fd(%d)\n", children_i, currfd);)
						int len = recv(currfd, buf, BUF_SIZE, 0);
					if( len == 0) {
						close(currfd);
					} else {
						write(currfd, buf, len);
					}	
				}
			}
	}

};
