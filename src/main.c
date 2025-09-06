#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>

#define APP_OK 0
#define APP_ERR 1
#define IS_OK(value, ok_value) ((value) == (ok_value))
#define IS_OK_APP(value) IS_OK(value, APP_OK)
#define IS_OK_SYS(value) IS_OK(value, 0)

#define LISTEN_QUEUE_SIZE 1024

/// @brief try to convert string to long integer
/// @param str string representing integer
/// @param result output pointer to write result to
/// @return integer indicating either success (APP_OK)
/// or failure (APP_ERR) w.r.t. conversion
int try_conv_long(const char* str, long* result) {
    // zero errno
    errno = 0;

    // call strtol
    char *end_ptr;
    long value = strtol(str, &end_ptr, 10);

    // catch error cases
    if (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN)) {
        return APP_ERR;
    }

    if (errno != 0 && value == 0) {
        return APP_ERR;
    }

    // no digits found
    if (end_ptr == str) {
        return APP_ERR;
    }

    // invalid character
    if (end_ptr && *end_ptr != '\0') {
        return APP_ERR;
    }

    // successful conversion
    *result = value;
    return APP_OK;
}

/*
 * readline - read a line of text
 * return the number of characters read
 * return -1 if error
 */
int readline(int fd, char * buf, int maxlen)
{
  int nc, n = 0;
  for(n=0; n < maxlen-1; n++)
    {
      nc = read(fd, &buf[n], 1);
      if( nc <= 0) return nc;
      if(buf[n] == '\n') break;
    }
  buf[n+1] = 0;
  return n+1;
}

/// @brief echo input back to client
/// @param connfd 
void echo(int connfd) {
    size_t n; 
    char buf[1024]; 

    while((n = readline(connfd, buf, 11024)) != 0) {
	printf("server received %ld bytes\n", n);
	write(connfd, buf, n);
    }
}

/// @brief program entrypoint 
int main(int argc, char* argv[]) {
    // port number should be provided in CLI arguments 
    if (argc < 2) {
        printf("usage: ./server <port>\n");
        return APP_ERR;
    }

    // get port number
    long port;
    if (!IS_OK_APP(try_conv_long(argv[1], &port)) || port < 0) {
        printf("invalid port number provided: '%s'\n", argv[1]);
        return APP_ERR;
    }

    printf("starting server on port %ld\n", port);
    
    // use host IP
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    
    if (!IS_OK_SYS(getaddrinfo(NULL, argv[1], &hints, &res))) {
        printf("could not get host address information\n");
        return APP_ERR;
    }

    if (!res) {
        printf("no host address information\n");
        return APP_ERR;
    }

    // bind socket to host
    int listen_fd = -1;
    if (res && (listen_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
        printf("error creating socket\n");
        return APP_ERR;
    }

    if (res && !IS_OK_SYS(bind(listen_fd, res->ai_addr, res->ai_addrlen))) {
        printf("failed to bind listening socket\n");
        return APP_ERR;
    }

    // allow socket reuse on program reruns
    int yes_reuse_socket = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes_reuse_socket, sizeof(yes_reuse_socket));

    // listen on the socket
    if (!IS_OK_SYS(listen(listen_fd, LISTEN_QUEUE_SIZE))) {
        printf("failed to configure socket to listen\n");
        return APP_ERR;
    }

    // setup server
    while (1) {
        struct sockaddr_in client_addr;
        size_t client_len = sizeof(client_addr);

        int connect_fd = accept(listen_fd, (struct sockaddr*) &client_addr, (socklen_t*) &client_len);
        echo(connect_fd);
        close(connect_fd);
    }

    freeaddrinfo(res);
    return APP_OK;
}