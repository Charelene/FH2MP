/*
 * xhr2.inc - in-process IXMLHTTPRequest2 over WinHTTP (included by azimuth_tls_proxy.c).
 *
 * Turn 10's LSP client (Xls::HttpRequest, FUN_141b7f530) does
 *   CoCreateInstance(CLSID_FreeThreadedXMLHTTP60, NULL, 0x15, IID_IXMLHTTPRequest2, &p)
 *   p->Open(method, url, callback, ...); p->SetRequestHeader(...); p->Send(body, cb);
 * and waits for HttpCallback to signal (OnDataAvailable stores the response stream,
 * OnResponseReceived/OnError set an event). Azimuth's COM object rejects every request as
 * "offline" (HttpProtocol2::OnRequestComplete finished with error 0x80500010), so no packet ever
 * leaves the machine. We repoint the game's CoCreateInstance import so that this CLSID yields the
 * object below, which sends the request over WinHTTP to our own server (fh2mp-lsp.txt =
 * "host:port" for plain HTTP, default 127.0.0.1:5080, or "https://host[:port]" for TLS, default port 443).
 * The original host is passed in X-FH2MP-Host, the launcher's session token in X-FH2MP-Token.
 * Disable with env FH2MP_NO_XHR.
 */

#include <winhttp.h>

#define XHR_MAX_HDR 32

typedef struct {
    const void *const *vtbl;
} com_obj;

typedef HRESULT(WINAPI *co_create_fn)(const GUID *, void *, DWORD, const GUID *, void **);
typedef LPVOID(WINAPI *co_alloc_fn)(SIZE_T);

typedef HINTERNET(WINAPI *wh_open_fn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET(WINAPI *wh_connect_fn)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI *wh_openreq_fn)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL(WINAPI *wh_addhdr_fn)(HINTERNET, LPCWSTR, DWORD, DWORD);
typedef BOOL(WINAPI *wh_send_fn)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI *wh_recv_fn)(HINTERNET, LPVOID);
typedef BOOL(WINAPI *wh_query_fn)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *wh_read_fn)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI *wh_close_fn)(HINTERNET);
typedef BOOL(WINAPI *wh_timeouts_fn)(HINTERNET, int, int, int, int);

static struct {
    HMODULE mod;
    wh_open_fn open;
    wh_connect_fn connect;
    wh_openreq_fn openreq;
    wh_addhdr_fn addhdr;
    wh_send_fn send;
    wh_recv_fn recv;
    wh_query_fn query;
    wh_read_fn read;
    wh_close_fn close;
    wh_timeouts_fn timeouts;
    co_alloc_fn cotaskalloc;
    int ready;
} g_wh;

static co_create_fn g_real_cocreate;
static wchar_t g_lsp_host[128] = L"127.0.0.1";
static unsigned g_lsp_port = 5080;
static int g_lsp_secure;
static int g_lsp_cfg_loaded;
static volatile LONG g_xhr_seq;

static const GUID XHR_CLSID_FREETHREADED = {0x88d96a09, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}};
static const GUID XHR_IID_IUNKNOWN = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID XHR_IID_XHR2 = {0xe5d37dc0, 0x552a, 0x4d52, {0x9c, 0xc0, 0xa1, 0x4d, 0x54, 0x6f, 0xbd, 0x04}};
static const GUID XHR_IID_SEQSTREAM = {0x0c733a30, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};

static int xhr_guid_eq(const GUID *a, const GUID *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    int i;

    for (i = 0; i < (int)sizeof(GUID); i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

static void *xhr_alloc(SIZE_T n)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1);
}

static void xhr_free(void *p)
{
    if (p) {
        HeapFree(GetProcessHeap(), 0, p);
    }
}

static wchar_t *xhr_wdup(const wchar_t *s)
{
    int n;
    wchar_t *r;

    if (!s) {
        return NULL;
    }
    n = lstrlenW(s);
    r = (wchar_t *)xhr_alloc((SIZE_T)(n + 1) * sizeof(wchar_t));
    if (r) {
        mem_copy(r, s, (SIZE_T)(n + 1) * sizeof(wchar_t));
    }
    return r;
}

static int xhr_weq_nocase(const wchar_t *a, const wchar_t *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

static void xhr_load_cfg(void)
{
    wchar_t path[MAX_PATH];
    char raw[160];
    DWORD got = 0;
    HANDLE f;
    int i, colon = -1, n;

    if (g_lsp_cfg_loaded) {
        return;
    }
    g_lsp_cfg_loaded = 1;
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-lsp.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!ReadFile(f, raw, sizeof raw - 1, &got, NULL)) {
        got = 0;
    }
    CloseHandle(f);
    raw[got] = '\0';
    {
        const char *r = raw;
        if (r[0] == 'h' && r[1] == 't' && r[2] == 't' && r[3] == 'p') {
            if (r[4] == 's' && r[5] == ':' && r[6] == '/' && r[7] == '/') {
                g_lsp_secure = 1;
                g_lsp_port = 443;
                r += 8;
            } else if (r[4] == ':' && r[5] == '/' && r[6] == '/') {
                g_lsp_port = 80;
                r += 7;
            }
        }
        n = 0;
        while (r[n] && r[n] != '\r' && r[n] != '\n' && r[n] != ' ' && r[n] != '/') {
            if (r[n] == ':') {
                colon = n;
            }
            n++;
        }
        if (n == 0) {
            return;
        }
        for (i = 0; i < (colon >= 0 ? colon : n) && i < 120; i++) {
            g_lsp_host[i] = (wchar_t)(unsigned char)r[i];
        }
        g_lsp_host[i] = 0;
        if (colon >= 0) {
            unsigned v = 0;
            for (i = colon + 1; i < n; i++) {
                v = v * 10 + (unsigned)(r[i] - '0');
            }
            if (v) {
                g_lsp_port = v;
            }
        }
    }
}

