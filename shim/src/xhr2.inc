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
 * "host:port", default 127.0.0.1:5080, plain HTTP). The original host is passed in X-FH2MP-Host.
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
    n = 0;
    while (raw[n] && raw[n] != '\r' && raw[n] != '\n' && raw[n] != ' ') {
        if (raw[n] == ':') {
            colon = n;
        }
        n++;
    }
    raw[n] = '\0';
    if (n == 0) {
        return;
    }
    for (i = 0; i < (colon >= 0 ? colon : n) && i < 120; i++) {
        g_lsp_host[i] = (wchar_t)(unsigned char)raw[i];
    }
    g_lsp_host[i] = 0;
    if (colon >= 0) {
        unsigned v = 0;
        for (i = colon + 1; i < n; i++) {
            v = v * 10 + (unsigned)(raw[i] - '0');
        }
        if (v) {
            g_lsp_port = v;
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
        xhr_free(s->data);
        xhr_free(s);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE st_read(void *self, void *buf, ULONG cb, ULONG *got)
{
    xhr_stream *s = (xhr_stream *)self;
    ULONG n = s->len - s->pos;

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

    ses = g_wh.open(L"fh2mp", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!ses) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }
    if (g_wh.timeouts) {
        g_wh.timeouts(ses, 3000, 3000, 10000, 30000);
    }
    con = g_wh.connect(ses, g_lsp_host, (INTERNET_PORT)g_lsp_port, 0);
    if (!con) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }
    req = g_wh.openreq(con, x->method, path, NULL, NULL, NULL, 0);
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

static HRESULT WINAPI hooked_cocreate(const GUID *clsid, void *outer, DWORD ctx, const GUID *iid, void **out)
{
    if (clsid && xhr_guid_eq(clsid, &XHR_CLSID_FREETHREADED) && !outer) {
        xhr_obj *x = (xhr_obj *)xhr_alloc(sizeof *x);
        HRESULT hr;

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
    *slot = (void *)hooked_cocreate;
    VirtualProtect(slot, 8, old, &old);
    if (!announced) {
        announced = 1;
        log_begin();
        log_raw("xhr: game CoCreateInstance import hooked; XMLHTTP60 requests go to the local LSP server\n");
    }
}
