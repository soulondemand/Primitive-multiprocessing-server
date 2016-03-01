#include <iostream>
#include <map>
#include <regex>
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
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sendfile.h>

//#define DEBUG
#include "local_def.h"

#define NUM_CHILD 3
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

using namespace std;

void child_event_loop(int i, int childListenSocket, string root_directory);
int parse_request(string &str_request, string &method, string &uri, string &http_ver); 


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
	
	string ip_str = "127.0.0.1";
	string port_str = "12345";
	string root_directory = "sites";
	
	static const char *opts = "h:p:d:"; // доступные опции, каждая принимает аргумент
	int opt; // каждая следующая опция попадает сюда
	while((opt = getopt(argc, argv, opts)) != -1) { // вызываем getopt пока она не вернет -1
		switch(opt) {
			case 'h': // если опция -h возвращаем строку
				ip_str = optarg;
				break;
			case 'p': // если опция -p, преобразуем строку с аргументом в число
				port_str = optarg;
				break;
			case 'd': //  если опция -d возвращаем строку
				root_directory = optarg;
				break;
		}
	}

	//cout << "ip_str: " << ip_str << endl;
	//cout << "port_str: " << port_str << endl;
	//cout << "root_directory: " << root_directory << endl;
	//exit(0);

	//ip_str = "127.0.0.1";
	//port_str = "12345";
	//root_directory = "sites";

	if( root_directory.back() == '/' )
		root_directory = root_directory.substr(0, (root_directory.length() - 1));

	//--------------------------------
	//TODO: Демонизация
	//--------------------------------
	
	#ifndef DEBUG	
	if( !daemonize() ) {
		fprintf(stderr, "Failed to become daemon");
	}
	#endif

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
			child_event_loop(i, child[i].fd_ch2parent, root_directory);
		}

	}
	//---------------------------------------
	// Прием и передача сетевых соединиений
	//---------------------------------------
	// TODO: перевести обмен с сокетом с write/read на send/recv				

	ip = inet_addr(ip_str.c_str());
	listen_port = (unsigned int) atol(port_str.c_str());

	int MasterSocket = socket(
			AF_INET /* IPv4*/,
			SOCK_STREAM /* TCP */,
			IPPROTO_TCP | SO_REUSEADDR);
	struct sockaddr_in SockAddr;
	SockAddr.sin_family = AF_INET;
	//SockAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	SockAddr.sin_addr.s_addr = ip;
	//SockAddr.sin_port   = htons( atoi( port_str.c_str() ));
	SockAddr.sin_port   = htons(listen_port);


	CHK2( MasterSocket , socket(PF_INET, SOCK_STREAM, IPPROTO_TCP) );
	DBG( printf("master: Main listener(fd=%d) created! \n", MasterSocket); )

		int optval = 1;
	setsockopt(MasterSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	CHK( bind(MasterSocket, (struct sockaddr *) (&SockAddr), sizeof(SockAddr)) );
	DBG( printf("master: Listener binded to: %s\n", port_str.c_str()); )

		CHK( listen(MasterSocket, SOMAXCONN) );

	int conn_sock;
	//event loop for listen socket
	while(true) {
		// принимаем сокет и передаем дескриптор worker'у
		CHK2( conn_sock, accept(MasterSocket, NULL, NULL));
		DBG( printf("master: New client (fd = %d)\n", conn_sock);)
			set_nonblock(conn_sock);
		char ch[] = "1";
		//---------------------------
		// Выбор потомков для работы
		//---------------------------
		static int curr_children = 0;
		size = sock_fd_write(child[curr_children].fd_ch2child, ch, 1, conn_sock);
		++curr_children;
		if(curr_children >= NUM_CHILD) curr_children = 0;
		DBG( printf ("master: wrote %d byte to worker (fd passing)\n", size);)
			close(conn_sock);
	}

	return 0;
}


