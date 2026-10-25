#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"

int main(void) {
    unsigned short port; char token[66];
    if (!audio_start(NULL, 0, &port, token)) { puts("FAIL: WASAPI startup"); return 1; }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    DWORD timeout = 3000; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    if (connect(sock, (struct sockaddr*)&address, sizeof(address))) { puts("FAIL: PCM socket"); audio_stop(); return 1; }
    token[64]='\n'; token[65]=0;
    if(send(sock,token,65,0)!=65){puts("FAIL: authentication write");closesocket(sock);audio_stop();return 1;}
    BYTE block[3528];int total=0;
    while(total < 3528*10){int n=recv(sock,(char*)block,sizeof(block),0);if(n<=0)break;total+=n;}
    BOOL running=audio_running();closesocket(sock);audio_stop();
    if(total<3528*10 || !running || audio_running()){printf("FAIL: PCM bytes=%d running=%d\n",total,running);return 1;}
    /* A second start catches stale COM/socket/thread state after Stop. */
    if(!audio_start(NULL,0,&port,token)){puts("FAIL: restart");return 1;}
    audio_stop();
    printf("PASS: WASAPI capture, authenticated PCM, %d bytes, stop and restart\n",total);
    return 0;
}

