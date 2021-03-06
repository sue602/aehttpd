/*
 * aehttpd - redis ae engine based simple http server.
 */


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
#include <zlib.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>


#include "server.h"
#include "anet.h"
#include "strext.h"
#include "mime-types.h"
#include "json.h"
#include "hash.h"
#include "tmpl.h"

#define INT2STR_BUF_SZ (3 * sizeof(size_t) + 1)

extern struct server g_svr;

char *uint_to_string(size_t value, char dst[INT2STR_BUF_SZ], size_t *len_out)
{
    /*
     * Based on routine by A. Alexandrescu, licensed under CC0
     * https://creativecommons.org/publicdomain/zero/1.0/legalcode
     */
    static const size_t length = INT2STR_BUF_SZ;
    size_t next = length - 1;
    static const char digits[201] =
            "0001020304050607080910111213141516171819"
            "2021222324252627282930313233343536373839"
            "4041424344454647484950515253545556575859"
            "6061626364656667686970717273747576777879"
            "8081828384858687888990919293949596979899";
    dst[next--] = '\0';
    while (value >= 100) {
        const uint32_t i = (uint32_t) ((value % 100) * 2);
        value /= 100;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
        next -= 2;
    }
    // Handle last 1-2 digits
    if (value < 10) {
        dst[next] = (char) ('0' + (uint32_t) value);
        *len_out = length - next - 1;
        return dst + next;
    }
    uint32_t i = (uint32_t) value * 2;
    dst[next] = digits[i + 1];
    dst[next - 1] = digits[i];
    *len_out = length - next;
    return dst + next - 1;
}

char *int_to_string(ssize_t value, char dst[INT2STR_BUF_SZ], size_t *len_out)
{
    if (value < 0) {
        char *p = uint_to_string((size_t) - value, dst, len_out);
        *--p = '-';
        ++*len_out;

        return p;
    }

    return uint_to_string((size_t)value, dst, len_out);
}

static void destroy_urlmap(void *data)
{
    struct url_map *urlmap = data;
    free((char *)urlmap->prefix);
    free(urlmap);
}

static struct url_map *
add_url_map(struct trie *t, const char *prefix, const struct url_map *map)
{
    struct url_map *um = malloc(sizeof(*um));
    if (!um)
        DIE("could not malloc for adding url map\n");
    
    memcpy(um, map, sizeof(*um));
    
    um->prefix = strdup(prefix ? prefix : um->prefix);
    um->prefix_len = strlen(um->prefix);
    trie_add(t, um->prefix, um);
    
    return um;
}

void http_set_url_map(struct server *svr, const struct url_map *map)
{
    trie_destroy(&svr->url_map);
    if (!trie_init(&svr->url_map, destroy_urlmap)) {
        DIE("could not init trie url map\n");
    }
    
    for (; map->prefix; map++) {
        struct url_map *um = add_url_map(&svr->url_map, NULL, map);
        if (!um)
            continue;
        
        um->flags = HANDLER_PARSE_MASK;
    }
}

#define FREE(ptr_) do {if (ptr_) {free(ptr_);ptr_=NULL;}} while(0)
void free_client(struct client *c) {
    if (!c)
        return;
    
    if (c->loop && (c->fd > 0)) {
        aeDeleteFileEvent(c->loop, c->fd, AE_READABLE);
        aeDeleteFileEvent(c->loop, c->fd, AE_WRITABLE);
        close(c->fd);
    }

    FREE(c->req.parser);
    FREE(c->req.path);
    FREE(c->req.query);
    int i;
    for (i = 0; i < c->req.headers_sz; i++) {
        if (c->req.headers[i].key) 
            free(c->req.headers[i].key);
        if (c->req.headers[i].value)
            free(c->req.headers[i].value);
    }

    FREE(c->resp.header);
    FREE(c->resp.iovec_buf);
    for (i = 0; i < c->resp.headers_sz; i++) {
        if (c->resp.headers[i].key) 
            free(c->resp.headers[i].key);
        if (c->resp.headers[i].value)
            free(c->resp.headers[i].value);
    }

    strbuf_free(c->resp.sbuf);
    strbuf_free(c->resp.head_sbuf);
    strbuf_free(c->resp.foot_sbuf);

    FREE(c->req.buf.value);
    
    free(c);
}
#undef FREE

