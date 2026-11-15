/* The Modded cars event option between this game and the server (native/internal/game/eventopts.go, its own magic FH2MPEO1; the row itself is
 * event_options.inc). Like the horn (honk_sync.inc) it rides on the game port as signed datagrams: a few times a second this game asks, and
 * for EVO_SAY_MS after its player started an event it says what the Modded cars row was set to. The server keeps that as the event's setting
 * when the player is the event's leader, and its reply tells every game of the session whether the pending or running event takes modded
 * cars. Two things follow the reply here:
 *   - the car pickers (picker_pi.inc, "custom cars out of online events") stop refusing a car pack's own car while the event takes them;
 *   - the row is only put on Event Options while the server answers (evo_server_answers), so a server without this, a Road Trip session (the
 *     server does not answer there) or blocked UDP replies leave the screen as the game makes it.
 * A reply older than EVO_STALE_MS counts for nothing: modded cars are refused again and the row is not added. Needs the lighting hooks (the
 * server address, ticket epoch and generation are theirs). The network thread never touches the game. FH2MP_NO_EVENTOPTS=1 turns the row and
 * this off. */
#include "eventopts_wire.h"

#define EVO_POLL_MS 400u         /* a poll while there is nothing to say */
#define EVO_BUSY_MS 80u          /* while the leader's choice is being said */
#define EVO_SAY_MS 8000u         /* how long after an event start the choice is said */
#define EVO_STALE_MS 3000u       /* a reply older than this counts for nothing */

#ifndef EVO_NOW
#define EVO_NOW() GetTickCount64()
#endif

static volatile LONG g_evo_allowed;        /* the Modded cars row at the last event start from this game: 1 Allowed */
static volatile LONG64 g_evo_started_at;   /* when that was (EVO_NOW), 0: never */
static volatile LONG64 g_evo_reply_at;     /* when the server last answered, 0: never */
static volatile LONG g_evo_running;        /* the worker runs (FH2MP_NO_EVENTOPTS unset) */

/* The row's function calls this when the leader starts an event (event_options.inc). */
static void evo_leader_chose(int allowed)
{
    InterlockedExchange(&g_evo_allowed, allowed != 0);
    InterlockedExchange64(&g_evo_started_at, (LONG64)EVO_NOW());
}

/* What the next request says: 0 or 1 for EVO_SAY_MS after an event start from this game, EO_NONE else. */
static unsigned char evo_choice_to_say(ULONGLONG now)
{
    LONG64 at = g_evo_started_at;
    if (!at || now - (ULONGLONG)at > EVO_SAY_MS) {
        return EO_NONE;
    }
    return g_evo_allowed ? 1 : 0;
}

/* 1 while the server answers event options requests for this session. */
static int evo_server_answers(void)
{
    LONG64 at = g_evo_reply_at;
    return at && EVO_NOW() - (ULONGLONG)at <= EVO_STALE_MS;
}

/* An authenticated reply: what the car pickers may do, and that the server answers. */
static void evo_reply(unsigned char flags, ULONGLONG now)
{
    InterlockedExchange(&g_ppi_modded_ok, (flags & EO_MODDED) != 0);
    InterlockedExchange64(&g_evo_reply_at, (LONG64)now);
}

/* No reply for EVO_STALE_MS: the pickers refuse modded cars again. */
static void evo_expire(ULONGLONG now)
{
    LONG64 at = g_evo_reply_at;
    if (g_ppi_modded_ok && (!at || now - (ULONGLONG)at > EVO_STALE_MS)) {
        InterlockedExchange(&g_ppi_modded_ok, 0);
    }
}

