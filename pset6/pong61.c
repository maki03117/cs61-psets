#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>
#include "serverinfo.h"

static const char* pong_host = PONG_HOST;
static const char* pong_port = PONG_PORT;
static const char* pong_user = PONG_USER;
static struct addrinfo* pong_addr;

// Maximum number of concurrent threads
#define MAX_THREADS 30

// Maximum number of concurrent connections
#define MAX_CONNS 5

// concurrent threads
int concurrent_threads = 1;

// indicate whether currently in loss period
int loss_period = 0;

// TIME HELPERS
double elapsed_base = 0;

// timestamp()
//    Return the current absolute time as a real number of seconds.
double timestamp(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec + (double) now.tv_usec / 1000000;
}

// elapsed()
//    Return the number of seconds that have elapsed since `elapsed_base`.
double elapsed(void) {
    return timestamp() - elapsed_base;
}


// HTTP CONNECTION MANAGEMENT

// http_connection
//    This object represents an open HTTP connection to a server.
typedef struct http_connection http_connection;
struct http_connection {
    int fd;                 // Socket file descriptor

    int state;              // Response parsing status (see below)
    int status_code;        // Response status code (e.g., 200, 402)
    size_t content_length;  // Content-Length value
    int has_content_length; // 1 iff Content-Length was provided
    int eof;                // 1 iff connection EOF has been reached

    char buf[BUFSIZ];       // Response buffer
    size_t len;             // Length of response buffer
};

// connnection counter
int nconnections = 0;


// connection_table
//      This object represents available connections
typedef struct connection_table connection_table;
struct connection_table
{
    http_connection* connection;
    connection_table* next;

};

void insert_connection(connection_table* table, http_connection* conn) {
    nconnections++;
    connection_table* entry = malloc(sizeof(connection_table));
    entry->connection = conn;
    entry->next = table;
    table = entry;
    fprintf(stderr, "insert_connection\n");
}

http_connection* remove_connection(connection_table* table) {
    nconnections--;
    connection_table* entry = table;
    http_connection* conn = entry->connection;
    table = entry->next;
    free(entry);
    return conn;
}

void delete_table(connection_table* table) {
    for (connection_table* next = table; table != NULL; next = table->next) {
        free(next);
    }
}

// Connection table of available connections
connection_table* available_connections = NULL;

pthread_mutex_t mutex;
pthread_cond_t condvar;


// `http_connection::state` constants
#define HTTP_REQUEST 0      // Request not sent yet
#define HTTP_INITIAL 1      // Before first line of response
#define HTTP_HEADERS 2      // After first line of response, in headers
#define HTTP_BODY    3      // In body
#define HTTP_DONE    (-1)   // Body complete, available for a new request
#define HTTP_CLOSED  (-2)   // Body complete, connection closed
#define HTTP_BROKEN  (-3)   // Parse error

// helper functions
char* http_truncate_response(http_connection* conn);
static int http_process_response_headers(http_connection* conn);
static int http_check_response_body(http_connection* conn);

static void usage(void);