/* Per client per connection. */
struct client *create_client(int fd) {
    if (fd < 0)
        return NULL;
    
    struct client *c = malloc(sizeof(struct client));
    if (!c)
        return NULL;
    memset(c, 0, sizeof(struct client));
    
    c->fd = fd;
    anetNonBlock(NULL, fd);
    anetEnableTcpNoDelay(NULL, fd);

    int i = fd % g_svr.cfg.thrd_nr;
    c->loop = g_svr.threads[i].loop;

    c->req.buf.len = 0;
    c->req.buf.sz = 8192; // 8KB limit for method other than POST.
    c->req.buf.value = malloc(c->req.buf.sz);
    if (!c->req.buf.value) {
        free_client(c);
        return NULL;
    }

    return c;
}

void page_304(int fd) {
    write(fd, "HTTP/1.1 304 Not Modified\n"
            "Content-length: 52\n"
            "Content-Type: text/html\n\n"
            "<html><body><H1>304: Not Modified</H1></body></html>",
            16+19+25+52);
}

void page_404(int fd) {
    write(fd, "HTTP/1.1 404 Page Not Found\n"
            "Content-length: 54\n"
            "Content-Type: text/html\n\n"
            "<html><body><H1>404: Page Not Found</H1></body></html>",
            16+19+25+54);
}


void page_418(int fd) {
    write(fd, "HTTP/1.1 418 I'm a teapot\n"
            "Content-length: 52\n"
            "Content-Type: text/html\n\n"
            "<html><body><H1>418: I'm a teapot</H1></body></html>",
            16+19+25+52);
}


void page_500(int fd) {
    write(fd, "HTTP/1.1 500 Internal server error\n"
            "Content-length: 61\n"
            "Content-Type: text/html\n\n"
            "<html><body><H1>500: Internal server error</H1></body></html>",
            16+19+25+61);
}

#define RETURN_0_ON_OVERFLOW(len_) \
    if (p_headers + (len_) >= p_headers_end) return 0

#define APPEND_STRING_LEN(const_str_,len_) \
    do { \
        RETURN_0_ON_OVERFLOW(len_); \
        p_headers = (char *)memcpy(p_headers, (const_str_), (len_))+(len_); \
    } while(0)

#define APPEND_STRING(str_) \
    do { \
        size_t len = strlen(str_); \
        APPEND_STRING_LEN((str_), len); \
    } while(0)

#define APPEND_CHAR(value_) \
    do { \
        RETURN_0_ON_OVERFLOW(1); \
        *p_headers++ = (value_); \
    } while(0)

#define APPEND_CHAR_NOCHECK(value_) \
    *p_headers++ = (value_)

#define APPEND_UINT(value_) \
    do { \
        size_t len; \
        char *tmp = uint_to_string((value_), buffer, &len); \
        RETURN_0_ON_OVERFLOW(len); \
        APPEND_STRING_LEN(tmp, len); \
    } while(0)

#define APPEND_CONSTANT(const_str_) \
    APPEND_STRING_LEN((const_str_), sizeof(const_str_) - 1)

static unsigned char uncompressed_mime_entries[MIME_UNCOMPRESSED_LEN];
static struct mime_entry mime_entries[MIME_ENTRIES];
static bool mime_entries_initialized = false;

void mime_tables_init(void)
{
    if (mime_entries_initialized)
        return;

    uLongf uncompressed_length = MIME_UNCOMPRESSED_LEN;
    int ret = uncompress((Bytef*)uncompressed_mime_entries,
            &uncompressed_length, (const Bytef*)mime_entries_compressed,
            MIME_COMPRESSED_LEN);
    if (ret != Z_OK)
        DIE("Error while uncompressing table: zlib error %d", ret);

    if (uncompressed_length != MIME_UNCOMPRESSED_LEN)
        DIE("Expected uncompressed length %d, got %ld",
                MIME_UNCOMPRESSED_LEN, uncompressed_length);

    unsigned char *ptr = uncompressed_mime_entries;
    size_t i;
    for (i = 0; i < MIME_ENTRIES; i++) {
        mime_entries[i].extension = (char*)ptr;
        ptr = (char *)memchr(ptr + 1, '\0', MIME_UNCOMPRESSED_LEN) + 1;
        mime_entries[i].type = (char*)ptr;
        ptr = (char *)memchr(ptr + 1, '\0', MIME_UNCOMPRESSED_LEN) + 1;
    }

    mime_entries_initialized = true;
}

