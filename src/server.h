#define _GNU_SOURCE
#pragma once 


#include <stdint.h>
#include <pthread.h>
#include <sys/uio.h>


#include "strext.h"
#include "trie.h"
#include "ae.h"
#include "server.h"
#include "http_parser.h"
#include "hash.h"
#include "list.h"

#if defined(DEBUG)
#define DBG(fmt,...) do {printf("[DEBUG] " fmt "\n", ##__VA_ARGS__);} while(0)
#else
#define DBG(fmt,...) (void)(fmt)
#endif

#define WARN(fmt,...) do {printf("[WARN] " fmt "\n",  ##__VA_ARGS__);} while(0)
#define DIE(fmt,...) do {printf("[CRITICAL] " fmt "\n",  ##__VA_ARGS__);exit(1);} while(0)


enum http_status {
    HTTP_OK = 200,
    HTTP_PARTIAL_CONTENT = 206,
    HTTP_MOVED_PERMANENTLY = 301,
    HTTP_NOT_MODIFIED = 304,
    HTTP_BAD_REQUEST = 400,
    HTTP_NOT_AUTHORIZED = 401,
    HTTP_FORBIDDEN = 403,
    HTTP_NOT_FOUND = 404,
    HTTP_NOT_ALLOWED = 405,
    HTTP_TIMEOUT = 408,
    HTTP_TOO_LARGE = 413,
    HTTP_RANGE_UNSATISFIABLE = 416,
    HTTP_I_AM_A_TEAPOT = 418,
    HTTP_INTERNAL_ERROR = 500,
    HTTP_NOT_IMPLEMENTED = 501,
    HTTP_UNAVAILABLE = 503,    
};

enum http_handler_flag {
    HANDLER_PARSE_QUERY_STRING = 1<<0,
    HANDLER_PARSE_IF_MODIFIED_SINCE = 1<<1,
    HANDLER_PARSE_RANGE = 1<<2,
    HANDLER_PARSE_ACCEPT_ENCODING = 1<<3,
    HANDLER_PARSE_POST_DATA = 1<<4,
    HANDLER_MUST_AUTHORIZE = 1<<5,
    HANDLER_REMOVE_LEADING_SLASH = 1<<6,
    HANDLER_CAN_REWRITE_URL = 1<<7,
    HANDLER_PARSE_COOKIES = 1<<8,
    HANDLER_DATA_IS_HASH_TABLE = 1<<9,

    HANDLER_PARSE_MASK = 1<<0 | 1<<1 | 1<<2 | 1<<3 | 1<<4 | 1<<8    
};


enum http_request_flag {
    REQUEST_ALL_FLAGS          = -1,
    REQUEST_METHOD_GET         = 1<<0,
    REQUEST_METHOD_HEAD        = 1<<1,
    REQUEST_METHOD_POST        = 1<<2,
    REQUEST_ACCEPT_DEFLATE     = 1<<3,
    REQUEST_ACCEPT_GZIP        = 1<<4,
    REQUEST_IS_HTTP_1_0        = 1<<5,
    REQUEST_ALLOW_PROXY_REQS   = 1<<6,
    REQUEST_PROXIED            = 1<<7,

    RESPONSE_SENT_HEADERS      = 1<<8,
    RESPONSE_CHUNKED_ENCODING  = 1<<9,
    RESPONSE_NO_CONTENT_LENGTH = 1<<10,
    RESPONSE_URL_REWRITTEN     = 1<<11
};

enum http_connection_flag {
    CONN_MASK               = -1,
    CONN_KEEP_ALIVE         = 1<<0,
    CONN_IS_ALIVE           = 1<<1,
    CONN_SHOULD_RESUME_CORO = 1<<2,
    CONN_WRITE_EVENTS       = 1<<3,
    CONN_MUST_READ          = 1<<4,
    CONN_FLIP_FLAGS         = 1<<5,
};

static inline int32_t string_as_int32(const char *s)
{
    int32_t i;
    memcpy(&i, s, sizeof(int32_t));
    return i;
}

static inline int16_t string_as_int16(const char *s)
{
    int16_t i;
    memcpy(&i, s, sizeof(int16_t));
    return i;
}

#define STRING_SWITCH(s) switch (string_as_int32(s))
#define STRING_SWITCH_L(s) switch (string_as_int32(s) | 0x20202020)
    
