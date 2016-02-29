all:
	g++ -std=c++11 -o server server.cpp http-parser/http_parser.c