void mime_tables_shutdown(void)
{
}

static int compare_mime_entry(const void *a, const void *b)
{
    const struct mime_entry *me1 = a;
    const struct mime_entry *me2 = b;
    return strcmp(me1->extension, me2->extension);
}

const char * file_mime_type(const char *file_name)
{
    char *last_dot = strrchr(file_name, '.');
    if (!last_dot)
        goto fallback;

    enum {
        EXT_JPG = MULTICHAR_CONSTANT_L('.','j','p','g'),
        EXT_PNG = MULTICHAR_CONSTANT_L('.','p','n','g'),
        EXT_HTM = MULTICHAR_CONSTANT_L('.','h','t','m'),
        EXT_CSS = MULTICHAR_CONSTANT_L('.','c','s','s'),
        EXT_TXT = MULTICHAR_CONSTANT_L('.','t','x','t'),
        EXT_JS  = MULTICHAR_CONSTANT_L('.','j','s',0x20),
    };

    STRING_SWITCH_L(last_dot) {
    case EXT_CSS:
        return "text/css";
    case EXT_HTM:
        return "text/html";
    case EXT_JPG:
        return "image/jpeg";
    case EXT_JS:
        return "application/javascript";
    case EXT_PNG:
        return "image/png";
    case EXT_TXT:
        return "text/plain";
    }

    if (*last_dot) {
        struct mime_entry *entry, key = { .extension = last_dot + 1 };

        entry = bsearch(&key, mime_entries, MIME_ENTRIES,
                       sizeof(struct mime_entry), compare_mime_entry);
        if (entry)
            return entry->type;
    }

fallback:
    return "application/octet-stream";
}


const char *
http_status_as_string_with_code(enum http_status status)
{
    const char *ret;

#define RESP(code,description)		[code] = #code " " description
    static const char *resps[] = {
        RESP(200, "OK"),
        RESP(206, "Partial content"),
        RESP(301, "Moved permanently"),
        RESP(304, "Not modified"),
        RESP(400, "Bad request"),
        RESP(401, "Not authorized"),
        RESP(403, "Forbidden"),
        RESP(404, "Not found"),
        RESP(405, "Not allowed"),
        RESP(408, "Request timeout"),
        RESP(413, "Request too large"),
        RESP(416, "Requested range unsatisfiable"),
        RESP(418, "I'm a teapot"),
        RESP(500, "Internal server error"),
        RESP(501, "Not implemented"),
        RESP(503, "Service unavailable")
    };
#undef RESP

    ret = status < sizeof(resps)/sizeof(resps[0]) ? resps[status] : NULL;
    return ret ? ret : "999 Invalid";
}

size_t prepare_resp_header(struct client *c, char *headers, size_t buf_size)
{
    struct http_request *req = &c->req;
    struct http_response *resp = &c->resp;
    
    char *p_headers;
    char *p_headers_end = headers + buf_size;
    char buffer[INT2STR_BUF_SZ];

    p_headers = headers;

    APPEND_CONSTANT("HTTP/1.1 ");
    APPEND_STRING(http_status_as_string_with_code(resp->status));
    int i;
    for (i = 0; i < resp->headers_sz; i++) {
        APPEND_STRING(resp->headers[i].key);
        APPEND_STRING(resp->headers[i].value);
    }

    if (resp->status != HTTP_OK) {
        APPEND_CONSTANT("\r\nConnection: close");
        APPEND_CONSTANT("\r\nServer: aehttpd\r\n\r\n\0");
        return (size_t)(p_headers - headers - 1);
    }

    APPEND_CONSTANT("\r\nContent-Length: ");
    
    size_t buf_len = 0;
    if (resp->head_sbuf)
        buf_len = resp->head_sbuf->len.buffer;
    if  (resp->foot_sbuf)
        buf_len += resp->foot_sbuf->len.buffer; 
    buf_len += resp->sbuf->len.buffer;

    APPEND_UINT(buf_len);
    APPEND_CONSTANT("\r\nContent-Type: ");
    APPEND_STRING(resp->mime_type);

    APPEND_CONSTANT("\r\nConnection: close");
    APPEND_CONSTANT("\r\nServer: aehttpd\r\n\r\n\0");


    return (size_t)(p_headers - headers - 1);
}