//-------------------------
// цикл событий worker'ов
//-------------------------
void child_event_loop(int children_i, int fd_ch2parent, string root_directory) {
	//size_t max_len_request = 1024;
	//size_t max_num_request = 1000;
	//char *buf_request;
	static const char* templ = "HTTP/1.0 200 OK\r\n"
		"Content-length: %d\r\n"
		"Content-Type: text/html\r\n"
		"\r\n";


	static const char not_found_header[] = "HTTP/1.0 404 NOT FOUND\r\n"
		"Content-length: %u\r\n"
		"Content-Type: text/html\r\n"
		"\r\n";
	static const char not_found_msg[]    = "<h1>404 - File not found.</h1>\r\n";

	map<int, string> str_request; // num_socket:str_request;

	int epfd;
	struct epoll_event Event;
	struct epoll_event Events[MAX_EVENTS]; 
	int i;

	ssize_t size;
	char buf[BUF_SIZE];
	memset(buf, 0, BUF_SIZE);

	CHK2( epfd, epoll_create1(0));
	DBG( printf("children %d: epoll created\n", children_i);)

		Event.data.fd = fd_ch2parent;
	Event.events = EPOLLIN;
	CHK(epoll_ctl(epfd, EPOLL_CTL_ADD, fd_ch2parent, &Event) == -1)
		DBG( printf("children %d: listen socket added to epoll\n", children_i);)
		int conn_sock;
	int epoll_events_count;
	//event loop of child 
	while(true) {
		CHK2( epoll_events_count, epoll_wait(epfd, Events, MAX_EVENTS,-1));
		DBG( printf("children %d: Epoll events count: %d\n", children_i, epoll_events_count);)
			DBG( printf("children %d: PID: %d\n", children_i, getpid());)
			for(i = 0; i < epoll_events_count ; ++i) {
				int currfd = Events[i].data.fd;
				// Сообщение с канала от родителя - принимаем дескриптор нового соединения
				if (Events[i].data.fd == fd_ch2parent) {
					size = sock_fd_read(fd_ch2parent, buf, sizeof(buf), &conn_sock);
					if (size <= 0)
						break;
					DBG( printf("children %d: read %d byte (fd passing)\n", children_i, size);)
						DBG( printf("children %d: New client (fd = %d)\n", children_i,  conn_sock);)
						Event.events = EPOLLIN;// | EPOLLET;
					Event.data.fd = conn_sock;
					CHK( epoll_ctl(epfd, EPOLL_CTL_ADD, conn_sock,	&Event) == -1) 
						DBG( printf("children %d: Added new client (fd = %d) to epoll\n", children_i,  conn_sock);)
						//write(Events[i].data.fd, "#Hello, client!\n", 16);
				} else { //событие от потомка
					DBG(printf("children %d: Try to read from fd(%d)\n", children_i, currfd);)
						int len = recv(currfd, buf, BUF_SIZE, 0);
					if( len == 0) {
						close(currfd);
						break;
					}
			
					string chunk = string(buf, len);
					//Оптимизации по работе со строками по добавлению новых chunks в запрос нет, не продакшн
					//printf("\tПолучен chunk: %s\n",chunk.c_str());
					//если запроса с таким дескриптором еще не было
					if(str_request.find(currfd) == str_request.end()) {
						//printf("\tНовый запрос\n");
						str_request.insert(pair<int, string>(currfd,chunk));
						//printf("\tВ буфере после добавления %d байт\n", str_request[currfd].length());
					} else {
						//printf("\tПовторный  запрос\n");
						//printf("\tВ буфере перед добавлением %d байт\n", str_request[currfd].length());
						str_request[currfd] += chunk;
						//printf("\tВ буфере после добавления %d байт\n", str_request[currfd].length());
					}
					//write(STDOUT_FILENO, buf, len);
					//write(STDOUT_FILENO, str_request[currfd].c_str(), str_request[currfd].length() );
					if( (str_request[currfd].find("\n\n") == string::npos) && (str_request[currfd].find("\n\r\n") == string::npos))
						continue;
					DBG( fprintf(stderr, "------------------------request--------------------------\n" ));
					DBG( fprintf(stderr, str_request[currfd].c_str(), str_request[currfd].length() ));
					DBG( fprintf(stderr, "---------------------------------------------------------\n" ));

					//-----------------------------------------------
					// запрос полностью получен => парсим, отвечаем
					//-----------------------------------------------
					//write(currfd, buf, len);
					map<string, string> headers;
					string method, uri, http_ver;
					parse_request(str_request[currfd], method, uri, http_ver);
					string target_file;
					int pos_param = uri.find("?");
					if(pos_param != string::npos)
						target_file = uri.substr(0, pos_param);
					else
						target_file = uri;
					//cout << "Target file: " <<  target_file << endl;
					string fullpath = root_directory + target_file;
					FILE *fp = fopen(fullpath.c_str(), "r");
					DBG( cerr << "\tFullpath: " << fullpath << endl;)
					//cout << "\tFILE: " << fp << endl;
					if( fp == NULL || !is_regular_file(fullpath.c_str()) ) {
						dprintf(currfd, not_found_header, sizeof(not_found_msg));
						write(currfd, not_found_msg, sizeof(not_found_msg));
					} else {
						struct stat buf;
						fstat(fileno(fp), &buf);
						int file_size = buf.st_size;
						dprintf(currfd, templ, file_size);
						sendfile(currfd, fileno(fp), NULL, file_size);
						fclose(fp);
					}
					//close(currfd);
					shutdown(currfd, SHUT_RDWR);
					str_request[currfd] = "";
					//printf("===================================================");
				}
			}
	}

};