// http_connect(ai)
//    Open a new connection to the server described by `ai`. Returns a new
//    `http_connection` object for that server connection. Exits with an
//    error message if the connection fails.
http_connection* http_connect(const struct addrinfo* ai) {
    // connect to the server
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    int yes = 1;
    (void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (r < 0) {
        perror("connect");
        exit(1);
    }

    // construct an http_connection object for this connection
    // if there's a connection available in the table...use that connection
    // http_connection* conn = available_connections ? remove_connection(available_connections) : (http_connection*) malloc(sizeof(http_connection));
    http_connection* conn;
    if (available_connections != NULL){
        fprintf(stderr, "Inside here!\n");
        pthread_mutex_lock(&mutex);
        conn = remove_connection(available_connections);
        pthread_mutex_unlock(&mutex);

    }
    else {
        conn = (http_connection*) malloc(sizeof(http_connection));
    }
    conn->fd = fd; 
    conn->state = HTTP_REQUEST;
    conn->eof = 0;
    return conn;
}


// http_close(conn)
//    Close the HTTP connection `conn` and free its resources.
void http_close(http_connection* conn) {
    close(conn->fd);

    if (conn->state == HTTP_DONE){

        // connection is now available - add to table of connections
        pthread_mutex_lock(&mutex);
        insert_connection(available_connections, conn); 
        fprintf(stderr, "Number of connections: %d\n", nconnections);
        pthread_mutex_unlock(&mutex);

    }
    else{
        free(conn);
    }
}


// http_send_request(conn, uri)
//    Send an HTTP POST request for `uri` to connection `conn`.
//    Exit on error.
void http_send_request(http_connection* conn, const char* uri) {
    assert(conn->state == HTTP_REQUEST || conn->state == HTTP_DONE);

    // prepare and write the request
    char reqbuf[BUFSIZ];
    size_t reqsz = sprintf(reqbuf,
                           "POST /%s/%s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n",
                           pong_user, uri, pong_host);
    size_t pos = 0;
    while (pos < reqsz) {
        ssize_t nw = write(conn->fd, &reqbuf[pos], reqsz - pos);
        if (nw == 0)
            break;
        else if (nw == -1 && errno != EINTR && errno != EAGAIN) {
            perror("write");
            exit(1);
        } else if (nw != -1)
            pos += nw;
    }

    if (pos != reqsz) {
        fprintf(stderr, "%.3f sec: connection closed prematurely\n",
                elapsed());
        exit(1);
    }

    // clear response information
    conn->state = HTTP_INITIAL;
    conn->status_code = -1;
    conn->content_length = 0;
    conn->has_content_length = 0;
    conn->len = 0;
}


// http_receive_response_headers(conn)
//    Read the server's response headers. On return, `conn->status_code`
//    holds the server's status code. If the connection terminates
//    prematurely, `conn->status_code` is -1.
void http_receive_response_headers(http_connection* conn) {
    assert(conn->state != HTTP_REQUEST);
    if (conn->state < 0)
        return;

    // read & parse data until told `http_process_response_headers`
    // tells us to stop
    while (http_process_response_headers(conn)) {
        ssize_t nr = read(conn->fd, &conn->buf[conn->len], BUFSIZ);
        if (nr == 0)
            conn->eof = 1;
        else if (nr == -1 && errno != EINTR && errno != EAGAIN) {
            perror("read");
            exit(1);
        } else if (nr != -1)
            conn->len += nr;
    }

    // Status codes >= 500 mean we are overloading the server
    // and should exit.
    if (conn->status_code >= 500) {
        fprintf(stderr, "%.3f sec: exiting because of "
                "server status %d (%s)\n", elapsed(),
                conn->status_code, http_truncate_response(conn));
        exit(1);
    }

}


// http_receive_response_body(conn)
//    Read the server's response body. On return, `conn->buf` holds the
//    response body, which is `conn->len` bytes long and has been
//    null-terminated.
void http_receive_response_body(http_connection* conn) {
    assert(conn->state < 0 || conn->state == HTTP_BODY);
    if (conn->state < 0)
        return;

    // read response body (http_check_response_body tells us when to stop)
    while (http_check_response_body(conn)) {
        ssize_t nr = read(conn->fd, &conn->buf[conn->len], BUFSIZ);
        if (nr == 0)
            conn->eof = 1;
        else if (nr == -1 && errno != EINTR && errno != EAGAIN) {
            perror("read");
            exit(1);
        } else if (nr != -1)
            conn->len += nr;
    }

    // null-terminate body
    conn->buf[conn->len] = 0;
}


// http_truncate_response(conn)
//    Truncate the `conn` response text to a manageable length and return
//    that truncated text. Useful for error messages.
char* http_truncate_response(http_connection* conn) {
    char *eol = strchr(conn->buf, '\n');
    if (eol)
        *eol = 0;
    if (strnlen(conn->buf, 100) >= 100)
        conn->buf[100] = 0;
    return conn->buf;
}


// MAIN PROGRAM

typedef struct pong_args {
    int x;
    int y;
} pong_args;

// pong_thread(threadarg)
//    Connect to the server at the position indicated by `threadarg`
//    (which is a pointer to a `pong_args` structure).
void* pong_thread(void* threadarg) {
    pthread_detach(pthread_self());

    // Copy thread arguments onto our stack.
    pong_args pa = *((pong_args*) threadarg);

    char url[256];
    snprintf(url, sizeof(url), "move?x=%d&y=%d&style=on",
             pa.x, pa.y);



    pthread_mutex_lock(&mutex);
    while (nconnections > 5) {
        fprintf(stderr, "aghgghghg\n");
        // wait until that thread signls us to continue
        pthread_cond_wait(&condvar, &mutex);
    }
    pthread_mutex_unlock(&mutex);

    http_connection* conn = http_connect(pong_addr);

    // Exponential backoff for each attempt
    long long backoff = 1;

    do {

        // It it's not the first attempt...
        if (backoff != 1) {
            // ... indicate currently in loss period
            loss_period = 1;
            fprintf(stderr, "setting loss_period to 1\n");
            // ... close the previous connection 
            http_close(conn);
            // ... wait to retry (using exponential backoff)
            usleep(backoff * 100000);
            // ... retry the connection
            conn = http_connect(pong_addr);
        }

        http_send_request(conn, url);
        http_receive_response_headers(conn);

        backoff *= 2;

    } while (conn->state == HTTP_BROKEN || conn->status_code == -1);

   
    // No longer in loss period
    loss_period = 0;

     // signal the main thread to continue
    pthread_cond_signal(&condvar); // MOVE TO RIGHT AFTER YOU RECEIVE RESPONSE HEADERS


    fprintf(stderr, "setting loss_period to 0\n");

    if (conn->status_code != 200)
        fprintf(stderr, "%.3f sec: warning: %d,%d: "
                "server returned status %d (expected 200)\n",
                elapsed(), pa.x, pa.y, conn->status_code);

    http_receive_response_body(conn);
    double result = strtod(conn->buf, NULL);
    if (result < 0) {
        fprintf(stderr, "%.3f sec: server returned error: %s\n",
                elapsed(), http_truncate_response(conn));
        exit(1);
    }

    http_close(conn);

    concurrent_threads--;
    
    // and exit!
    pthread_exit(NULL);
}


// usage()
//    Explain how pong61 should be run.
static void usage(void) {
    fprintf(stderr, "Usage: ./pong61 [-h HOST] [-p PORT] [USER]\n");
    exit(1);
}


// main(argc, argv)
//    The main loop.
int main(int argc, char** argv) {
    // parse arguments
    int ch, nocheck = 0;
    while ((ch = getopt(argc, argv, "nh:p:u:")) != -1) {
        if (ch == 'h')
            pong_host = optarg;
        else if (ch == 'p')
            pong_port = optarg;
        else if (ch == 'u')
            pong_user = optarg;
        else if (ch == 'n')
            nocheck = 1;
        else
            usage();
    }
    if (optind == argc - 1)
        pong_user = argv[optind];
    else if (optind != argc)
        usage();

    // look up network address of pong server
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    int r = getaddrinfo(pong_host, pong_port, &hints, &pong_addr);
    if (r != 0) {
        fprintf(stderr, "problem looking up %s: %s\n",
                pong_host, gai_strerror(r));
        exit(1);
    }

    // reset pong board and get its dimensions
    int width, height;
    {
        http_connection* conn = http_connect(pong_addr);
        http_send_request(conn, nocheck ? "reset?nocheck=1" : "reset");
        http_receive_response_headers(conn);
        http_receive_response_body(conn);
        if (conn->status_code != 200
            || sscanf(conn->buf, "%d %d\n", &width, &height) != 2
            || width <= 0 || height <= 0) {
            fprintf(stderr, "bad response to \"reset\" RPC: %d %s\n",
                    conn->status_code, http_truncate_response(conn));
            exit(1);
        }
        http_close(conn);
    }
    // measure future times relative to this moment
    elapsed_base = timestamp();

    // print display URL
    printf("Display: http://%s:%s/%s/%s\n",
           pong_host, pong_port, pong_user,
           nocheck ? " (NOCHECK mode)" : "");

    // initialize global synchronization objects
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&condvar, NULL);

    // play game
    int x = 0, y = 0, dx = 1, dy = 1;
    char url[BUFSIZ];
    
    while (1) {
        // create a new thread to handle the next position
        pong_args pa;
        pa.x = x;
        pa.y = y;
        pthread_t pt;

        concurrent_threads++;
        fprintf(stderr, "starting new thread\n");

        // if (concurrent_threads >= MAX_THREADS || loss_period) {
        //     // wait until that thread signals us to continue
        //     pthread_mutex_lock(&mutex);
        //     pthread_cond_wait(&condvar, &mutex);
        //     pthread_mutex_unlock(&mutex);
            
        // }

        loss_period = 1;

        r = pthread_create(&pt, NULL, pong_thread, &pa);

        if (r != 0) {
            fprintf(stderr, "%.3f sec: pthread_create: %s\n",
                    elapsed(), strerror(r));
            exit(1);
        }
        
        // If we already have too many concurrent threads...

        pthread_mutex_lock(&mutex);
        while (concurrent_threads >= MAX_THREADS || loss_period) {
            fprintf(stderr, "we are STUCK\n");
            // wait until that thread signals us to continue
            pthread_cond_wait(&condvar, &mutex);
        }
        pthread_mutex_unlock(&mutex);
        assert(concurrent_threads < MAX_THREADS && !loss_period);
        fprintf(stderr, "checked loss_period and moved on\n");

        // update position
        x += dx;
        y += dy;
        if (x < 0 || x >= width) {
            dx = -dx;
            x += 2 * dx;
        }
        if (y < 0 || y >= height) {
            dy = -dy;
            y += 2 * dy;
        }

        // wait 0.1sec
        usleep(100000);
    }
}


