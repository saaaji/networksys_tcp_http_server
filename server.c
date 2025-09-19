#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/sendfile.h>
#include <signal.h>
#include <pthread.h>

/**
 * TYPES
 */
typedef struct {
    char path[512];
    char connection[32];
    char method[16];
    char version[16];
} http_request_t;

typedef enum {
    APP_OK = 0,
    APP_ERR = 1
} result_t;

typedef struct {
    int keep_alive;
    time_t last_activity;
} connection_t;

/**
 * CONSTANTS
 */

// status constants
#define APP_OK 0
#define APP_ERR 1
#define IS_OK(value, ok_value) ((value) == (ok_value))
#define IS_OK_APP(value) IS_OK(value, APP_OK)
#define IS_OK_SYS(value) IS_OK(value, 0)

// listening socket configuration
#define LISTEN_QUEUE_SIZE 1024

// HTTP configuration
#define BUFFER_SIZE 8192
#define DOCUMENT_ROOT "./www"
#define PATH_MAX_LEN 1024

/**
 * SIGNAL HANDLERS
 */
static volatile sig_atomic_t gShouldStop = 0;

void cleanup_handler(int status) {
    // communicate that the server should stop now
    gShouldStop = 1;
}

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

const char* find_substr(const char* str, size_t str_len, const char* substr, size_t substr_len) {
    if (substr_len == 0 || str_len < substr_len) return NULL;
    for (size_t i = 0; i <= str_len - substr_len; i++) {
        if (memcmp(str + i, substr, substr_len) == 0) {
            return (char*)(str + i);
        }
    }
    return NULL;
}

void send_error(int client_fd, int code, const char* version, const connection_t* conn) {
    const char* name;
    switch (code) {
        case 400: name = "Bad Request"; break;
        case 403: name = "Forbidden"; break;
        case 404: name = "Not Found"; break;
        case 405: name = "Method Not Allowed"; break;
        case 505: name = "HTTP Version Not Supported"; break;
        default: name = "Internal Server Error"; break;
    }

    char body[1024];
    int content_length = snprintf(body, sizeof(body),
        "<html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1></body></html>",
        code, name, code, name
    );

    dprintf(client_fd, 
        "%s %d %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n"
        "%s", 
        version, 
        code, 
        name, 
        content_length, 
        conn->keep_alive ? "keep-alive" : "close",
        body);
}

const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return NULL;

    if (strcasecmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcasecmp(ext, ".css") == 0) return "text/css";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain";
    if (strcasecmp(ext, ".ico") == 0) return "image/x-icon";

    return NULL;
}

result_t serve_file(int client_fd, const char* full_path, const http_request_t* request, const connection_t* conn) {
    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        printf("entry at '%s' doesn't exist\n", full_path);
        return APP_ERR;
    }
    
    const char* mime_type = get_mime_type(full_path);
    if (!mime_type) {
        // not a supported MIME type
        printf("MIME-type of '%s' is not supported\n", full_path);
        send_error(client_fd, 400, request->version, conn);
        return APP_ERR;
    }

    // if regular file, create response
    int fd = open(full_path, O_RDONLY);
    if (fd < 0) {
        printf("could not open file '%s'\n", full_path);
        const int err_code = errno == EACCES ? 403 : 400;
        send_error(client_fd, err_code, request->version, conn);
        return APP_ERR;
    }

    // send headers
    ssize_t ret = dprintf(
        client_fd,
        // strings will append across lines
        "%s 200 OK\r\n"
        "Content-Length: %jd\r\n"
        "Content-Type: %s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        request->version,
        (intmax_t) st.st_size,
        mime_type,
        conn->keep_alive ? "keep-alive" : "close"
    );

    if (ret < 0) {
        close(fd);
        return APP_ERR;
    }

    // send body
    off_t send_offset = 0;
    while (send_offset < st.st_size) {
        ssize_t bytes_sent = sendfile(client_fd, fd, &send_offset, st.st_size - send_offset);
        if (bytes_sent <= 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break; // error probably occurred
        }
    }

    close(fd);
    return APP_OK;
}

void find_header_value(
    const char* buffer, 
    const char* header_end, 
    const char* header_name,
    char* dest,
    size_t dest_len
) {
    const char* header = find_substr(buffer, header_end - buffer, header_name, strlen(header_name));
    if (header && header < header_end) {
        char* colon = strchr(header, ':');

        if (colon && colon < header_end) {
            char* field_start = colon;

            do {
                field_start++;
            } while (isspace(*field_start) && field_start < header_end);
            
            if (field_start && field_start < header_end) {
                const char* line_end = find_substr(field_start, header_end - field_start, "\r\n", strlen("\r\n"));
                
                if (line_end && line_end < header_end) {
                    size_t field_len = line_end - field_start;
                    if (field_len >= dest_len) field_len = dest_len - 1;
                    memcpy(dest, field_start, field_len);
                    dest[field_len] = '\0';
                }
            }
        }
    }
}

