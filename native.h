
enum {
	UV_E_INACTIVE,
	UV_E_WRITE,
	UV_E_READ,
	UV_E_CONNECT,
	UV_E_MAX,
	UV_E_MAX_ARG = 2,
};

enum {
	UV_M_KEEP = 0x01,
};

typedef union {
	int i;
	void *p;
	struct {
		void *base;
		int len;
	} buf;
} uvEventArg;

typedef struct {
	void *handle;
	int type;
	uvEventArg args[UV_E_MAX_ARG];
} uvEvent;

uint32_t uvGetId(void *handle);

int uvPollLoop(uvEvent *e);

void *uvSetTimer(int timeout, int repeat);
void uvStopTimer(void *handle);

void uvRead(void *handle, void *bytes);

void *uvConnect(void *handle, const char *host, const char *port);
void *uvListen6(void *handle, const char *ip, int port, int backlog);
void *uvTcpNew();

void uvHandleDump();
int uvHandleCount();

void uvWalkClearMark();
void uvMarkKeep(void *handle);
void uvWalkGc();

void *bytesNew(int len);
void bytesFree(void *b);
void bytesAppend(void *a, void *b);
uint32_t bytesId(void *b);
void *bytesBuf(void *b);
uint32_t bytesReadIntBE(void *b, int pos, int len);
void bytesWriteIntBE(void *b, int pos, int len, uint32_t v);
void bytesAppendUIntBE(void *_b, uint32_t v, int len);
const char *bytesHexdump16(void *_b, int pos);
int bytesLen(void *_b);