// HTTP PARSING

// http_process_response_headers(conn)
//    Parse the response represented by `conn->buf`. Returns 1
//    if more header data remains to be read, 0 if all headers
//    have been consumed.
static int http_process_response_headers(http_connection* conn) {
    size_t i = 0;
    while ((conn->state == HTTP_INITIAL || conn->state == HTTP_HEADERS)
           && i + 2 <= conn->len) {
        if (conn->buf[i] == '\r' && conn->buf[i+1] == '\n') {
            conn->buf[i] = 0;
            if (conn->state == HTTP_INITIAL) {
                int minor;
                if (sscanf(conn->buf, "HTTP/1.%d %d",
                           &minor, &conn->status_code) == 2)
                    conn->state = HTTP_HEADERS;
                else
                    conn->state = HTTP_BROKEN;
            } else if (i == 0)
                conn->state = HTTP_BODY;
            else if (strncmp(conn->buf, "Content-Length: ", 16) == 0) {
                conn->content_length = strtoul(conn->buf + 16, NULL, 0);
                conn->has_content_length = 1;
            }
            memmove(conn->buf, conn->buf + i + 2, conn->len - (i + 2));
            conn->len -= i + 2;
            i = 0;
        } else
            ++i;
    }

    if (conn->eof)
        conn->state = HTTP_BROKEN;
    return conn->state == HTTP_INITIAL || conn->state == HTTP_HEADERS;
}


// http_check_response_body(conn)
//    Returns 1 if more response data should be read into `conn->buf`,
//    0 if the connection is broken or the response is complete.
static int http_check_response_body(http_connection* conn) {
    if (conn->state == HTTP_BODY
        && (conn->has_content_length || conn->eof)
        && conn->len >= conn->content_length)
        conn->state = HTTP_DONE;

    if (conn->eof && conn->state == HTTP_DONE)
        conn->state = HTTP_CLOSED;
    else if (conn->eof)
        conn->state = HTTP_BROKEN;
    return conn->state == HTTP_BODY;
}