#ifndef EVO_TEST
static DWORD WINAPI evo_worker(void *unused)
{
    HMODULE ws;
    SOCKET s=INVALID_SOCKET; ULONGLONG seq=0,received=0,generation=0,last_send=0,polls=0;
    SOCKET (WSAAPI *make_socket)(int,int,int);
    int (WSAAPI *nonblocking)(SOCKET,long,u_long *);
    int (WSAAPI *receive)(SOCKET,char *,int,int,struct sockaddr *,int *);
    int (WSAAPI *option)(SOCKET,int,int,const char *,int);
    int (WSAAPI *close_socket)(SOCKET);
    u_long nb=1; int v6only=0, socket_told=0, silent_told=0, was_modded=-1; unsigned rejected=0;
    (void)unused;
    ws=LoadLibraryW(L"ws2_32.dll");
    if(ws) {
        make_socket=(void *)GetProcAddress(ws,"socket"); nonblocking=(void *)GetProcAddress(ws,"ioctlsocket");
        receive=(void *)GetProcAddress(ws,"recvfrom"); option=(void *)GetProcAddress(ws,"setsockopt");
        close_socket=(void *)GetProcAddress(ws,"closesocket");
    } else make_socket=NULL,nonblocking=NULL,receive=NULL,option=NULL,close_socket=NULL;
    if(!make_socket || !nonblocking || !receive || !option || !close_socket) {
        log_begin();log_raw("eventopts: the Modded cars option is off, ws2_32.dll could not be used (error ");log_dec(GetLastError());log_raw(")\n");
        return 0;
    }
    for(;;) {
        unsigned char address[28],epoch[16],packet[EO_REQUEST],choice;
        int length, i, n, drained; ULONGLONG predicted,current_generation,now=EVO_NOW();
        evo_expire(now);
        if(!g_light_enabled) {Sleep(1000);continue;}
        AcquireSRWLockShared(&g_light_lock);
        length=g_light.address_len; predicted=g_light.prediction_at; current_generation=g_light.generation;
        light_copy(address,g_light.address,(unsigned)length);
        for(i=0;i<16;i++) epoch[i]=g_light.epoch[i];
        ReleaseSRWLockShared(&g_light_lock);
        if(generation!=current_generation) {   /* a new game connection: start over */
            if(s!=INVALID_SOCKET) close_socket(s);
            s=INVALID_SOCKET; received=0; generation=current_generation; polls=0; rejected=0; silent_told=0;
        }
        if(!length || !predicted || now-predicted>1000 || !g_ticket_ready || !g_r_wsasendto) {Sleep(100);continue;}
        if(s==INVALID_SOCKET) {
            s=make_socket(*(const unsigned short *)address,SOCK_DGRAM,IPPROTO_UDP);
            if(s==INVALID_SOCKET) {
                if(!socket_told) {socket_told=1;log_begin();log_raw("eventopts: could not open the poll socket, error ");log_dec(GetLastError());log_raw(", retrying every 0.5 s\n");}
                Sleep(500);continue;
            }
            if(*(const unsigned short *)address==AF_INET6) option(s,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&v6only,sizeof v6only);
            if(nonblocking(s,FIONBIO,&nb)) {
                if(!socket_told) {socket_told=1;log_begin();log_raw("eventopts: could not make the poll socket non-blocking, error ");log_dec(GetLastError());log_raw(", retrying every 0.5 s\n");}
                close_socket(s);s=INVALID_SOCKET;Sleep(500);continue;
            }
            socket_told=0;
        }
        for(drained=0;drained<32 && (n=receive(s,(char *)packet,sizeof packet,0,NULL,NULL))>0;drained++) {
            unsigned char flags=0;
            if(!eo_response(packet,(unsigned)n,g_ticket_key,g_xuid,seq,received,epoch,&flags)) {rejected++;continue;}
            if(!received) {
                log_begin();log_raw("eventopts: the server answers (after ");log_dec((unsigned long)polls);
                log_raw(" polls): the session leader's Event Options get the Modded cars row\n");
            }
            received=light_u64(packet+28);
            evo_reply(flags,now);
            if(was_modded!=(int)(flags&EO_MODDED)) {
                was_modded=(int)(flags&EO_MODDED);
                log_begin();log_raw(was_modded ? "eventopts: this session's event takes modded cars: the car pickers let car packs' cars in\n"
                                               : "eventopts: this session's event does not take modded cars\n");
            }
        }
        choice=evo_choice_to_say(now);
        if(now-last_send>=(choice!=EO_NONE ? EVO_BUSY_MS : EVO_POLL_MS)) {
            WSABUF b; DWORD sent=0;
            eo_request(packet,g_ticket_key,g_xuid,++seq,epoch,choice);
            b.len=EO_REQUEST; b.buf=(char *)packet;
            g_r_wsasendto(s,&b,1,&sent,0,address,length,NULL,NULL);
            last_send=now;
            if(++polls%300==0) {
                if(!received && !silent_told) {
                    silent_told=1;
                    log_begin();log_raw("eventopts: no reply to ");log_dec((unsigned long)polls);
                    log_raw(" polls: the server does not answer event options here (a server without them, a Road Trip session, or UDP replies blocked); no Modded cars row\n");
                }
                if(rejected) {
                    log_begin();log_raw("eventopts: ");log_dec(rejected);
                    log_raw(" reply datagram(s) refused (not a valid, authentic, in-sequence reply for this session)\n");
                    rejected=0;
                }
            }
        }
        Sleep(20);
    }
}

/* FH2MP_NO_EVENTOPTS unset: the worker. The row's hook goes in with patch_event_options (event_options.inc). */
static void start_eventopts(void)
{
    HANDLE thread=CreateThread(NULL,0,evo_worker,NULL,0,NULL);
    if(thread) {CloseHandle(thread);InterlockedExchange(&g_evo_running,1);}
    else {log_begin();log_raw("eventopts: could not start its thread, error ");log_dec(GetLastError());log_raw("; no Modded cars row\n");}
}
#endif
