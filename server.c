#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define RMAX 4096
#define HMAX 1024
#define BMAX 1024
#define EMAX 16
#define ERR_BAD_REQ 400
#define ERR_REQ_TOO_LARGE 413
#define ERR_NOT_FOUND 404

static char request[RMAX+1];

static int HSIZE = 0;
static char header[HMAX];

static int BSIZE = 0;
static char body[BMAX];

static char stored_data[BMAX] = "(NULL)";
static int stored_len = 6;

static int client_file_fd[BMAX];


static int open_listenfd(int port) 
{
    int listenfd = socket(AF_INET, SOCK_STREAM, 0); // TCP

    //  socket can be re-binded
    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    bind(listenfd, (struct sockaddr*)&server, sizeof(server));
    listen(listenfd, 10);

    return listenfd;

}

void add_to_interestlist(int epfd, int fd) 
{
    // set up event struct
    struct epoll_event Event;
    memset(&Event, 0x00, sizeof(Event));
    Event.events = EPOLLIN;
    Event.data.fd = fd;

    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &Event);
}


static int accept_connection(int listenfd)
{
    static struct sockaddr_in client;
    static socklen_t csize;
    memset(&client, 0x00, sizeof(client));
    memset(&csize, 0x00, sizeof(csize));

    int clientfd = accept(listenfd, (struct sockaddr*)&client, &csize);
    return clientfd;
    
}

static void send_data(int clientfd, char buf[], int size)
{
    ssize_t amt, total = 0;
    do {
        amt = send(clientfd, buf + total, size- total, 0);
        total += amt;
    } while (total < size);
}

static void send_response(int clientfd)
{
    send_data(clientfd, header, HSIZE);
    send_data(clientfd, body, BSIZE);
}

void sendError (int res, int clientfd) 
{

    if (res == ERR_BAD_REQ) {
        const char *response_header = "HTTP/1.1 400 Bad Request\r\n\r\n";
        strcpy(header, response_header);
        strcpy(body, "");
    }

    if (res == ERR_REQ_TOO_LARGE) {
        const char *response_header = "HTTP/1.1 413 Request Entity Too Large\r\n\r\n";
        strcpy(header, response_header);
        strcpy(body, "");
    }

    if (res == ERR_NOT_FOUND) {
        const char *response_header = "HTTP/1.1 404 Not Found\r\n\r\n";
        strcpy(header, response_header);
        strcpy(body, "");
    }

    

    HSIZE = strlen(header);
    BSIZE = 0;

    send_response(clientfd);
}

void handle_hello_req(int clientfd)
{
    const char *response_header = "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n";
    const char *response_body = "greetings";

    strcpy(header, response_header);
    strcpy(body, response_body);

    HSIZE = strlen(header);
    BSIZE = strlen(body);

    send_response(clientfd);
}


char* parseHeaders(char* request)
{
    char* headers_start = strstr(request, "\r\n");

    if (headers_start) headers_start += 2; // skip \r\n

    char* result = malloc(HMAX);
    if (strlen(headers_start)-4 > 1024) return NULL; // -4 bcz dont include CRLF

    char* line = strtok(headers_start, "\r\n");
    while (line && strlen(line) > 0) {
        strcat(result, line);
        strcat(result, "\r\n");

        line = strtok(NULL, "\r\n");
    }
    result[strlen(result) - 2] = '\0';

    return result;
}


