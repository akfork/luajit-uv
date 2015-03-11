
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include <uv.h>

#include "strbuf.h"
#include "queue.h"
#include "native.h"

typedef struct {
	strbuf_t sb;
	int len;
	uint32_t id;
} bytes;

static uint32_t _bytesId;

void *bytesNew(int len) {
	bytes *b = calloc(sizeof(bytes), 1);
	b->len = len;
	b->id = _bytesId++;
	strbuf_init(&b->sb, len);
	return b;
}

void *bytesBuf(void *_b) {
	bytes *b = _b;
	return b->sb.buf;
}

uint32_t bytesReadIntBE(void *_b, int pos, int len) {
	bytes *b = _b;

	if (pos + len > b->len)
		return 0;

	uint32_t v = 0;
	uint8_t *p = b->sb.buf + pos;

	int i;
	for (i = 0; i < len; i++) {
		v <<= 8;
		v |= *p++;
	}

	return v;
}

void bytesWriteIntBE(void *_b, int pos, int len, uint32_t v) {
	bytes *b = _b;

	if (pos + len > b->len)
		return;

	uint8_t *p = b->sb.buf + pos;

	int i;
	for (i = len-1; i >= 0; i--) {
		*p++ = (v >> (i*8)) & 0xff;
	}
}

void bytesAppendUIntBE(void *_b, uint32_t v, int len) {
	bytes *b = _b;
	strbuf_ensure_empty_length(&b->sb, len);
	b->len += len;
	bytesWriteIntBE(b, b->len - len, len, v);
}

const char *bytesHexdump16(void *_b, int pos) {
	bytes *b = _b;
	static __thread char out[256];
	int len = 16;

	if (pos + len > b->len)
		len = b->len - pos;

	int i, j = 0;
	for (i = pos; i < b->len; i++) {
		sprintf(out + j, "%.2X ", *(uint8_t *)(b->sb.buf + i));
		j += 3;
	}

	return out;
}

int bytesLen(void *_b) {
	bytes *b = _b;
	return b->len;
}

uint32_t bytesId(void *_b) {
	bytes *b = _b;
	return b->id;
}

void bytesFree(void *_b) {
	bytes *b = _b;
	strbuf_free(&b->sb);
	free(b);
}

void bytesAppend(void *_a, void *_b) {
	bytes *a = _a;
	bytes *b = _b;
	strbuf_append_mem(&a->sb, b->sb.buf, b->len);
}

typedef struct {
	int marks;
	queue_t q;
	uvEvent e[UV_E_MAX];
	int emask;
	int refNr;
	uint32_t id;
	lua_State *L;
	bytes *readBytes;
	int readLen;
	int readPos;
	uv_connect_t *connReq;
	uv_getaddrinfo_t *resolveReq;
} uvHandleHdr;

typedef struct {
	queue_t pending;
} uvLoop;

static __thread uv_loop_t *curLoop;

static uvHandleHdr *uvGetHdr(void *handle) {
	return handle - sizeof(uvHandleHdr);
}

static void *uvGetHandle(uvHandleHdr *h) {
	return h + 1;
}

void *uvNewHandle(int size) {
	uvHandleHdr *h = calloc(sizeof(uvHandleHdr) + size, 1);
	void *handle = h + 1;
	static uint32_t id;

	queue_init(&h->q);
	h->refNr = 1;
	h->id = id++;

	return handle;
}

static void uvFreeHandle(void *handle) {
	uvHandleHdr *h = uvGetHdr(handle);
	if (h->connReq)
		free(h->connReq);
	if (h->resolveReq)
		free(h->resolveReq);
	free(h);
}

static void onHandleClosed(uv_handle_t *handle) {
	uvFreeHandle(handle);
}

void uvCloseHandle(void *handle) {
	//printf("closed h=%p\n", handle);
	uv_close(handle, onHandleClosed);
}

uint32_t uvGetId(void *handle) {
	return uvGetHdr(handle)->id;
}