static int xhr_wh_init(void)
{
    HMODULE ole;

    if (g_wh.ready) {
        return g_wh.ready > 0;
    }
    g_wh.mod = LoadLibraryW(L"winhttp.dll");
    ole = LoadLibraryW(L"ole32.dll");
    if (g_wh.mod) {
        g_wh.open = (wh_open_fn)GetProcAddress(g_wh.mod, "WinHttpOpen");
        g_wh.connect = (wh_connect_fn)GetProcAddress(g_wh.mod, "WinHttpConnect");
        g_wh.openreq = (wh_openreq_fn)GetProcAddress(g_wh.mod, "WinHttpOpenRequest");
        g_wh.addhdr = (wh_addhdr_fn)GetProcAddress(g_wh.mod, "WinHttpAddRequestHeaders");
        g_wh.send = (wh_send_fn)GetProcAddress(g_wh.mod, "WinHttpSendRequest");
        g_wh.recv = (wh_recv_fn)GetProcAddress(g_wh.mod, "WinHttpReceiveResponse");
        g_wh.query = (wh_query_fn)GetProcAddress(g_wh.mod, "WinHttpQueryHeaders");
        g_wh.read = (wh_read_fn)GetProcAddress(g_wh.mod, "WinHttpReadData");
        g_wh.close = (wh_close_fn)GetProcAddress(g_wh.mod, "WinHttpCloseHandle");
        g_wh.timeouts = (wh_timeouts_fn)GetProcAddress(g_wh.mod, "WinHttpSetTimeouts");
    }
    if (ole) {
        g_wh.cotaskalloc = (co_alloc_fn)GetProcAddress(ole, "CoTaskMemAlloc");
    }
    g_wh.ready = (g_wh.open && g_wh.connect && g_wh.openreq && g_wh.addhdr && g_wh.send && g_wh.recv &&
                  g_wh.query && g_wh.read && g_wh.close && g_wh.cotaskalloc)
                     ? 1
                     : -1;
    if (g_wh.ready < 0) {
        log_begin();
        log_raw("xhr: WinHTTP/ole32 entry points missing, requests will fail\n");
    }
    return g_wh.ready > 0;
}

/* ------------------------------------------------------------------ response stream */

typedef struct {
    const void *const *vtbl;
    volatile LONG ref;
    unsigned char *data;
    DWORD len;
    DWORD pos;
} xhr_stream;

/* The stream most recently read from on each thread. Embedded XSAPI copies a response into a Windows DataWriter and
 * reads it back as text; in this environment that round trip loses the body (docs/matchmaking.md), so
 * patch_xsapi_body() recovers the bytes from here. Entries are dropped when their stream is freed. */
#define XHR_CAP_MAX 32
static struct {
    volatile LONG tid;
    xhr_stream *s;
} g_cap[XHR_CAP_MAX];

static void cap_set(DWORD tid, xhr_stream *s)
{
    int i;
    for (i = 0; i < XHR_CAP_MAX; i++) {
        if ((DWORD)g_cap[i].tid == tid) {
            g_cap[i].s = s;
            return;
        }
    }
    for (i = 0; i < XHR_CAP_MAX; i++) {
        if (InterlockedCompareExchange(&g_cap[i].tid, (LONG)tid, 0) == 0) {
            g_cap[i].s = s;
            return;
        }
    }
}

static void cap_forget(xhr_stream *s)
{
    int i;
    for (i = 0; i < XHR_CAP_MAX; i++) {
        if (g_cap[i].s == s) {
            g_cap[i].s = NULL;
        }
    }
}