void* client_func(void* arg) {
    const int client_fd = *(int*) arg;
    free(arg);

    connection_t conn;
    conn.keep_alive = 0;
    conn.last_activity = time(NULL);

    printf("initiating new connection with client (new thread)...\n");

    // create buffers for receiving HTTP requests
    char buffer[BUFFER_SIZE];
    size_t buffer_len = 0;
    memset(buffer, 0, sizeof(buffer));

    while (!gShouldStop) {
        // timeout
        if (conn.keep_alive) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(client_fd, &fds);

            struct timeval tv;
            time_t now = time(NULL);
            int elapsed = now - conn.last_activity;
            int remaining = 10 - elapsed;

            if (remaining <= 0) {
                // timeout
                break;
            }

            tv.tv_sec = remaining;
            tv.tv_usec = 0;

            int ret = select(client_fd + 1, &fds, NULL, NULL, &tv);
            if (ret == 0) {
                // timed out
                break;
            } else if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            // receive new data after this
        }

        // -1 to accomodate null terminator
        ssize_t bytes_recv = recv(client_fd, buffer + buffer_len, sizeof(buffer) - buffer_len - 1, 0);
        if (bytes_recv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // non-blocking, no data available
                continue;
            }
            // otherwise, an error must have occurred
            break;
        }

        if (bytes_recv == 0) {
            // connection closed
            break;
        }
        
        // null terminate the buffer for safety and parsing
        buffer_len += bytes_recv;
        buffer[buffer_len] = '\0';

        // look for requests
        while (buffer_len > 0) {
            ssize_t eoh = -1;
            for (size_t i = 0; i < buffer_len - 3; i++) {
                if (buffer[i+0] == '\r' && buffer[i+1] == '\n' &&
                    buffer[i+2] == '\r' && buffer[i+3] == '\n') {
                    eoh = i + 4;
                    break;
                }
            }

            // no complete headers, need more bytes
            if (eoh == -1) break;
            char* header_end = buffer + eoh;

            // extract Content-Length if possible
            int content_length = 0;
            const char* cl_header = find_substr(buffer, header_end - buffer, "Content-Length", strlen("Content-Length"));
            if (cl_header && (cl_header - buffer) < eoh) {
                char* colon = strchr(cl_header, ':');

                if (colon && colon < header_end) {
                    long value;
                    if (try_conv_long(colon + 1, &value) == APP_OK) {
                        content_length = value;

                        // clamp for safety
                        if (content_length < 0) content_length = 0; 
                        // if (content_length > sizeof(buffer) - eoh);
                    }
                }
            }

            size_t total_size = eoh + content_length;
            if (buffer_len < total_size) {
                // wait for complete request
                break;
            }

            /**
             * PARSING
             */
            http_request_t request;
            size_t attrib_len;

            memset(&request, 0, sizeof(request));
            
            // method
            char* first_space = memchr(buffer, ' ', header_end - buffer);
            if (!first_space) goto cleanup;
            attrib_len = first_space - buffer;
            if (attrib_len >= sizeof(request.method)) attrib_len = sizeof(request.method) - 1;
            memcpy(request.method, buffer, attrib_len);
            request.method[attrib_len] = '\0';

            // path
            char* second_space = memchr(first_space + 1, ' ', header_end - (first_space + 1));
            if (!second_space) goto cleanup;;
            attrib_len = second_space - (first_space + 1);
            if (attrib_len >= sizeof(request.path)) attrib_len = sizeof(request.path) - 1;
            memcpy(request.path, first_space + 1, attrib_len);
            request.path[attrib_len] = '\0';
            
            // version
            const char* crlf = find_substr(second_space + 1, header_end - (second_space + 1), "\r\n", strlen("\r\n"));
            if (!crlf || crlf >= header_end) goto cleanup;
            attrib_len = crlf - (second_space + 1);
            if (attrib_len >= sizeof(request.version)) attrib_len = sizeof(request.version) - 1;
            memcpy(request.version, second_space + 1, attrib_len);
            request.version[attrib_len] = '\0';

            // connection timeout handling
            find_header_value(buffer, header_end, "Connection", request.connection, sizeof(request.connection));
            if (strcasecmp(request.connection, "keep-alive") == 0) {
                conn.keep_alive = 1;
            } else {
                conn.keep_alive = 0;
            }
            conn.last_activity = time(NULL);

            /**
             * HANDLE REQUEST
             */
            if (strcmp(request.method, "GET") != 0) {
                // method other than GET was requested
                printf("incompatible HTTP method: %s\n", request.method);
                send_error(client_fd, 405, request.version, &conn);
                goto cleanup;
            } else if (strcmp(request.version, "HTTP/1.0") != 0 && 
                        strcmp(request.version, "HTTP/1.1") != 0) {
                printf("incompatible HTTP version: %s\n", request.version);
                send_error(client_fd, 505, request.version, &conn);
                goto cleanup;
            } else {
                // handle proper GET
                char full_path[PATH_MAX_LEN];

                // clean up paths to avoid overflow
                if (strlen(request.path) >= PATH_MAX_LEN - strlen(DOCUMENT_ROOT) - 1) {
                    send_error(client_fd, 400, request.version, &conn);
                    goto cleanup;
                }

                if (find_substr(request.path, strlen(request.path), "..", strlen(".."))) {
                    send_error(client_fd, 403, request.version, &conn);
                    goto cleanup;
                }

                ssize_t bytes_written = snprintf(full_path, sizeof(full_path), "%s%s", DOCUMENT_ROOT, request.path);
                if (bytes_written < 0 || bytes_written >= sizeof(full_path)) {
                    send_error(client_fd, 400, request.version, &conn);
                    goto cleanup;
                }

                // look for path
                struct stat path_st;
                if (stat(full_path, &path_st) != 0) {
                    if (errno == ENOENT) {
                        // doesn't exist
                        printf("entry at '%s' doesn't exist\n", full_path);
                        send_error(client_fd, 404, request.version, &conn);
                    } else if (errno == EACCES) {
                        // no permissions
                        printf("insufficient permissions to access entry at '%s'\n", full_path);
                        send_error(client_fd, 403, request.version, &conn);
                    } else {
                        // generic error
                        printf("could not handle entry at '%s'\n", full_path);
                        send_error(client_fd, 400, request.version, &conn);
                    }

                    goto cleanup;
                }

                if (S_ISDIR(path_st.st_mode)) {
                    // directory, add a slash in case there is none
                    if (full_path[strlen(full_path) - 1] != '/') {
                        strlcat(full_path, "/", sizeof(full_path));
                    }

                    // if the path is a directory, look for index.html
                    char try_path[PATH_MAX_LEN];
                    strlcpy(try_path, full_path, sizeof(try_path));
                    strlcat(try_path, "index.html", sizeof(try_path));

                    printf("client asked for '%s' (directory), trying '%s'\n", full_path, try_path);

                    if (!IS_OK_APP(serve_file(client_fd, try_path, &request, &conn))) {
                        // try index.htm
                        strlcpy(try_path, full_path, sizeof(try_path));
                        strlcat(try_path, "index.htm", sizeof(try_path));

                        printf("client asked for '%s' (directory), trying '%s'\n", full_path, try_path);

                        if (!IS_OK_APP(serve_file(client_fd, try_path, &request, &conn))) {
                            send_error(client_fd, 404, request.version, &conn);
                            goto cleanup;
                        }
                    }
                } else if (S_ISREG(path_st.st_mode)) {
                    if (!IS_OK_APP(serve_file(client_fd, full_path, &request, &conn))) {
                        goto cleanup;
                    }
                }
            }
            
        cleanup:
            // remove complete request from buffer
            size_t remaining = buffer_len - total_size;
            if (remaining > 0) {
                memmove(buffer, buffer + total_size, remaining);
            }
            buffer_len = remaining;
            buffer[buffer_len] = '\0';

            if (!conn.keep_alive) {
                printf("no keep-alive, closing connection...\n");

                // close connection
                break;
            }
        }

        // discard if request is too large
        if (buffer_len > sizeof(buffer) - 1) {
            buffer_len = 0;
        }
    }
    
    printf("closing client connection...\n");
    close(client_fd);
    return NULL;
}