//-------------------------
// Парсим запрос на сервер
//-------------------------
int parse_request(string &str_request, string &method, string &uri, string &http_ver) {
	int request_length = str_request.length();
	if(request_length < sizeof("GET / HTTP/1.0\n"))
		return 1;
	
	
	size_t pos = 0; // prev_pos = 0, start_str_item = 0, end_str_item, delim_pos;
	pos = str_request.find("\n");
	if(str_request[pos-1] == '\r')
		--pos;
	string first_line = str_request.substr(0, pos);
	//----------------------
	// парсим первую строку
	//----------------------
	DBG( fprintf(stderr, "\t!!!! Parsing\n"););
	//smatch m;
	//regex_constants::match_flag_type flags = regex_constants::format_first_only;                              
	//regex e ("(\\S+)\\s+(\\S+)\\s+(\\S+)");
	DBG( fprintf(stderr, "\tstart parsing\n"););
	//if( regex_search(first_line, m, e, flags) == 0 )
	//	return 1;
	//DBG( fprintf(stderr, "\tend parsing\n"););
	//for (auto x:m) cout << "'" << x << "'" << endl;
	int pos1, pos2;
	pos1 = first_line.find(" ");
	method = first_line.substr(0, pos1);

	pos1 = first_line.find("/", pos1 + 1);
	pos2 = first_line.find(" ", pos1 + 1);
	uri	 = first_line.substr(pos1, pos2 - pos1);
	
	pos1 = first_line.find("H", pos1 + 1); 
	http_ver = first_line.substr(pos1,  first_line.length() - pos1);
	DBG( fprintf(stderr, "Metod: '%s'\n", method.c_str() ));
	DBG( fprintf(stderr, "URI: '%s'\n", uri.c_str() ));
	DBG( fprintf(stderr, "http_ver : '%s'\n", http_ver .c_str() ));
	// root_directory 


	//----------------------------------------------------------------------------
	// Парсить заголовки запроса кроме первой строки для задачи нет необходимости
	//----------------------------------------------------------------------------

	//++pos; //переходим на новую строку запроса
	//string item;
	//string name_header, value_header;
	//while(pos < request_length) {
	//	start_str_item = pos;
	//	pos = str_request.find("\n");
	//	if(pos == string::npos)   //больше нет в строке запроса \n
	//	   break;
	//	if(pos == start_str_item) //с начала нового блока поймали - больше нет данных в запросе
	//		break;
	//	if(str_request[pos-1] == '\r')
	//		end_str_item = pos - 2;
	//	else
	//		end_str_item = pos - 1;
	//	if(start_str_item <= end_str_item) //пустая строка
	//		break;
	//	
	//	item = str_request.substr(start_str_item, (end_str_item - start_str_item + 1));
	//	delim_pos = item.find(": ");
	//	if(delim_pos == string::npos)
	//		return 1;
	//	if(delim_pos == 0 || delim_pos == (item.length() - 1) ) return 1; //invalid header
	//	name_header  = item.substr(0, delim_pos - 1);
	//	value_header = item.substr(delim_pos + 1, item.length() - 1 - delim_pos);
	//}


	return 0;
}