void free_blog_buf(struct blog *b) {
    if (b->info.heading) free(b->info.heading);
    if (b->info.sub_heading) free(b->info.sub_heading);
    if (b->info.author) free(b->info.author);
    if (b->info.author_link) free(b->info.author_link);
    if (b->content) free(b->content);
}

void free_blogs_list(struct list_head *head)
{
    struct blog *node, *next;
    list_for_each_safe(head, node, next, blogs) {
        list_del(&node->blogs);
        free_blog_buf(node);
        free(node);
    }
    free(head);
}

void write_loop(aeEventLoop *loop, int fd, void *data, int mask) {
    if (!loop || !data)
        return;

    struct client *c = data;
    struct http_request *req = &c->req;
    struct http_response *resp = &c->resp;   

    for (;;) {
        ssize_t nwrite = writev(fd, resp->iovec_buf + resp->curr_iov, 
                resp->iovec_sz - resp->curr_iov);
        if (nwrite < 0) {
            switch (errno) {
                case EAGAIN:
                case EINTR:
                    aeCreateFileEvent(loop, fd, AE_WRITABLE, write_loop, c);  
                    return;
                default:
                    goto out;
            }
        }
        else if (nwrite == 0) {
            goto out;
        }

        while (resp->curr_iov < resp->iovec_sz && 
                nwrite >= (ssize_t)resp->iovec_buf[resp->curr_iov].iov_len) {
            nwrite -= (ssize_t)resp->iovec_buf[resp->curr_iov].iov_len;
            resp->curr_iov++;
        }

        if (resp->curr_iov == resp->iovec_sz)
            break;

        resp->iovec_buf[resp->curr_iov].iov_base = 
                (char *)resp->iovec_buf[resp->curr_iov].iov_base + nwrite;
        resp->iovec_buf[resp->curr_iov].iov_len -= (size_t)nwrite;
    }

out:    
    free_client(c);

}


void write_proc(aeEventLoop *loop, int fd, void *data, int mask) {
    if (!loop || !data)
        return;

    struct client *c = data;
    struct http_request *req = &c->req;
    struct http_response *resp = &c->resp;

    resp->header = malloc(512); 

    
    size_t header_len = prepare_resp_header(c, resp->header, 512);
    if (!header_len) {
        page_500(fd);
        goto out;
    }
    resp->iovec_sz = 1 + (resp->sbuf != 0) + (resp->head_sbuf != 0) + (resp->head_sbuf != 0);
    resp->iovec_buf = calloc(resp->iovec_sz, sizeof(struct iovec));
    if (!resp->iovec_buf) {
        page_500(fd);
        goto out;
    }
    resp->curr_iov = 0;
    resp->total_written = 0;
    int i = 0;
    resp->iovec_buf[i].iov_base = resp->header;
    resp->iovec_buf[i].iov_len = header_len;
    i++;
    
    if (resp->status != HTTP_OK) {
        write_loop(loop, fd, data, mask);
        return;
    }

    if (!resp->mime_type) {
        page_404(fd);
        goto out;
    }


    if (resp->head_sbuf) {
        resp->iovec_buf[i].iov_base = strbuf_get_buffer(resp->head_sbuf);
        resp->iovec_buf[i].iov_len = strbuf_get_length(resp->head_sbuf);
        i++;
    } 
    if (resp->sbuf) {
        resp->iovec_buf[i].iov_base = strbuf_get_buffer(resp->sbuf);
        resp->iovec_buf[i].iov_len = strbuf_get_length(resp->sbuf);
        i++;
    }
    if (resp->foot_sbuf) {
        resp->iovec_buf[i].iov_base = strbuf_get_buffer(resp->foot_sbuf);
        resp->iovec_buf[i].iov_len = strbuf_get_length(resp->foot_sbuf);
        i++;
    } 

    write_loop(loop, fd, data, mask);
    return;
out:
    free_client(c);
}

int req_url_cb(http_parser* parser, const char *at, size_t len) {
    struct client *c = parser->data;

    struct http_parser_url url;
    if (http_parser_parse_url(at, len, 0, &url) == 0) {
        if (url.field_set & (1 << UF_PATH)) {
            c->req.path = strndup(at+url.field_data[UF_PATH].off,
                    url.field_data[UF_PATH].len);
        }
        
        if (url.field_set & (1 << UF_QUERY)) {
            c->req.query = strndup(at+url.field_data[UF_QUERY].off,
                    url.field_data[UF_QUERY].len);
        }
    }
    
    return 0;
}