/// @brief program entrypoint 
int main(int argc, char* argv[]) {
    signal(SIGINT, cleanup_handler);

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

    // make nonblocking
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    // allow socket reuse on program reruns
    int yes_reuse_socket = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes_reuse_socket, sizeof(yes_reuse_socket));

    if (res && !IS_OK_SYS(bind(listen_fd, res->ai_addr, res->ai_addrlen))) {
        printf("failed to bind listening socket\n");
        return APP_ERR;
    }

    // listen on the socket
    if (!IS_OK_SYS(listen(listen_fd, LISTEN_QUEUE_SIZE))) {
        printf("failed to configure socket to listen\n");
        return APP_ERR;
    }

    // setup server
    while (!gShouldStop) {
        struct sockaddr_in client_addr;
        size_t client_len = sizeof(client_addr);

        int* client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (struct sockaddr*) &client_addr, (socklen_t*) &client_len);

        if (*client_fd < 0) {
            free(client_fd);
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no clients pending, sleep
                usleep(10000); // 10 millis
            }

            if (gShouldStop) break;
            continue;
        }

        pthread_t task_id;
        pthread_create(&task_id, NULL, client_func, client_fd);
        pthread_detach(task_id);
    }

    printf("closing listening socket...\n");
    close(listen_fd);
    freeaddrinfo(res);
    return APP_OK;
}