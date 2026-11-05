/* Quick chat (the QuickChat mod, fh2-mods\QuickChat.dll, an optional add-on). QuickChat sends nothing itself: it publishes QuickChat.Bridge
 * (quickchat_bridge.h) through the platform, and this file carries its messages. Once every mod has started it gets the bridge; without
 * QuickChat it does nothing. Then, like the horn (honk_sync.inc), it tells the server (a side channel like lighting, server/quickchat.py,
 * its own magic FH2MPQC1) the newest message the player sent, a number and a phrase, and hands the other players' messages in the reply to
 * QuickChat, which shows each (player, number) once. Only a phrase number crosses the network. A message is repeated until the reply says
 * the server took it, so a lost datagram heals on the next one. Needs the lighting hooks (the server address, ticket epoch and generation
 * are theirs). The network thread never touches the game; QuickChat's calls are any-thread. FH2MP_NO_QUICKCHAT=1 turns it off. */
#include "quickchat_bridge.h"
#include "quickchat_wire.h"

#define QC_POLL_MS 250u          /* a poll while nothing of ours is waiting to be taken */
#define QC_BUSY_MS 80u           /* while one is */

static const QuickChatBridge *g_qc;   /* set once by the worker */

/* The bridge, once every mod's fh2mod_init has returned (QuickChat publishes it there); NULL without QuickChat. */
static const QuickChatBridge *qc_find_bridge(void)
{
    const QuickChatBridge *b;
    if(!g_api || !g_api->wait_for_mods || !g_api->get_interface) return NULL;
    g_api->wait_for_mods(0xFFFFFFFFu);
    b=(const QuickChatBridge *)g_api->get_interface(QUICKCHAT_BRIDGE,1);
    if(b && (b->size<sizeof(QuickChatBridge) || !b->latest || !b->deliver)) return NULL;
    return b;
}