static HRESULT STDMETHODCALLTYPE st_qi(void *self, const GUID *iid, void **out)
{
    if (xhr_guid_eq(iid, &XHR_IID_IUNKNOWN) || xhr_guid_eq(iid, &XHR_IID_SEQSTREAM)) {
        InterlockedIncrement(&((xhr_stream *)self)->ref);
        *out = self;
        return S_OK;
    }
    log_begin();
    log_raw("xhr: stream QueryInterface refused\n");
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE st_addref(void *self)
{
    return (ULONG)InterlockedIncrement(&((xhr_stream *)self)->ref);
}

static ULONG STDMETHODCALLTYPE st_release(void *self)
{
    xhr_stream *s = (xhr_stream *)self;
    LONG r = InterlockedDecrement(&s->ref);

    if (r == 0) {
        cap_forget(s);
        xhr_free(s->data);
        xhr_free(s);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE st_read(void *self, void *buf, ULONG cb, ULONG *got)
{
    xhr_stream *s = (xhr_stream *)self;
    ULONG n = s->len - s->pos;

    if (s->pos == 0) {
        cap_set(GetCurrentThreadId(), s);
    }
    if (n > cb) {
        n = cb;
    }
    if (n) {
        mem_copy(buf, s->data + s->pos, n);
    }
    s->pos += n;
    if (got) {
        *got = n;
    }
#ifdef FH2MP_DEBUG_BUILD
    {
        static volatile LONG reads;
        if (InterlockedIncrement(&reads) <= 400) { /* who reads the response, how much, when (debugging aid) */
            log_begin();
            log_raw("xhr: stream read req=");
            log_dec(cb);
            log_raw(" got=");
            log_dec(n);
            log_raw(" pos=");
            log_dec(s->pos);
            log_raw("/");
            log_dec(s->len);
            log_raw(" tid=");
            log_dec(GetCurrentThreadId());
            log_raw("\n");
        }
    }
#endif
    return n == cb ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE st_write(void *self, const void *buf, ULONG cb, ULONG *written)
{
    (void)self; (void)buf;
    if (written) {
        *written = cb;
    }
    return S_OK;
}

static const void *const g_stream_vtbl[] = {(void *)st_qi, (void *)st_addref, (void *)st_release,
                                            (void *)st_read, (void *)st_write};

static xhr_stream *xhr_stream_new(unsigned char *data, DWORD len)
{
    xhr_stream *s = (xhr_stream *)xhr_alloc(sizeof *s);

    if (!s) {
        return NULL;
    }
    s->vtbl = g_stream_vtbl;
    s->ref = 1;
    s->data = data;
    s->len = len;
    return s;
}

/* ------------------------------------------------------------------ the request object */

typedef struct {
    const void *const *vtbl;
    volatile LONG ref;
    wchar_t method[16];
    wchar_t *url;
    com_obj *cb; /* IXMLHTTPRequest2Callback */
    wchar_t *hname[XHR_MAX_HDR];
    wchar_t *hval[XHR_MAX_HDR];
    int nhdr;
    unsigned char *body;
    DWORD nbody;
    DWORD seq;
    volatile LONG aborted;
    DWORD status;
    wchar_t *raw_headers; /* CRLF separated, from the server */
} xhr_obj;

typedef HRESULT(STDMETHODCALLTYPE *cb_redirect_fn)(void *, void *, const wchar_t *);
typedef HRESULT(STDMETHODCALLTYPE *cb_headers_fn)(void *, void *, DWORD, const wchar_t *);
typedef HRESULT(STDMETHODCALLTYPE *cb_stream_fn)(void *, void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *cb_error_fn)(void *, void *, HRESULT);
typedef ULONG(STDMETHODCALLTYPE *unk_release_fn)(void *);
typedef ULONG(STDMETHODCALLTYPE *unk_addref_fn)(void *);
typedef HRESULT(STDMETHODCALLTYPE *seq_read_fn)(void *, void *, ULONG, ULONG *);

static HRESULT STDMETHODCALLTYPE xhr_qi(void *self, const GUID *iid, void **out)
{
    if (xhr_guid_eq(iid, &XHR_IID_IUNKNOWN) || xhr_guid_eq(iid, &XHR_IID_XHR2)) {
        InterlockedIncrement(&((xhr_obj *)self)->ref);
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE xhr_addref(void *self)
{
    return (ULONG)InterlockedIncrement(&((xhr_obj *)self)->ref);
}

static ULONG STDMETHODCALLTYPE xhr_release(void *self)
{
    xhr_obj *x = (xhr_obj *)self;
    LONG r = InterlockedDecrement(&x->ref);
    int i;

    if (r == 0) {
        if (x->cb) {
            ((unk_release_fn)x->cb->vtbl[2])(x->cb);
        }
        for (i = 0; i < x->nhdr; i++) {
            xhr_free(x->hname[i]);
            xhr_free(x->hval[i]);
        }
        xhr_free(x->url);
        xhr_free(x->body);
        xhr_free(x->raw_headers);
        xhr_free(x);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE xhr_open(void *self, const wchar_t *method, const wchar_t *url, void *cb,
                                          const wchar_t *user, const wchar_t *pass, const wchar_t *puser,
                                          const wchar_t *ppass)
{
    xhr_obj *x = (xhr_obj *)self;
    int n = method ? lstrlenW(method) : 0;

    (void)user; (void)pass; (void)puser; (void)ppass;
    if (n > 15) {
        n = 15;
    }
    if (n) {
        mem_copy(x->method, method, (SIZE_T)n * sizeof(wchar_t));
    }
    x->method[n] = 0;
    x->url = xhr_wdup(url);
    x->cb = (com_obj *)cb;
    if (cb) {
        ((unk_addref_fn)x->cb->vtbl[1])(x->cb);
    }
    x->seq = (DWORD)InterlockedIncrement(&g_xhr_seq);
    log_begin();
    log_raw("xhr#");
    log_dec(x->seq);
    log_raw(" Open ");
    log_wide(x->method);
    log_raw(" ");
    log_wide(url ? url : L"(null)");
    log_raw("\n");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE xhr_set_header(void *self, const wchar_t *name, const wchar_t *value)
{
    xhr_obj *x = (xhr_obj *)self;

    if (!name || x->nhdr >= XHR_MAX_HDR) {
        return E_INVALIDARG;
    }
    x->hname[x->nhdr] = xhr_wdup(name);
    x->hval[x->nhdr] = xhr_wdup(value ? value : L"");
    x->nhdr++;
    return S_OK;
}

typedef struct {
    xhr_obj *x;
} xhr_job;

static void xhr_signal_error(xhr_obj *x, HRESULT hr)
{
    log_begin();
    log_raw("xhr#");
    log_dec(x->seq);
    log_raw(" error hr=0x");
    log_hex((unsigned long long)(unsigned)hr, 8);
    log_raw("\n");
    if (x->cb) {
        ((cb_error_fn)x->cb->vtbl[7])(x->cb, x, hr);
    }
}

static void xhr_dump_request(xhr_obj *x, const wchar_t *path)
{
    wchar_t name[MAX_PATH];
    wchar_t num[16];
    HANDLE f;
    DWORD w;
    int i, k = 0;
    DWORD v = x->seq;
    char line[512];
    WCHAR gate[4];

    /* Research aid (it writes one file per request, with the game's headers and bodies): debug builds only, and then env FH2MP_XHR_DUMP=1. */
#ifdef FH2MP_DEBUG_BUILD
    if (!GetEnvironmentVariableW(L"FH2MP_XHR_DUMP", gate, 4)) {
        return;
    }
#else
    (void)gate;
    return;
#endif
    /* fh2mp-lsp-<seq>.req : first the headers as text, then a blank line, then the raw body */
    lstrcpyW(name, g_dir);
    lstrcatW(name, L"\\fh2mp-lsp-");
    {
        wchar_t tmp[16];
        int t = 0;
        if (v == 0) {
            tmp[t++] = L'0';
        }
        while (v) {
            tmp[t++] = (wchar_t)(L'0' + (v % 10));
            v /= 10;
        }
        while (t) {
            num[k++] = tmp[--t];
        }
        num[k] = 0;
    }
    lstrcatW(name, num);
    lstrcatW(name, L".req");
    f = CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    for (i = -1; i < x->nhdr; i++) {
        int n;
        if (i < 0) {
            n = WideCharToMultiByte(CP_UTF8, 0, x->method, -1, line, sizeof line - 8, NULL, NULL);
            if (n > 0) {
                n--;
            }
            line[n++] = ' ';
            {
                int m = WideCharToMultiByte(CP_UTF8, 0, path, -1, line + n, (int)sizeof line - n - 4, NULL, NULL);
                n += m > 0 ? m - 1 : 0;
            }
        } else {
            n = WideCharToMultiByte(CP_UTF8, 0, x->hname[i], -1, line, 200, NULL, NULL);
            n = n > 0 ? n - 1 : 0;
            line[n++] = ':';
            line[n++] = ' ';
            {
                int m = WideCharToMultiByte(CP_UTF8, 0, x->hval[i], -1, line + n, (int)sizeof line - n - 4, NULL, NULL);
                n += m > 0 ? m - 1 : 0;
            }
        }
        line[n++] = '\r';
        line[n++] = '\n';
        WriteFile(f, line, (DWORD)n, &w, NULL);
    }
    WriteFile(f, "\r\n", 2, &w, NULL);
    if (x->nbody) {
        WriteFile(f, x->body, x->nbody, &w, NULL);
    }
    CloseHandle(f);
}

static DWORD WINAPI xhr_thread(LPVOID param)
{
    xhr_obj *x = (xhr_obj *)param;
    const wchar_t *u = x->url ? x->url : L"";
    const wchar_t *path = L"/";
    wchar_t orig_host[128];
    HINTERNET ses = NULL, con = NULL, req = NULL;
    unsigned char *data = NULL;
    DWORD ndata = 0, cap = 0;
    DWORD status = 0, sz;
    wchar_t status_text[64];
    wchar_t hdrline[600];
    int i, k;
    HRESULT hr = S_OK;
    const char *stage = "winhttp load";

    /* split "scheme://host[:port]/path" */
    orig_host[0] = 0;
    for (i = 0; u[i] && !(u[i] == L':' && u[i + 1] == L'/' && u[i + 2] == L'/'); i++) {
    }
    if (u[i]) {
        const wchar_t *h = u + i + 3;
        k = 0;
        while (h[k] && h[k] != L'/' && k < 126) {
            orig_host[k] = h[k];
            k++;
        }
        orig_host[k] = 0;
        path = h[k] ? h + k : L"/";
    }
    if (!xhr_wh_init()) {
        hr = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        goto fail;
    }
    xhr_load_cfg();
    log_begin();
    log_raw("xhr#");
    log_dec(x->seq);
    log_raw(" -> ");
    log_wide(g_lsp_host);
    log_raw(":");
    log_dec(g_lsp_port);
    log_raw(" (orig host ");
    log_wide(orig_host);
    log_raw(") body=");
    log_dec(x->nbody);
    log_raw(" hdrs=");
    log_dec((unsigned long)x->nhdr);
    log_raw("\n");
    xhr_dump_request(x, path);

    stage = "open";
    ses = g_wh.open(L"fh2mp", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!ses) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }
    if (g_wh.timeouts) {
        g_wh.timeouts(ses, 3000, 3000, 10000, 30000);
    }
    stage = "connect";
    con = g_wh.connect(ses, g_lsp_host, (INTERNET_PORT)g_lsp_port, 0);
    if (!con) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }
    stage = "open request";
    req = g_wh.openreq(con, x->method, path, NULL, NULL, NULL, g_lsp_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }
    for (i = 0; i < x->nhdr; i++) {
        if (lstrlenW(x->hname[i]) > 100 || lstrlenW(x->hval[i]) > 480) {
            continue;
        }
        lstrcpyW(hdrline, x->hname[i]);
        lstrcatW(hdrline, L": ");
        lstrcatW(hdrline, x->hval[i]);
        g_wh.addhdr(req, hdrline, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    lstrcpyW(hdrline, L"X-FH2MP-Host: ");
    lstrcatW(hdrline, orig_host);
    g_wh.addhdr(req, hdrline, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (g_token[0]) { /* the session the launcher signed in; the server takes the player's identity from it, not from a header the game wrote */
        int k;
        lstrcpyW(hdrline, L"X-FH2MP-Token: ");
        k = lstrlenW(hdrline);
        for (i = 0; g_token[i] && i < 90; i++) {
            hdrline[k + i] = (WCHAR)(unsigned char)g_token[i];
        }
        hdrline[k + i] = 0;
        g_wh.addhdr(req, hdrline, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    g_wh.addhdr(req, L"X-FH2MP-Shim: " FH2MP_WSTR(FH2MP_SHIM_VERSION), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    stage = "send/receive";
    if (!g_wh.send(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, x->body, x->nbody, x->nbody, 0) || !g_wh.recv(req, NULL)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }
    sz = sizeof status;
    g_wh.query(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
    sz = sizeof status_text;
    status_text[0] = 0;
    g_wh.query(req, WINHTTP_QUERY_STATUS_TEXT, NULL, status_text, &sz, NULL);
    sz = 0;
    g_wh.query(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, NULL, NULL, &sz, NULL);
    if (sz) {
        x->raw_headers = (wchar_t *)xhr_alloc(sz + 4);
        if (x->raw_headers && !g_wh.query(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, NULL, x->raw_headers, &sz, NULL)) {
            x->raw_headers[0] = 0;
        }
    }
    x->status = status;
    stage = "read body";
    for (;;) {
        DWORD got = 0;
        if (cap - ndata < 4096) {
            unsigned char *nd = (unsigned char *)xhr_alloc((SIZE_T)cap * 2 + 65536);
            if (!nd) {
                hr = E_OUTOFMEMORY;
                goto fail;
            }
            if (ndata) {
                mem_copy(nd, data, ndata);
            }
            xhr_free(data);
            data = nd;
            cap = cap * 2 + 65536;
        }
        if (!g_wh.read(req, data + ndata, cap - ndata, &got)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto fail;
        }
        if (!got) {
            break;
        }
        ndata += got;
    }
    log_begin();
    log_raw("xhr#");
    log_dec(x->seq);
    log_raw(" status=");
    log_dec(status);
    log_raw(" response bytes=");
    log_dec(ndata);
    log_raw("\n");
    if (InterlockedCompareExchange(&x->aborted, 0, 0)) {
        goto done;
    }
    if (x->cb) {
        xhr_stream *s = xhr_stream_new(data, ndata);
        data = NULL;
        if (!s) {
            hr = E_OUTOFMEMORY;
            goto fail;
        }
        ((cb_headers_fn)x->cb->vtbl[4])(x->cb, x, status, status_text);
        ((cb_stream_fn)x->cb->vtbl[5])(x->cb, x, s);
        ((cb_stream_fn)x->cb->vtbl[6])(x->cb, x, s);
        st_release(s);
    }
    goto done;

fail:
    {
        /* release builds keep this line ("fail"): without it a player whose game never reaches the server has nothing to send us */
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 20) {
            log_begin();
            log_raw("xhr#");
            log_dec(x->seq);
            log_raw(" failed at ");
            log_raw(stage);
            log_raw(": hr=0x");
            log_hex((unsigned long)hr, 8);
            log_raw(" server ");
            log_wide(g_lsp_host);
            log_raw(":");
            log_dec(g_lsp_port);
            log_raw(" (orig host ");
            log_wide(orig_host);
            log_raw(")\n");
        }
    }
    if (!InterlockedCompareExchange(&x->aborted, 0, 0)) {
        xhr_signal_error(x, hr);
    }
done:
    xhr_free(data);
    if (req) {
        g_wh.close(req);
    }
    if (con) {
        g_wh.close(con);
    }
    if (ses) {
        g_wh.close(ses);
    }
    xhr_release(x);
    return 0;
}

static HRESULT STDMETHODCALLTYPE xhr_send(void *self, void *body, unsigned long long cb)
{
    xhr_obj *x = (xhr_obj *)self;
    HANDLE t;

    if (body && cb) {
        DWORD total = 0;
        x->body = (unsigned char *)xhr_alloc((SIZE_T)cb);
        if (!x->body) {
            return E_OUTOFMEMORY;
        }
        while (total < cb) {
            ULONG got = 0;
            HRESULT hr = ((seq_read_fn)((com_obj *)body)->vtbl[3])(body, x->body + total, (ULONG)(cb - total), &got);
            if (FAILED(hr) || got == 0) {
                break;
            }
            total += got;
        }
        x->nbody = total;
    }
    InterlockedIncrement(&x->ref); /* the worker owns a reference */
    t = CreateThread(NULL, 0, xhr_thread, x, 0, NULL);
    if (!t) {
        InterlockedDecrement(&x->ref);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(t);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE xhr_abort(void *self)
{
    InterlockedExchange(&((xhr_obj *)self)->aborted, 1);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE xhr_set_cookie(void *self, const void *cookie, DWORD *state)
{
    (void)self; (void)cookie;
    if (state) {
        *state = 0;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE xhr_set_custom_stream(void *self, void *stream)
{
    (void)self; (void)stream;
    log_begin();
    log_raw("xhr: SetCustomResponseStream (ignored)\n");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE xhr_set_property(void *self, int prop, unsigned long long value)
{
    (void)self; (void)prop; (void)value;
    return S_OK;
}

static wchar_t *xhr_co_wdup(const wchar_t *s, int n)
{
    wchar_t *r;

    if (!g_wh.cotaskalloc) {
        return NULL;
    }
    r = (wchar_t *)g_wh.cotaskalloc((SIZE_T)(n + 1) * sizeof(wchar_t));
    if (r) {
        if (n) {
            mem_copy(r, s, (SIZE_T)n * sizeof(wchar_t));
        }
        r[n] = 0;
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE xhr_get_all_headers(void *self, wchar_t **out)
{
    xhr_obj *x = (xhr_obj *)self;
    const wchar_t *h = x->raw_headers ? x->raw_headers : L"";

    xhr_wh_init();
    *out = xhr_co_wdup(h, lstrlenW(h));
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE xhr_get_cookie(void *self, const wchar_t *url, const wchar_t *name, DWORD flags,
                                                 DWORD *count, void **cookies)
{
    (void)self; (void)url; (void)name; (void)flags;
    if (count) {
        *count = 0;
    }
    if (cookies) {
        *cookies = NULL;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE xhr_get_response_header(void *self, const wchar_t *name, wchar_t **out)
{
    xhr_obj *x = (xhr_obj *)self;
    const wchar_t *p = x->raw_headers;
    int nl = name ? lstrlenW(name) : 0;

    *out = NULL;
    xhr_wh_init();
    while (p && *p && nl) {
        const wchar_t *e = p;
        while (*e && *e != L'\r' && *e != L'\n') {
            e++;
        }
        if ((int)(e - p) > nl && p[nl] == L':') {
            wchar_t tmp[80];
            if (nl < 79) {
                const wchar_t *v = p + nl + 1;
                mem_copy(tmp, p, (SIZE_T)nl * sizeof(wchar_t));
                tmp[nl] = 0;
                if (xhr_weq_nocase(tmp, name)) {
                    while (v < e && (*v == L' ' || *v == L'\t')) {
                        v++;
                    }
                    *out = xhr_co_wdup(v, (int)(e - v));
                    return *out ? S_OK : E_OUTOFMEMORY;
                }
            }
        }
        while (*e == L'\r' || *e == L'\n') {
            e++;
        }
        p = e;
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static const void *const g_xhr_vtbl[] = {
    (void *)xhr_qi,          (void *)xhr_addref,      (void *)xhr_release,
    (void *)xhr_open,        (void *)xhr_send,        (void *)xhr_abort,
    (void *)xhr_set_cookie,  (void *)xhr_set_custom_stream, (void *)xhr_set_property,
    (void *)xhr_set_header,  (void *)xhr_get_all_headers,   (void *)xhr_get_cookie,
    (void *)xhr_get_response_header,
};

/* ------------------------------------------------------------------ CoCreateInstance hook */

#define FH2_COCREATE_IAT_RVA 0x1dd0ea0u
static unsigned char *g_cocreate_img;

static HRESULT WINAPI hooked_cocreate(const GUID *clsid, void *outer, DWORD ctx, const GUID *iid, void **out)
{
    if (clsid && xhr_guid_eq(clsid, &XHR_CLSID_FREETHREADED) && !outer) {
        xhr_obj *x = (xhr_obj *)xhr_alloc(sizeof *x);
        HRESULT hr;
        static volatile LONG first;

        if (InterlockedExchange(&first, 1) == 0) {
            /* compared with the "import hooked" line: a game that asked before the hook got Azimuth's offline stub instead */
            unsigned char *ra = (unsigned char *)__builtin_return_address(0);
            log_begin();
            log_raw("xhr: first request object through the hook, caller ");
            if (g_cocreate_img && ra >= g_cocreate_img && ra < g_cocreate_img + 0x4000000u) {
                log_raw("game+0x");
                log_hex((unsigned long long)(ra - g_cocreate_img), 8);
            } else {
                log_raw("0x");
                log_hex((unsigned long long)ra, 16);
                log_raw(" (XSAPI or other)");
            }
            log_raw("\n");
        }
        if (!x) {
            return E_OUTOFMEMORY;
        }
        x->vtbl = g_xhr_vtbl;
        x->ref = 1;
        hr = xhr_qi(x, iid, out);
        xhr_release(x);
        return hr;
    }
    return g_real_cocreate ? g_real_cocreate(clsid, outer, ctx, iid, out) : E_FAIL;
}

/* (Re)points the game's CoCreateInstance import at hooked_cocreate. Called from the monitor loop
 * because Azimuth binds the game's imports after the helper loads. */
static void patch_game_cocreate(unsigned char *img)
{
    static int announced;
    void **slot;
    DWORD old;
    wchar_t tmp[8];

    if (GetEnvironmentVariableW(L"FH2MP_NO_XHR", tmp, 8) > 0) {
        return;
    }
    slot = (void **)(img + FH2_COCREATE_IAT_RVA);
    if (*slot == (void *)hooked_cocreate || *slot == NULL) {
        return;
    }
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        return;
    }
    g_real_cocreate = (co_create_fn)*slot;
    g_cocreate_img = img;
    *slot = (void *)hooked_cocreate;
    VirtualProtect(slot, 8, old, &old);
    if (!announced) {
        announced = 1;
        log_begin();
        log_raw("xhr: game CoCreateInstance import hooked (was 0x");
        log_hex((unsigned long long)g_real_cocreate, 16);
        log_raw("); XMLHTTP60 requests go to the local LSP server\n");
    } else {
        /* Azimuth rebound the slot after we hooked it: anything the game created in between went to its offline stub */
        log_begin();
        log_raw("xhr: game CoCreateInstance import was rebound, hooked again\n");
    }
}

/*
 * XSAPI (Microsoft.Xbox.Services.dll, loaded by Azimuth) makes its own HTTP calls through IXMLHTTPRequest2 obtained
 * with its own CoCreateInstance import, which Azimuth resolves to its offline request stub. Pointing that slot at
 * hooked_cocreate too sends XSAPI's requests (QoS servers, sessions, tickets) to the local LSP, like the game's.
 */
static void patch_xsapi_cocreate(void)
{
    static int announced;
    unsigned char *img;
    void **slot;
    DWORD old;
    wchar_t tmp[8];

    if (GetEnvironmentVariableW(L"FH2MP_NO_XHR", tmp, 8) > 0) {
        return;
    }
    img = find_xsapi_image();
    if (!img) {
        return;
    }
    slot = xs_find_import_slot(img, "CoCreateInstance");
    if (!slot || *slot == (void *)hooked_cocreate || *slot == NULL) {
        return;
    }
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        return;
    }
    if (!g_real_cocreate) {
        g_real_cocreate = (co_create_fn)*slot;
    }
    *slot = (void *)hooked_cocreate;
    VirtualProtect(slot, 8, old, &old);
    if (!announced) {
        announced = 1;
        log_begin();
        log_raw("xhr: XSAPI CoCreateInstance import hooked; its HTTP requests go to the local LSP server\n");
    }
}

/*
 * XSAPI response body. XSAPI's RestCall completion (module +0x378b0) builds a RestCallResponse (+0x28 buffer, +0x30
 * cached body string, +0x38 status) and hands it to FUN_1800014d0 (module +0x14d0, two register arguments). It reads the
 * body from the buffer with Windows DataReader::ReadString, but its DataWriter round trip delivers an empty buffer, so
 * BodyToString returns null and every deserialization (sessions, achievements, ...) is skipped: WriteSessionAsync
 * returns a null session, which parks the matchmaker. The accessor (+0x35ec0) uses the cached string when it is
 * non-empty, so this wraps the call at the end of the completion: it fills the cache from the stream the shim just
 * delivered (UTF-8 -> UTF-16, WindowsCreateString taken from the module's import table), then calls the original.
 * Disable with env FH2MP_NO_XSBODY.
 */
#define XS_COMPLETE_RVA 0x378b0u
#define XS_COMPLETE_END 0x37d50u
#define XS_RESP_CTOR_RVA 0x35d70u
#define XS_RESP_STORE_RVA 0x14d0u

static void (*g_xs_store_orig)(void *, void *);
static LONG (*g_xs_mkstr)(const WCHAR *, unsigned, void **);

static void xs_response_store_stub(void *slot, void *resp)
{
    DWORD tid = GetCurrentThreadId();
    int i;

    if (resp && g_xs_mkstr && *(void **)((unsigned char *)resp + 0x30) == NULL) {
        for (i = 0; i < XHR_CAP_MAX; i++) {
            xhr_stream *st = g_cap[i].s;
            if ((DWORD)g_cap[i].tid == tid && st) {
                unsigned char *p = st->data;
                DWORD n = st->len;
                if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
                    p += 3;
                    n -= 3;
                }
                if (n) {
                    int wn = MultiByteToWideChar(CP_UTF8, 0, (const char *)p, (int)n, NULL, 0);
                    if (wn > 0) {
                        WCHAR *w = (WCHAR *)xhr_alloc(((SIZE_T)wn + 1) * sizeof(WCHAR));
                        if (w) {
                            void *h = NULL;
                            MultiByteToWideChar(CP_UTF8, 0, (const char *)p, (int)n, w, wn);
                            if (g_xs_mkstr(w, (unsigned)wn, &h) >= 0) {
                                *(void **)((unsigned char *)resp + 0x30) = h;
                            }
                            xhr_free(w);
                        }
                    }
                }
                break;
            }
        }
    }
    g_xs_store_orig(slot, resp);
}

/* Last resort that allocates nothing: a run of int3 padding between functions inside the module's own code, which is always
 * within rel32 reach and needs no new executable memory (works where VirtualAlloc of RWX pages is refused or the address space
 * around the module is full). Returns NULL when there is no run of `n` bytes. */
static unsigned char *find_code_padding(unsigned char *img, unsigned n)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)img;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(img + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    unsigned s, i, run;

    for (s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        unsigned char *c;
        unsigned size;
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
            continue;
        }
        c = img + sec[s].VirtualAddress;
        size = sec[s].Misc.VirtualSize;
        for (i = 0, run = 0; i < size; i++) {
            run = c[i] == 0xCC ? run + 1 : 0;
            if (run >= n) {
                return c + i + 1 - n; /* this module's gaps between functions are 8-16 bytes of int3, about 6000 of them are 12+ */
            }
        }
    }
    return NULL;
}

/* Commits one page of executable memory within +-2 GB of `anchor` (so a rel32 call reaches it), nearest free region first, upward
 * and downward alternately. A fixed window after the module is not enough: on some Windows address-space layouts every 64 KB
 * slot there is taken, which left the XSAPI bodies unpatched and parked the matchmaker (reported 2026-10-19). */
static unsigned char *alloc_near(unsigned char *anchor)
{
    SYSTEM_INFO si;
    ULONG_PTR lo, hi, up, down;
    int step;

    GetSystemInfo(&si);
    lo = (ULONG_PTR)anchor > 0x7FF00000u ? (ULONG_PTR)anchor - 0x7FF00000u : 0;
    if (lo < (ULONG_PTR)si.lpMinimumApplicationAddress) {
        lo = (ULONG_PTR)si.lpMinimumApplicationAddress;
    }
    hi = (ULONG_PTR)anchor + 0x7FF00000u;
    if (hi > (ULONG_PTR)si.lpMaximumApplicationAddress) {
        hi = (ULONG_PTR)si.lpMaximumApplicationAddress;
    }
    up = down = (ULONG_PTR)anchor;
    for (step = 0; step < 200000 && (up < hi || down > lo); step++) {
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *p;

        if (up < hi) {
            if (!VirtualQuery((void *)up, &mbi, sizeof mbi)) {
                up = hi;
            } else {
                ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress, end = base + mbi.RegionSize;
                ULONG_PTR cand = (base + 0xFFFFu) & ~(ULONG_PTR)0xFFFFu;
                if (mbi.State == MEM_FREE && cand + 0x1000 <= end && cand + 0x1000 <= hi && cand >= lo &&
                    (p = (unsigned char *)VirtualAlloc((void *)cand, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
                    return p;
                }
                up = end > up ? end : up + 0x1000;
            }
        }
        if (down > lo) {
            if (!VirtualQuery((void *)(down - 1), &mbi, sizeof mbi)) {
                down = lo;
            } else {
                ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress, end = base + mbi.RegionSize;
                ULONG_PTR cand = (end - 0x1000) & ~(ULONG_PTR)0xFFFFu;
                if (mbi.State == MEM_FREE && cand >= base && cand >= lo && cand + 0x1000 <= hi &&
                    (p = (unsigned char *)VirtualAlloc((void *)cand, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
                    return p;
                }
                down = base < down ? base : down - 0x1000;
            }
        }
    }
    return NULL;
}

/* 0: module not there yet (retry), 1: patched, 2: unusable (do not retry) */
static int patch_xsapi_body(void)
{
    unsigned char *img = find_xsapi_image();
    unsigned char *code, *site = NULL, *ctor_site = NULL, *cave = NULL;
    unsigned i;
    DWORD old, err = 0;
    int in_module = 0;
    long long rel;
    int rel32;
    unsigned char jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    ULONGLONG t;

    if (!img) {
        return 0;
    }
    code = img + XS_COMPLETE_RVA;
    for (i = 0; i + 5 < XS_COMPLETE_END - XS_COMPLETE_RVA; i++) {
        if (code[i] == 0xE8) {
            int r;
            mem_copy(&r, code + i + 1, 4);
            if ((long long)XS_COMPLETE_RVA + i + 5 + r == XS_RESP_CTOR_RVA && !ctor_site) {
                ctor_site = code + i;
            } else if (ctor_site && !site && (long long)XS_COMPLETE_RVA + i + 5 + r == XS_RESP_STORE_RVA) {
                site = code + i;
            }
        }
    }
    g_xs_mkstr = (LONG(*)(const WCHAR *, unsigned, void **))xs_find_import(img, "WindowsCreateString");
    if (!site || !g_xs_mkstr || site - ctor_site > 0x100) {
        log_begin();
        log_raw("xsbody: call sites or WindowsCreateString not found, not patched\n");
        return 2;
    }
    for (i = 0; i < 512; i++) { /* the usual place: a code cave just after the module (rel32 call reach) */
        cave = (unsigned char *)VirtualAlloc(img + 0x600000u + (SIZE_T)i * 0x10000u, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_EXECUTE_READWRITE);
        if (cave) {
            break;
        }
    }
    if (!cave) {
        err = GetLastError();
        cave = alloc_near(site);
    }
    if (!cave) {
        cave = find_code_padding(img, 12);
        in_module = cave != NULL;
    }
    if (!cave) {
        log_begin();
        log_raw("xsbody: no code cave near the XSAPI module, not patched (VirtualAlloc error ");
        log_dec(err);
        log_raw(", module ");
        log_hex((unsigned long long)(ULONG_PTR)img, 16);
        log_raw(")\n");
        return 2;
    }
    t = (ULONGLONG)(ULONG_PTR)xs_response_store_stub;
    for (i = 0; i < 8; i++) {
        jmp[2 + i] = (unsigned char)(t >> (8 * i));
    }
    if (in_module && !VirtualProtect(cave, 12, PAGE_EXECUTE_READWRITE, &old)) {
        log_begin();
        log_raw("xsbody: could not make the module padding writable, not patched\n");
        return 2;
    }
    mem_copy(cave, jmp, 12);
    if (in_module) {
        FlushInstructionCache(GetCurrentProcess(), cave, 12);
    }
    rel = (long long)(cave - (site + 5));
    rel32 = (int)rel;
    if ((long long)rel32 != rel) {
        return 2;
    }
    g_xs_store_orig = (void (*)(void *, void *))(img + XS_RESP_STORE_RVA);
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        return 2;
    }
    mem_copy(site + 1, &rel32, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log_begin();
    log_raw(in_module ? "xsbody: XSAPI response bodies are now filled from the HTTP stream (module padding)\n"
                      : "xsbody: XSAPI response bodies are now filled from the HTTP stream\n");
    return 1;
}