#define CURR_LINE (&headers[c->req.curr_header])

int req_header_field_cb(http_parser *parser, const char *at, size_t len)
{
    struct client *c = parser->data;
    kv_t *headers = c->req.headers;

    if (c->req.last_was_value) {
        c->req.curr_header++;

        if (c->req.curr_header == MAX_HEADER_LINES)
            return -1;
        
        CURR_LINE->value = NULL;
        CURR_LINE->value_len = 0;

        CURR_LINE->key_len = len;
        CURR_LINE->key = malloc(len+1);
        strncpy(CURR_LINE->key, at, len);
    } else {
        if (CURR_LINE->value) {
            free(CURR_LINE->value);
            CURR_LINE->value = NULL;
        }
        CURR_LINE->value_len = 0;        

        CURR_LINE->key_len += len;
        CURR_LINE->key = realloc(CURR_LINE->key, CURR_LINE->key_len+1);
        strncpy(CURR_LINE->key, at, len);
    }
    
    CURR_LINE->key[CURR_LINE->key_len] = 0;
    c->req.last_was_value = 0;

    return 0;
}

int req_header_value_cb(http_parser *parser, const char *at, size_t len)
{
    struct client *c = parser->data;
    kv_t *headers = c->req.headers;

    if (!c->req.last_was_value) {
        CURR_LINE->value_len = len;
        CURR_LINE->value = malloc(len+1);
        strncpy(CURR_LINE->value, at, len);
    } else {
        CURR_LINE->value_len += len;
        CURR_LINE->value = realloc(CURR_LINE->value, CURR_LINE->value_len+1);
        strncpy(CURR_LINE->value, at, len);
    }

    CURR_LINE->value[CURR_LINE->value_len] = 0;
    c->req.last_was_value = 1;

    return 0;
}

int req_headers_complete_cb(http_parser *parser)
{
    struct client *c = parser->data;
    c->req.headers_sz = c->req.curr_header + 1;

    int i = 0;
    for (i = 0; i < c->req.headers_sz; i++) {
        DBG("%s: %s", c->req.headers[i].key, c->req.headers[i].value);
        if (strcmp(c->req.headers[i].key, "If-Modified-Since") == 0) {
            c->req.mtime = c->req.headers[i].value;
        }
    }

    return 0;
}


void read_proc(aeEventLoop *loop, int fd, void *data, int mask) {
    struct client *c = data;
    
    (void)(mask);
    
    ssize_t nread;
    nread = read(fd, c->req.buf.value, c->req.buf.sz);
    if (nread == -1) {
        if (errno == EAGAIN) {
            WARN("Read from client failed: %s", strerror(errno));
            aeCreateFileEvent(loop, fd, AE_READABLE, read_proc, c);
            return;
        } else {
            WARN("Read from client failed: %s", strerror(errno));
            free_client(c);
            return;
        }
    } else if (nread == 0) {
        DBG("client closed connection");
        free_client(c);
        return; 
    }
    
    c->req.buf.len = nread;
    c->req.buf.value[nread] = 0;

    http_parser *parser = malloc(sizeof(http_parser));
    if (!parser) {
        WARN("create http parser failed.");
        free_client(c);
        return;
    }
        
    http_parser_init(parser, HTTP_REQUEST);
    parser->data = c;
    size_t nparsed;
    
    nparsed = http_parser_execute(parser, &g_svr.parser_settings, 
            c->req.buf.value, c->req.buf.len);

    if (parser->upgrade) {
        /* handle new protocol */
        DBG("new http protocol discovered.");
        free_client(c);
        return;
    } else if (nparsed != nread) {
        /* Handle error. Usually just close the connection. */
        WARN("parse http request failed.");
        free_client(c);
        return;
    }
    c->req.parser = parser;

    c->req.um = trie_lookup_prefix(&g_svr.url_map, c->req.path);
    if (!c->req.um) {
        WARN("unreachable path."); // TODO: return 404 page?
        free_client(c);
        return;
    }

    c->resp.status = c->req.um->handler(c);

    aeCreateFileEvent(c->loop, c->fd, AE_WRITABLE, write_proc, c);  
}