static void uvMarkEventHappened(void *handle, int type, uvEventArg *args, int argsNr) {
	uvHandleHdr *h = uvGetHdr(handle);
	uvEvent *e = &h->e[type];

	int i;
	for (i = 0; i < argsNr; i++)
		e->args[i] = args[i];

	if (h->emask == 0) {
		uvHandleHdr *hLoop = uvGetHdr(curLoop);
		queue_insert_tail(&hLoop->q, &h->q);
	}

	h->emask |= 1<<type;
}

#define MAX_READLEN 2048

static void onReadAlloc(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
	uvHandleHdr *h = uvGetHdr(handle);
	bytes *b = h->readBytes;

	buf->base = b->sb.buf + h->readPos;
	buf->len = b->len - h->readPos;
}

static void markReadDone(uv_stream_t *handle) {
	uvHandleHdr *h = uvGetHdr(handle);

	uvEventArg args[] = {
		{.i = h->readPos},
	};
	uvMarkEventHappened(handle, UV_E_READ, args, 1);
	uv_read_stop(handle);
}

static void onReadDone(uv_stream_t *handle, ssize_t size, const uv_buf_t *buf) {
	uvHandleHdr *h = uvGetHdr(handle);

	if (size < 0) {
		markReadDone(handle);
	} else {
		h->readPos += size;
		if (h->readPos == h->readBytes->len)
			markReadDone(handle);
	}
}

void uvRead(void *handle, void *bytes) {
	uvHandleHdr *h = uvGetHdr(handle);

	h->readBytes = bytes;
	h->readPos = 0;

	uv_read_start(handle, onReadAlloc, onReadDone);
}

static void onWalkCount(uv_handle_t *handle, void *p) {
	int *n = (int *)p;
	(*n)++;
}

int uvHandleCount() {
	int n = 0;
	uv_walk(curLoop, onWalkCount, &n);
	return n;
}

static const char *handleTypeStr(uv_handle_type type) {
	switch (type) {
	case UV_TCP:
		return "tcp";
	case UV_TIMER:
		return "timer";
	default:
		return "?";
	}
}

static void onWalkDump(uv_handle_t *handle, void *p) {
	printf("h=%p type=%s active=%d\n", 
			handle, handleTypeStr(handle->type),
			uv_is_active(handle)
			);
}

void uvHandleDump() {
	uv_walk(curLoop, onWalkDump, NULL);
}

void *uvLoopNew() {
	return uvNewHandle(sizeof(uv_loop_t));
}

static void onWalkClearMark(uv_handle_t *handle, void *data) {
	uvHandleHdr *h = uvGetHdr(handle);
	h->marks = 0;
}

void uvWalkClearMark() {
	uv_walk(curLoop, onWalkClearMark, NULL);
}

void uvMarkKeep(void *handle) {
	uvHandleHdr *h = uvGetHdr(handle);
	h->marks |= UV_M_KEEP;
}

static void onWalkGc(uv_handle_t *handle, void *data) {
	uvHandleHdr *h = uvGetHdr(handle);
	if (!(h->marks & UV_M_KEEP) && !uv_is_closing(handle))
		uvCloseHandle(handle);
}

void uvWalkGc() {
	uv_walk(curLoop, onWalkGc, NULL);
}

static int pollFromEventCache(uvEvent *e) {
	void *loop = curLoop;
	uvHandleHdr *hLoop = uvGetHdr(loop);

	if (!queue_empty(&hLoop->q)) {
		uvHandleHdr *hHandle = queue_data(queue_head(&hLoop->q), uvHandleHdr, q);
		int i;

		for (i = 0; i < UV_E_MAX; i++) {
			if (hHandle->emask & (1<<i)) {
				hHandle->emask &= ~(1<<i);
				*e = hHandle->e[i];
				e->type = i;
				break;
			}
		}

		//printf("eq type=%d emask=%x\n", e->type, hHandle->emask);

		e->handle = uvGetHandle(hHandle);
		if (hHandle->emask == 0)
			queue_remove(&hHandle->q);

		return 1;
	}
	return 0;
}

int uvPollLoop(uvEvent *e) {
	if (pollFromEventCache(e))
		return 1;
	int r = uv_run(curLoop, UV_RUN_ONCE);
	if (pollFromEventCache(e))
		return 1;
	return r == 0 ? -1 : 0;
}

static void onTimer(uv_timer_t *handle) {
	uvMarkEventHappened(handle, UV_E_READ, NULL, 0);
}