static DWORD WINAPI qc_worker(void *unused)
{
    HMODULE ws;
    SOCKET s=INVALID_SOCKET; ULONGLONG seq=0,received=0,generation=0,last_send=0,polls=0;
    SOCKET (WSAAPI *make_socket)(int,int,int);
    int (WSAAPI *nonblocking)(SOCKET,long,u_long *);
    int (WSAAPI *receive)(SOCKET,char *,int,int,struct sockaddr *,int *);
    int (WSAAPI *option)(SOCKET,int,int,const char *,int);
    int (WSAAPI *close_socket)(SOCKET);
    u_long nb=1; int v6only=0, socket_told=0, silent_told=0; unsigned rejected=0, delivered=0, sent_number=0, taken=0;
    (void)unused;
    g_qc=qc_find_bridge();
    if(!g_qc) {
        log_begin();log_raw("quickchat: QuickChat isn't installed (no QuickChat.Bridge); quick chat is off\n");
        return 0;
    }
    log_begin();log_raw("quickchat: QuickChat found (bridge v");log_dec(g_qc->version);log_raw(", ");log_dec(g_qc->phrase_count);
    log_raw(" phrases); its messages go to the server\n");
    ws=LoadLibraryW(L"ws2_32.dll");
    if(ws) {
        make_socket=(void *)GetProcAddress(ws,"socket"); nonblocking=(void *)GetProcAddress(ws,"ioctlsocket");
        receive=(void *)GetProcAddress(ws,"recvfrom"); option=(void *)GetProcAddress(ws,"setsockopt");
        close_socket=(void *)GetProcAddress(ws,"closesocket");
    } else make_socket=NULL,nonblocking=NULL,receive=NULL,option=NULL,close_socket=NULL;
    if(!make_socket || !nonblocking || !receive || !option || !close_socket) {
        log_begin();log_raw("quickchat: quick chat is off, ws2_32.dll could not be used (error ");log_dec(GetLastError());log_raw(")\n");
        return 0;
    }
    for(;;) {
        unsigned char address[28],epoch[16],packet[QC_LIMIT]; qc_record records[QC_MAX];
        int length, i, n, drained; ULONGLONG predicted,current_generation,now=GetTickCount64(); unsigned count=0, number=0, phrase=0;
        uint32_t qn=0, qp=0; uint64_t qms=0;
        if(!g_light_enabled) {Sleep(1000);continue;}
        if(g_qc->latest(&qn,&qp,&qms)) number=qn, phrase=qp;
        AcquireSRWLockShared(&g_light_lock);
        length=g_light.address_len; predicted=g_light.prediction_at; current_generation=g_light.generation;
        light_copy(address,g_light.address,(unsigned)length);
        for(i=0;i<16;i++) epoch[i]=g_light.epoch[i];
        ReleaseSRWLockShared(&g_light_lock);
        if(generation!=current_generation) {   /* a new game connection: start over */
            if(s!=INVALID_SOCKET) close_socket(s);
            s=INVALID_SOCKET; received=0; generation=current_generation; polls=0; rejected=0; silent_told=0; taken=0;
        }
        if(!length || !predicted || now-predicted>1000 || !g_ticket_ready || !g_r_wsasendto) {Sleep(100);continue;}
        if(s==INVALID_SOCKET) {
            s=make_socket(*(const unsigned short *)address,SOCK_DGRAM,IPPROTO_UDP);
            if(s==INVALID_SOCKET) {
                if(!socket_told) {socket_told=1;log_begin();log_raw("quickchat: could not open the poll socket, error ");log_dec(GetLastError());log_raw(", retrying every 0.5 s\n");}
                Sleep(500);continue;
            }
            if(*(const unsigned short *)address==AF_INET6) option(s,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&v6only,sizeof v6only);
            if(nonblocking(s,FIONBIO,&nb)) {
                if(!socket_told) {socket_told=1;log_begin();log_raw("quickchat: could not make the poll socket non-blocking, error ");log_dec(GetLastError());log_raw(", retrying every 0.5 s\n");}
                close_socket(s);s=INVALID_SOCKET;Sleep(500);continue;
            }
            socket_told=0;
        }
        for(drained=0;drained<32 && (n=receive(s,(char *)packet,sizeof packet,0,NULL,NULL))>0;drained++) {
            unsigned got=0, k;
            if(!qc_response(packet,(unsigned)n,g_ticket_key,g_xuid,seq,received,epoch,records,&count,&got)) {rejected++;continue;}
            if(!received) {log_begin();log_raw("quickchat: authenticated reply received after ");log_dec((unsigned long)polls);log_raw(" polls\n");}
            received=light_u64(packet+28);
            taken=got;
            for(k=0;k<count;k++)
                if(g_qc->deliver(records[k].owner,records[k].name[0]?records[k].name:NULL,records[k].number,records[k].phrase)) delivered++;
        }
        {
            /* a new message goes out at once; then a poll every 80 ms until the server holds it, every 250 ms else */
            const ULONGLONG gap=(number && taken!=number) ? QC_BUSY_MS : QC_POLL_MS;
            if((number!=sent_number) ? now-last_send>=35 : now-last_send>=gap) {
                WSABUF b; DWORD sent=0;
                qc_request(packet,g_ticket_key,g_xuid,++seq,epoch,number,(unsigned char)(phrase<QC_PHRASES?phrase:0));
                b.len=QC_REQUEST; b.buf=(char *)packet;
                g_r_wsasendto(s,&b,1,&sent,0,address,length,NULL,NULL);
                last_send=now; sent_number=number;
                if(++polls%400==0) {
                    if(!received && !silent_told) {
                        silent_told=1;
                        log_begin();log_raw("quickchat: no reply to ");log_dec((unsigned long)polls);
                        log_raw(" polls, the server does not answer quick chat (a server without it, or UDP replies blocked); messages stay on this screen\n");
                    }
                    if(rejected || delivered) {
                        log_begin();log_raw("quickchat: ");log_dec(delivered);log_raw(" message(s) from other players shown, ");
                        log_dec(rejected);log_raw(" reply datagram(s) refused (not a valid, authentic, in-sequence reply for this session)\n");
                        rejected=0; delivered=0;
                    }
                }
            }
        }
        Sleep(20);
    }
}

/* FH2MP_NO_QUICKCHAT unset: the worker, which waits for every mod to start and does nothing without QuickChat. */
static void start_quickchat(void)
{
    HANDLE thread=CreateThread(NULL,0,qc_worker,NULL,0,NULL);
    if(thread) CloseHandle(thread);
    else {log_begin();log_raw("quickchat: could not start its thread, error ");log_dec(GetLastError());log_raw("; quick chat is off\n");}
}