#define MAX_ACCEPTS_PER_CALL 1000
void accept_proc(aeEventLoop *loop, int fd, void *data, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[128];
    
    (void)(loop);
    (void)(mask);
    (void)(data);
    
    while (max--) {
        cfd = anetTcpAccept(NULL, fd, cip, 128, &cport);
        if (cfd == ANET_ERR)
            return;
        
        struct client *c = create_client(cfd);
        int ret = aeCreateFileEvent(c->loop, cfd, AE_READABLE, read_proc, c);
        if (ret == AE_ERR) {
            fprintf(stderr, "can not create ae for reading.\n");
            close(cfd);
        } 
    }
}

content_t *get_file_content(char *path)
{
    content_t *str = hash_find(g_svr.cache, path);
    if (!str) {
        WARN("open %s", path);

        struct stat st;
        if (stat(path, &st) == -1) {
            st.st_mtime = 0;
        }

        FILE *f = fopen(path, "rb");
        if (!f) {
            DBG("fopen failed: %s", path);
            content_t *null_cont = content_new(NULL, 0, 0);
            if (!null_cont) {
                WARN("malloc content failed");
                return NULL;
            }
            hash_add(g_svr.cache, strdup(path), null_cont);
            return NULL;
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc(fsize+1);
        if (!buf)
            return NULL;
        fread(buf, fsize, 1, f);
        fclose(f);
        buf[fsize] = 0;


        content_t *new_cont = content_new(buf, fsize, fsize+1);
        if (!new_cont) {
            DBG("content malloc failed");
            free(buf);
            return NULL;
        }
        new_cont->mtime = st.st_mtime;
        hash_add(g_svr.cache, strdup(path), new_cont);
        return new_cont;
    } else {
        if (str->value == NULL)
            return NULL;
    }

    return str;
}


int server_cron(struct aeEventLoop *loop, long long id, void *data) {
    (void)(loop);
    (void)(id);
    (void)(data);
    if (!g_svr.running) {
        aeStop(loop);
    }

    refresh_index_page();
        
    return 1000;
}

int task_cron(struct aeEventLoop *loop, long long id, void *data) {
    (void)(loop);
    (void)(id);
    (void)(data);

    refresh_index_page();
        
    return 10000;
}

void before_sleep(struct aeEventLoop *loop) {
    (void)(loop);
    /* do something before sleep */
}


/*
 * url handlers.
 */

int build_blog(int id, struct blog *b)
{
    char path[1024];
    snprintf(path, sizeof(path), "./data/blogs/%d", id);
    content_t *str = get_file_content(path);
    if (!str) {
        DBG("get json %s failed.", path);
        return HTTP_NOT_FOUND;
    }

    JsonNode *json = json_decode(str->value);
    if (!json) {
        DBG("decode json %s failed.", path);
        return HTTP_INTERNAL_ERROR;
    }
    JsonNode *tmp;
#define JSON_MEM_STR(key, def) \
        (tmp = json_find_member(json, key)) ? tmp->string_ : def
#define JSON_MEM_NUM(key, def) \
        (tmp = json_find_member(json, key)) ? tmp->number_ : def
    b->info.id = id;
    b->info.heading = strdup(JSON_MEM_STR("heading", "No Heading"));
    b->info.sub_heading = strdup(JSON_MEM_STR("sub_heading", "No Subheading"));
    b->info.author = strdup(JSON_MEM_STR("author", "guest")); 
    b->info.author_link = strdup(JSON_MEM_STR("author_link", "#"));
    b->info.timestamp = (time_t)JSON_MEM_NUM("timestamp", 1469227894);
    b->content = strdup(JSON_MEM_STR("content", "~_~"));

    json_delete(json);
    return 0;
}

int build_blog_cache(int id)
{
    char html_path[1024];
    snprintf(html_path, sizeof(html_path), "./data/blogs/%d.html", id);

    struct blog b;
    if (build_blog(id, &b) != 0)
        return HTTP_INTERNAL_ERROR;

    size_t sz = strlen(b.content) + 8196;
    char *buf = malloc(sz);
    if (!buf) {
        free_blog_buf(&b);
        return HTTP_INTERNAL_ERROR;
    }
    int len = snprintf(buf, sz, tmpl_blog, 
            b.info.heading, b.info.sub_heading, b.info.author_link, 
            b.info.author, ctime(&b.info.timestamp), b.content );
    free_blog_buf(&b);
    if (len >= sz) {
        len = sz-1;
        buf[len] = 0;
    }

    /* insert formatted html to cache. */
    content_t *new_str = content_new(buf, len, sz);
    if (!new_str) {
        DBG("content malloc failed");
        free(buf); 
        return HTTP_INTERNAL_ERROR;
    }
    // No need to free buf because it is inserted to cache.
    hash_add(g_svr.cache, strdup(html_path), new_str);
    return 0;
}

/* /blogs */
enum http_status blogs(void *data) {
    if (!data)
        return HTTP_INTERNAL_ERROR;
    struct client *c = data;
    struct http_request *req = &c->req;
    struct http_response *resp = &c->resp;

    content_t *str_head= NULL;
    content_t *str_foot= NULL;
    content_t *str;


    /* support both /blogs/1 and /blogs?1 */
    int id = 0;
    char html_path[1024];
    if (req->query) {
        id = atoi(req->query);
    } else {
        char *query = req->path+strlen("/blogs/");
        if (*query)
            id = atoi(query);
    }

    if (id <=0) {
        return HTTP_NOT_FOUND;
    }


    snprintf(html_path, sizeof(html_path), "./data/blogs/%d.html", id);
    str = get_file_content(html_path);
    if (!str) {
        /* build blog cache */
        build_blog_cache(id);
        str = get_file_content(html_path);
        if (!str)
            return HTTP_INTERNAL_ERROR;
    }
    str_head= get_file_content("./tmpl/blogs_header.html");
    if (!str_head)
        return HTTP_INTERNAL_ERROR;

    str_foot = get_file_content("./tmpl/blogs_footer.html");
    if (!str_foot)
        return HTTP_INTERNAL_ERROR;
    
    resp->mime_type = "text/html";
    resp->head_sbuf = strbuf_new_static(str_head->value, str_head->len);
    resp->sbuf = strbuf_new_static(str->value, str->len);
    resp->foot_sbuf = strbuf_new_static(str_foot->value, str_foot->len);
    //strbuf_set_static(resp->head_sbuf, str_head->value, str_head->len);
    //strbuf_set_static(resp->sbuf, str->value, str->len);
    //strbuf_set_static(resp->foot_sbuf, str_foot->value, str_foot->len);
    return HTTP_OK;
}

/* root handler */
enum http_status static_files(void *data) {
    if (!data)
        return HTTP_INTERNAL_ERROR;
    struct client *c = data;
    struct http_request *req = &c->req;
    struct http_response *resp = &c->resp;

    char *filepath = NULL;

    if (req->path[0] != '/')
        return HTTP_NOT_FOUND;
    
    if (req->path[1] == 0)
        filepath = "index.html";
    else
        filepath = req->path + 1;
    
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_svr.cfg.dir, filepath);
    resp->mime_type = file_mime_type(basename(filepath));

    content_t *str = get_file_content(path);
    if (!str)
        return HTTP_NOT_FOUND;
    

    char time_str[128];
    time_t t;
    struct tm tmp;
    if (req->mtime) {
        strptime(req->mtime, "%a, %d %b %Y %T %z", &tmp);
        t = mktime(&tmp);
        if (t >= str->mtime) {
            DBG("not modified: %s", path);
            return HTTP_NOT_MODIFIED;
        }
    }


    gmtime_r(&str->mtime, &tmp);
    if (strftime(time_str, sizeof(time_str), "%a, %d %b %Y %T %Z", &tmp) != 0) {
        resp->curr_header = 0;
        resp->headers[resp->curr_header].key = strdup("\r\nLast-Modified: ");
        resp->headers[resp->curr_header].value = strdup(time_str);
        resp->headers_sz++;
        
        resp->curr_header++;
        resp->headers[resp->curr_header].key = strdup("\r\nCache-Control: ");
        resp->headers[resp->curr_header].value = strdup("max-age=3600");
        resp->headers_sz++;
    }
    t = time(NULL);
    gmtime_r(&t, &tmp);
    if (strftime(time_str, sizeof(time_str), "%a, %d %b %Y %T %Z", &tmp) != 0) {
        resp->curr_header++;
        resp->headers[resp->curr_header].key = strdup("\r\nDate: ");
        resp->headers[resp->curr_header].value = strdup(time_str);
        resp->headers_sz++;
    }
    

    resp->sbuf = strbuf_new_static(str->value, str->len);
    //strbuf_set_static(resp->sbuf, str->value, str->len);

    
    return HTTP_OK;
}