void *uvSetTimer(int timeout, int repeat) {
	uv_timer_t *handle = uvNewHandle(sizeof(uv_timer_t));
	uv_timer_init(curLoop, handle);
	uv_timer_start(handle, onTimer, timeout, repeat);
	return handle;
}

void uvStopTimer(void *handle) {
	uv_timer_stop(handle);
}

void *uvTcpNew() {
	void *handle = uvNewHandle(sizeof(uv_tcp_t));
	uv_tcp_init(curLoop, handle);
	return handle;
}

static void onConnected(uv_connect_t *req, int stat) {
	void *handle = req->handle;
	uvEventArg args[] = {
		{},
		{.i = stat},
	};
	uvMarkEventHappened(handle, UV_E_CONNECT, args, 2);
	//printf("connected h=%p s=%d\n", handle, stat);
}

static void onGotAddrInfo(uv_getaddrinfo_t *req, int stat, struct addrinfo *resp) {
	struct addrinfo *rp;
	void *handle = req->data;

	for (rp = resp; rp; rp = rp->ai_next) {
		if (rp->ai_family == AF_INET)
			break;
	}

	if (rp == NULL) {
		uvEventArg args[] = {
			{},
			{.i = -1},
		};
		uvMarkEventHappened(handle, UV_E_CONNECT, args, 2);
	} else {
		uvHandleHdr *h = uvGetHdr(handle);
		h->connReq = calloc(sizeof(uv_connect_t), 1);
		uv_tcp_connect(h->connReq, handle, rp->ai_addr, onConnected);
	}

	uv_freeaddrinfo(resp);
}

void *uvConnect(void *handle, const char *host, const char *port) {
	//struct sockaddr_in addr;
	//uv_ip4_addr(host, port, &addr);
	uvHandleHdr *h = uvGetHdr(handle);
	h->resolveReq = calloc(sizeof(uv_getaddrinfo_t), 1);
	h->resolveReq->data = handle;
	uv_getaddrinfo(curLoop, h->resolveReq, onGotAddrInfo, host, port, NULL);
}

static void onAccept(uv_stream_t *handle, int status) {
	uvHandleHdr *h = uvGetHdr(handle);
	void *client = NULL;

	if (status == 0) {
		client = uvTcpNew(handle->loop);
		uv_accept(handle, client);
	}

	uvEventArg args[2] = {
		{.p = client},
		{.i = status},
	};
	//printf("accept h=%p\n", handle);
	uvMarkEventHappened(handle, UV_E_CONNECT, args, 2);
}

void *uvListen6(void *handle, const char *ip, int port, int backlog) {
	struct sockaddr_in6 addr;
	uv_ip6_addr(ip, port, &addr);
	uv_tcp_bind(handle, (const struct sockaddr *)&addr, 0);
	uv_listen(handle, backlog, onAccept);
	return NULL;
}

static void printTraceback() {
	fprintf(stderr, "native traceback:\n");
	void *array[128];
	size_t size;
	size = backtrace(array, 128);
	backtrace_symbols_fd(array, size, 2);
	fprintf(stderr, "\n");
}

static void fault(int sig) {
	signal(SIGILL, SIG_IGN);
	signal(SIGBUS, SIG_IGN);
	signal(SIGSEGV, SIG_IGN);
	signal(SIGABRT, SIG_IGN);

	printTraceback();

	lua_State *L = uvGetHdr(curLoop)->L;
	lua_getglobal(L, "printTraceback");
	lua_call(L, 0, 0);
}

int main() {
	signal(SIGILL, fault);
	signal(SIGBUS, fault);
	signal(SIGSEGV, fault);
	signal(SIGABRT, fault);

	uv_loop_t *loop = uvNewHandle(sizeof(uv_loop_t));
	uv_loop_init(loop);
	curLoop = loop;

	lua_State *L = luaL_newstate();
	uvGetHdr(loop)->L = L;
	luaL_openlibs(L);
	luaL_dostring(L, "return require('main')");
	lua_pushlightuserdata(L, loop);
	lua_call(L, 1, 0);

	lua_close(L);
	uv_loop_close(loop);

	return 0;
}