void handle_header_req(int clientfd, char* request)
{

    char* headers = parseHeaders(request);
    if (!headers) {
        sendError(ERR_REQ_TOO_LARGE, clientfd);
        return;
    }
    int len = strlen(headers);

    snprintf(header, HMAX, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", len);
    strcpy(body, headers);

    HSIZE = strlen(header);
    BSIZE = strlen(body);

    send_response(clientfd);

}

int checkRequest(char* request)
{
    char* header_end = strstr(request, "\r\n\r\n");
    if (header_end == NULL) return ERR_BAD_REQ;
    else return 0;
}

char* get_body(char* request)
{
    char* headers_end = strstr(request, "\r\n\r\n");
    char* body_start = headers_end += 4;
    return body_start;
}

int get_contentlen(char* request)
{
    char* line = strstr(request, "Content-Length:");
    if (line == NULL) {
        return -1;
    }

    int content_length = -1;
    sscanf(line, "Content-Length: %d\r\n", &content_length);
    return content_length;

}



void handle_POST_req(int clientfd, char* request)
{
    char* parsed_body = get_body(request);
    int len = get_contentlen(request);
    if (len > 1024) {
        sendError(413, clientfd);
        return;
    }
    if (len == -1) {
        sendError(400, clientfd);
        return;
    }

    memcpy(stored_data, parsed_body, len);
    stored_len = len;


    snprintf(header, HMAX, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", len);
    memcpy(body, stored_data, len);

    HSIZE = strlen(header);
    BSIZE = len;

    send_response(clientfd);

}

void handle_GET_datareq(int clientfd, char* request, int epfd)
{
    snprintf(header, HMAX, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", stored_len);
    memcpy(body, stored_data, stored_len);

    HSIZE = strlen(header);
    BSIZE = stored_len;

    send_response(clientfd);

    struct epoll_event rmevent;
    epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &rmevent);
}

void handle_GET_filereq(int clientfd, char* request)
{
    char path[100] = "./";

    // check if GET format correct
    char* temp = strstr(request, "GET");
    if (*(temp+3) != ' '){
        sendError(400, clientfd);
        return;
    }

    char* start = strstr(request, "GET ") + 4;
    char* end = strstr(start, " HTTP");
    strncat(path, start, end - start);


    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        sendError(404, clientfd);
        return;
    }

    // find out file size
    struct stat file_stat;
    int res = fstat(fd, &file_stat);
    int file_size = file_stat.st_size;

    if (res == -1 || !S_ISREG(file_stat.st_mode)) {
        sendError(404, clientfd);
        return;
    }

    char* file_data = malloc(file_size);

    ssize_t total = 0;
    ssize_t amt;
    while ((amt = read(fd, file_data + total, BMAX)) > 0) {
        total += amt;
    }

    // send header
    snprintf(header, HMAX, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", total);
    send(clientfd, header, strlen(header), 0);

    // send body (file)
    send(clientfd, file_data, total, 0);
}

void handle_GET_concfilereq(int clientfd, char* request, int epfd)
{
    char path[100] = "./";

    // check if GET format correct
    char* temp = strstr(request, "GET");
    if (*(temp+3) != ' '){
        sendError(400, clientfd);
        client_file_fd[clientfd] = -1;
        return;
    }

    char* start = strstr(request, "GET ") + 4;
    char* end = strstr(start, " HTTP");
    strncat(path, start, end - start);


    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        sendError(404, clientfd);
        client_file_fd[clientfd] = fd;
        return;
    }

    // find out file size
    struct stat file_stat;
    int res = fstat(fd, &file_stat);
    int file_size = file_stat.st_size;

    if (res == -1 || !S_ISREG(file_stat.st_mode)) {
        sendError(404, clientfd);
        return;
    }

    // send header
    snprintf(header, HMAX, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", file_size);
    send(clientfd, header, strlen(header), 0);

    client_file_fd[clientfd] = fd;


}




static void handle_request(int clientfd, int epfd)
{
   ssize_t amt = recv(clientfd, request, RMAX, 0);
   if (amt <= 0) {
    return; 
    close(clientfd);
   }
   request[amt] = '\0';

   int res = checkRequest(request);
   if (res) {
        sendError(res, clientfd);
        close(clientfd);
        return;
   }

   if (strstr(request, "/hello")) {
    handle_hello_req(clientfd);
    struct epoll_event rmevent;
    epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &rmevent);
    close(clientfd);
   }
   else if (strstr(request, "/headers")) {
    handle_header_req(clientfd, request);
    close(clientfd);
   }
   else if (strstr(request, "POST /data")) {
    handle_POST_req(clientfd, request);
    close(clientfd);
   }
   else if (strstr(request, "GET /stored")) {
    handle_GET_datareq(clientfd, request, epfd);
    close(clientfd);
   }
   else if (strstr(request, "GET /")) {
    handle_GET_concfilereq(clientfd, request, epfd); // send header

    
    struct epoll_event rmevent;
    epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &rmevent);

    
    struct epoll_event Event;
    memset(&Event, 0x00, sizeof(Event));
    Event.events = EPOLLOUT;
    Event.data.fd = clientfd;

    epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &Event);
   }
   else {
    sendError(400, clientfd);
    close(clientfd);
   }
}



void send_chunk_body(int clientfd, int epfd)
{
    int fd = client_file_fd[clientfd];
    if (fd < 0) {
        close(fd);
        client_file_fd[clientfd] = -1;

        struct epoll_event rmevent;
        epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &rmevent);
        close(clientfd);
        return;
    }

    char buf[BMAX];
    ssize_t amt = read(fd, buf, BMAX);

    if (amt > 0) {
        send(clientfd, buf, amt, 0);
    }

    if (amt <= 0) { // sent all chunks
        close(fd);
        client_file_fd[clientfd] = -1;

        struct epoll_event rmevent;
        epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &rmevent);
        close(clientfd);
    }
}



int
main(int argc, char * argv[])
{
    assert(argc == 2);
    int port = atoi(argv[1]);
    int listenfd = open_listenfd(port);
    int epfd = epoll_create1(0);

    add_to_interestlist(epfd, listenfd);

    // concurrent
    while (1)
    {

        struct epoll_event event_list[EMAX];
        int nfds = epoll_wait(epfd, event_list, EMAX, -1);

        for (int i = 0; i < nfds; i++)
        {
            int evfd = event_list[i].data.fd;
            int clientfd;
            if (event_list[i].events == EPOLLIN) {

                if (evfd == listenfd){ // listenfd ready for reading
                    clientfd = accept_connection(listenfd);
                    add_to_interestlist(epfd, clientfd);
                }
                else { // client ready for reading
                    handle_request(evfd, epfd);
                }
            }
            else { // EPOLLOUT client ready for writing
                send_chunk_body(evfd, epfd);
                //close(clientfd);
            }

        }
    }
    

    // while (1)
    // {
    //     int clientfd = accept_connection(listenfd);
    //     handle_request(clientfd);
    //     close(clientfd);
    // }

    return 0;
}