/* scan resource dirs, generate etag and last modify info. */
int refresh_resources(void) {
    static time_t last_mtime = 0;

    struct stat st;
    stat(g_svr.cfg.dir, &st);
    if (st.st_mtime <= last_mtime) {
        return 0;
    }

    return 1;
}

/* scan the data dir and generate index page. */
int refresh_index_page(void) {
    int cmp_int(const void *a, const void *b) {
        const int *pa = a, *pb = b;
        return (*pa - *pb);
    }

    static time_t last_mtime = 0;
    int sz = 0, cnt = 0;
    int *ids = NULL, *tmp_ids = NULL;


    struct stat st;
    stat("./data/blogs/", &st);
    if (st.st_mtime <= last_mtime) {
        return 0;
    }

    hash_free(g_svr.cache);
    g_svr.cache = hash_str_new(free, content_free);

    DIR *dir = opendir("./data/blogs/");
    if (!dir)
        return -1;

    struct dirent *ptr;
    while ((ptr = readdir(dir)) != NULL) {
        if (ptr->d_name[0] == '.')
            continue;
        
        int id = atoi(ptr->d_name);
        if (id <= 0)
            continue;
        if (ids == NULL) {
            sz = 32;
            ids = calloc(32, sizeof(int));
            if (!ids) {
                WARN("malloc failed for refresh index page.");
                closedir(dir);
                return -1;
            }
        } 
       
        if (cnt >= sz) {
            tmp_ids = realloc(ids, sz * 2 * sizeof(int));
            if (!tmp_ids)
                break;
            sz *= 2;
            ids = tmp_ids;
        } 
        ids[cnt++] = id;
    }

    closedir(dir);

    /* only refresh when the max id changed. */
    if (cnt != 0) {
        qsort(ids, cnt, sizeof(int), cmp_int);
    }

    struct list_head *head = malloc(sizeof(*head));
    list_head_init(head);

    struct blog *b;
    int i;
    for (i = 0; i < cnt; i++) {
        b = calloc(1, sizeof(struct blog));
        if (!b)
            break;
        if (build_blog(ids[i], b) != 0) {
            free_blog_buf(b);
            free(b);
            continue;
        }
        list_add(head, &b->blogs);
    }
    free(ids);

    last_mtime = st.st_mtime;
    
    pthread_mutex_lock(&g_svr.mtx);
    struct list_head *tmp = g_svr.blogs;
    g_svr.blogs = head;
    pthread_mutex_unlock(&g_svr.mtx);
    free_blogs_list(tmp);


    /* regenerate index.html */
    struct blog *node, *next;
    int buf_sz = 1024*cnt + 8196, len = 0;
    
    char *buf = malloc(buf_sz);
    if (!buf)
        return -1;

    content_t *str;
    str = get_file_content("./tmpl/index_header.html");
    if (!str)
        return HTTP_INTERNAL_ERROR;

    int nwrite;
    nwrite = snprintf(buf, buf_sz, "%s", str->value);
    len += nwrite;
    
    list_for_each_safe(g_svr.blogs, node, next, blogs) {
        build_blog_cache(node->info.id);
        int nwrite = snprintf(buf + len, buf_sz - len, tmpl_blog_info,
                node->info.id, node->info.heading, node->info.sub_heading,
                node->info.author_link, node->info.author,
                ctime(&node->info.timestamp) );
        len += nwrite;
        if (buf_sz - len < 1024) {
            buf_sz += 4096;
            buf = realloc(buf, buf_sz);
        }
    }

    str = get_file_content("./tmpl/index_footer.html");
    if (!str)
        return HTTP_INTERNAL_ERROR;
    nwrite = snprintf(buf+len, buf_sz-len, "%s", str->value);
    len += nwrite;

    char path[1024];
    snprintf(path, sizeof(path), "%s/index.html", g_svr.cfg.dir);
    
    content_t *index_str = content_new(buf, len, buf_sz);
    hash_add(g_svr.cache, strdup(path), index_str);
    return last_mtime;
}

