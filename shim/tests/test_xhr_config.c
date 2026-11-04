#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
static wchar_t g_dir[MAX_PATH]=L"Z:\\fixture";
static HANDLE read_entered,read_finish,second_done;
static volatile LONG reads;
static HANDLE WINAPI test_open(LPCWSTR a,DWORD b,DWORD c,LPSECURITY_ATTRIBUTES d,DWORD e,DWORD f,HANDLE g){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;return (HANDLE)7;}
static BOOL WINAPI test_read(HANDLE a,LPVOID b,DWORD n,LPDWORD got,LPOVERLAPPED x){
 const char config[]="https://api.forza.scuffed.af\n";(void)a;(void)n;(void)x;
 InterlockedIncrement(&reads);SetEvent(read_entered);WaitForSingleObject(read_finish,INFINITE);
 memcpy(b,config,sizeof(config)-1);*got=sizeof(config)-1;return TRUE;
}
static BOOL WINAPI test_close(HANDLE h){(void)h;return TRUE;}
#define CreateFileW test_open
#define ReadFile test_read
#define CloseHandle test_close
#include "src/xhr_config.inc"
#undef CreateFileW
#undef ReadFile
#undef CloseHandle
static DWORD WINAPI first(LPVOID p){(void)p;xhr_load_cfg();return 0;}
static DWORD WINAPI second(LPVOID p){(void)p;xhr_load_cfg();SetEvent(second_done);return 0;}
int main(void){
 read_entered=CreateEventW(NULL,TRUE,FALSE,NULL);read_finish=CreateEventW(NULL,TRUE,FALSE,NULL);second_done=CreateEventW(NULL,TRUE,FALSE,NULL);
 HANDLE a=CreateThread(NULL,0,first,NULL,0,NULL);if(WaitForSingleObject(read_entered,2000)!=WAIT_OBJECT_0)return 2;
 HANDLE b=CreateThread(NULL,0,second,NULL,0,NULL);
 int premature=WaitForSingleObject(second_done,100)==WAIT_OBJECT_0;
 SetEvent(read_finish);WaitForSingleObject(a,2000);WaitForSingleObject(b,2000);
 if(premature){puts("RACE: second request returned before configuration was read");return 1;}
 if(reads!=1 || lstrcmpW(g_lsp_host,L"api.forza.scuffed.af") || g_lsp_port!=443 || !g_lsp_secure)return 3;
 puts("PASS: concurrent requests wait for complete HTTPS configuration");return 0;
}