#define STRING_SWITCH_SMALL(s) switch (string_as_int16(s))
#define STRING_SWITCH_SMALL_L(s) switch (string_as_int16(s) | 0x2020)

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define MULTICHAR_CONSTANT(a,b,c,d) ((int32_t)((a) | (b) << 8 | (c) << 16 | (d) << 24))
#  define MULTICHAR_CONSTANT_SMALL(a,b) ((int16_t)((a) | (b) << 8))
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define MULTICHAR_CONSTANT(d,c,b,a) ((int32_t)((a) | (b) << 8 | (c) << 16 | (d) << 24))
#  define MULTICHAR_CONSTANT_SMALL(b,a) ((int16_t)((a) | (b) << 8))
#elif __BYTE_ORDER__ == __ORDER_PDP_ENDIAN__
#  error A PDP? Seriously?
#endif

#define MULTICHAR_CONSTANT_L(a,b,c,d) (MULTICHAR_CONSTANT(a,b,c,d) | 0x20202020)
#define MULTICHAR_CONSTANT_SMALL_L(a,b) (MULTICHAR_CONSTANT_SMALL(a,b) | 0x2020)


typedef struct key_value {
    char *key;
    size_t key_len;
    char *value;
    size_t value_len;
} kv_t;

static kv_t *kv_new(char *key, size_t key_len, char *value, size_t value_len)
{
    kv_t *kv = malloc(sizeof(kv_t));
    if (!kv)
        return NULL;
    kv->key = key;
    kv->key_len = key_len;
    kv->value = value;
    kv->value_len = value_len;

    return kv;
}

typedef struct content {
    char *value;
    size_t len;     /* strlen of value */
    size_t sz;      /* sizeof *value */

    time_t mtime;
    char etag[16];
} content_t;

static content_t *content_new(char *value, size_t len, size_t sz) {
    content_t *cont = malloc(sizeof(content_t));
    if (!cont)
        return NULL;
    cont->value = value;
    cont->len = len;
    cont->sz = sz;
    cont->mtime = 0;
    cont->etag[0] = 0;
    return cont;
}

static void content_free(void *s) {
    content_t *cont= s;
    if (!cont)
        return;

    if (!cont->value)
        free(cont->value);

    free(cont);
}


struct http_request;
#define MAX_HEADER_LINES 128
struct http_response {
    enum http_status status;
    strbuf *sbuf; //static and main 
    strbuf *head_sbuf;
    strbuf *foot_sbuf;
    const char *mime_type;
    size_t content_length;

    kv_t headers[MAX_HEADER_LINES];
    int headers_sz;
    int curr_header;

    char *header;
    int iovec_sz;
    int curr_iov;
    struct iovec *iovec_buf;
    ssize_t total_written;
    
    struct client *parent_client;
};

struct http_request {
    content_t buf;

    http_parser *parser;

    char *path;
    char *query;
    char *mtime;        // If Modified Since
    struct url_map *um;

    kv_t headers[MAX_HEADER_LINES];
    int headers_sz;
    int curr_header;
    int last_was_value;

    struct client *parent_client;
};


struct client {
    uint64_t id;
    int fd;
    time_t ttl;
    
    aeEventLoop *loop;
    
    struct http_request req;
    struct http_response resp;
};


struct cfg {
    uint16_t port;
    uint8_t thrd_nr;
    char *dir;
    char *ip;
};

struct status {
    uint64_t static_files_cached_time;
    
    uint64_t get_cnt;
    uint64_t post_cnt;
    
};

struct thrd {
    aeEventLoop *loop;
    pthread_t self;
};


struct server {
    int fd;
    int running;
    /* config and status*/
    struct cfg cfg;
    struct status status;
    
    /* runtime */
    http_parser_settings parser_settings;

    struct trie url_map;
    pthread_mutex_t mtx;
    struct thrd *threads;
    
    struct hash *cache; // key value cache.

    struct list_head *blogs; // mainly for blog info.
};


struct url_map {
    enum http_status (*handler)(void *data);
    void *data;
    
    const char *prefix;
    size_t prefix_len;
    enum http_handler_flag flags;
};

struct blog_info {
    int id;
    time_t timestamp;
    char *heading;
    char *sub_heading;
    char *author;
    char *author_link;
};

struct blog {
    struct list_node blogs;
    struct blog_info info;
    char *content;
};


void http_set_url_map(struct server *svr, const struct url_map *map);
void mime_tables_init(void);
void mime_tables_shutdown(void);
void free_blogs_list(struct list_head *head);

int req_url_cb(http_parser *parser, const char *at, size_t length);
int req_header_field_cb(http_parser *parser, const char *at, size_t length);
int req_header_value_cb(http_parser *parser, const char *at, size_t length);
int req_headers_complete_cb(http_parser *parser);

void accept_proc(aeEventLoop *loop, int fd, void *data, int mask);
int server_cron(struct aeEventLoop *loop, long long id, void *data);
int task_cron(struct aeEventLoop *loop, long long id, void *data);
void before_sleep(struct aeEventLoop *loop);

enum http_status blogs(void *data);
enum http_status static_files(void *data);

int refresh_index_page(void);

