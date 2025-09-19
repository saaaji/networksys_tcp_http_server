all:
	gcc server.c -pthread -O3 -g -o server

clean:
	rm server