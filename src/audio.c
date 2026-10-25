#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <bcrypt.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
#include <string.h>
#include "audio.h"

/* The Windows audio engine resamples to the RAOP wire format. The capture
   thread never waits for the receiver. A separate clocked socket pump keeps
   at most 200 ms of live audio, so reconnects cannot replay stale sound. */
#define BLOCK_BYTES 3528 /* 882 stereo s16 frames = 20 ms at 44100 Hz */
#define RING_BYTES (BLOCK_BYTES * 10)
static struct {
    HANDLE stop, initialized, capture, pump;
    CRITICAL_SECTION lock;
    BYTE ring[RING_BYTES];
    size_t read, count;
    SOCKET listener;
    char token[65];
    HWND window;
    UINT error_message;
    HRESULT result;
    volatile LONG ready;
    HANDLE mute_guard_done;
    BOOL active;
} a;

/* A tiny independent instance restores the exact endpoint after a UI crash.
   It never captures audio and exits as soon as normal cleanup acknowledges it. */
static BOOL start_mute_guard(IMMDevice *device, BOOL previous_mute) {
    WCHAR *id = NULL;
    if (FAILED(IMMDevice_GetId(device, &id))) return FALSE;
    WCHAR base[160], done_name[180], ready_name[180], path[MAX_PATH], command[2048];
    swprintf(base,160,L"Local\\AirLink.Mute.%lu.%.16hs",GetCurrentProcessId(),a.token);
    swprintf(done_name,180,L"%ls.Done",base);
    swprintf(ready_name,180,L"%ls.Ready",base);
    HANDLE ready = CreateEventW(NULL,TRUE,FALSE,ready_name);
    a.mute_guard_done = CreateEventW(NULL,TRUE,FALSE,done_name);
    GetModuleFileNameW(NULL,path,MAX_PATH);
    int length = swprintf(command,2048,L"\"%ls\" --mute-guard %lu %d \"%ls\" \"%ls\"",path,GetCurrentProcessId(),previous_mute,id,base);
    CoTaskMemFree(id);
    BOOL ok = FALSE;
    if (length > 0 && ready && a.mute_guard_done) {
        STARTUPINFOW startup = {0}; startup.cb=sizeof(startup);
        PROCESS_INFORMATION process = {0};
        if(CreateProcessW(path,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)){
            ok=WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;
            CloseHandle(process.hThread);CloseHandle(process.hProcess);
        }
    }
    if(ready)CloseHandle(ready);
    if(!ok && a.mute_guard_done){SetEvent(a.mute_guard_done);CloseHandle(a.mute_guard_done);a.mute_guard_done=NULL;}
    return ok;
}

int audio_mute_guard(int argc, WCHAR **argv) {
    if(argc!=6)return 1;
    DWORD pid=wcstoul(argv[2],NULL,10);BOOL old_mute=wcstol(argv[3],NULL,10)!=0;
    WCHAR done_name[180],ready_name[180];
    swprintf(done_name,180,L"%ls.Done",argv[5]);swprintf(ready_name,180,L"%ls.Ready",argv[5]);
    HANDLE handles[2]={OpenEventW(SYNCHRONIZE,FALSE,done_name),OpenProcess(SYNCHRONIZE,FALSE,pid)};
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,ready_name);
    if(!handles[0]||!handles[1]||!ready){if(handles[0])CloseHandle(handles[0]);if(handles[1])CloseHandle(handles[1]);if(ready)CloseHandle(ready);return 1;}
    IMMDeviceEnumerator *enumerator=NULL;IMMDevice *device=NULL;IAudioEndpointVolume *volume=NULL;
    HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);BOOL com=SUCCEEDED(hr);
    if(com)hr=CoCreateInstance(&CLSID_MMDeviceEnumerator,NULL,CLSCTX_ALL,&IID_IMMDeviceEnumerator,(void**)&enumerator);
    if(SUCCEEDED(hr))hr=IMMDeviceEnumerator_GetDevice(enumerator,argv[4],&device);
    if(SUCCEEDED(hr))hr=IMMDevice_Activate(device,&IID_IAudioEndpointVolume,CLSCTX_ALL,NULL,(void**)&volume);
    if(SUCCEEDED(hr)){
        SetEvent(ready);
        if(WaitForMultipleObjects(2,handles,FALSE,INFINITE)==WAIT_OBJECT_0+1)IAudioEndpointVolume_SetMute(volume,old_mute,NULL);
    }
    if(volume)IAudioEndpointVolume_Release(volume);if(device)IMMDevice_Release(device);if(enumerator)IMMDeviceEnumerator_Release(enumerator);if(com)CoUninitialize();
    CloseHandle(ready);CloseHandle(handles[0]);CloseHandle(handles[1]);return FAILED(hr)?1:0;
}

static void push(const BYTE *data, size_t size, BOOL silent) {
    EnterCriticalSection(&a.lock);
    /* Keep complete stereo frames, even if a device produces a large packet. */
    if (size > RING_BYTES) { if (data) data += size - RING_BYTES; size = RING_BYTES; }
    if (a.count + size > RING_BYTES) {
        size_t skip = a.count + size - RING_BYTES;
        a.read = (a.read + skip) % RING_BYTES;
        a.count -= skip;
    }
    size_t end = (a.read + a.count) % RING_BYTES;
    for (size_t i = 0; i < size; ++i) a.ring[(end + i) % RING_BYTES] = silent ? 0 : data[i];
    a.count += size;
    LeaveCriticalSection(&a.lock);
}

static void pull(BYTE block[BLOCK_BYTES], BOOL live) {
    ZeroMemory(block, BLOCK_BYTES);
    EnterCriticalSection(&a.lock);
    if (!live) { a.count = 0; a.read = 0; }
    else {
        size_t n = a.count < BLOCK_BYTES ? a.count : BLOCK_BYTES;
        for (size_t i = 0; i < n; ++i) block[i] = a.ring[(a.read + i) % RING_BYTES];
        a.read = (a.read + n) % RING_BYTES;
        a.count -= n;
    }
    LeaveCriticalSection(&a.lock);
}

static DWORD WINAPI capture_thread(void *unused) {
    (void)unused;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioCaptureClient *capture = NULL;
    IAudioEndpointVolume *volume = NULL;
    BOOL was_muted = FALSE, changed_mute = FALSE, started = FALSE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL com = SUCCEEDED(hr);
    if (FAILED(hr)) goto done;
#define CHECK(call) do { hr = (call); if (FAILED(hr)) goto done; } while (0)
    CHECK(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator));
    CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device));
    CHECK(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client));
    CHECK(IMMDevice_Activate(device, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&volume));
    CHECK(IAudioEndpointVolume_GetMute(volume, &was_muted));
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0};
    CHECK(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, &format, NULL));
    CHECK(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&capture));
    CHECK(IAudioClient_Start(client));
    started = TRUE;
    a.result = S_OK;
    SetEvent(a.initialized);
    while (WaitForSingleObject(a.stop, 5) == WAIT_TIMEOUT) {
        if (InterlockedCompareExchange(&a.ready, 0, 0) && !changed_mute) {
            if (!start_mute_guard(device,was_muted)) { hr=E_FAIL;goto done; }
            CHECK(IAudioEndpointVolume_SetMute(volume, TRUE, NULL));
            changed_mute = TRUE;
        }
        UINT32 packets = 0;
        CHECK(IAudioCaptureClient_GetNextPacketSize(capture, &packets));
        while (packets) {
            BYTE *data;
            UINT32 frames;
            DWORD flags;
            CHECK(IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL));
            push(data, (size_t)frames * 4, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            CHECK(IAudioCaptureClient_ReleaseBuffer(capture, frames));
            CHECK(IAudioCaptureClient_GetNextPacketSize(capture, &packets));
        }
    }
done:
    if (started) IAudioClient_Stop(client);
    BOOL restored = !changed_mute || SUCCEEDED(IAudioEndpointVolume_SetMute(volume, was_muted, NULL));
    if(a.mute_guard_done){if(restored)SetEvent(a.mute_guard_done);CloseHandle(a.mute_guard_done);a.mute_guard_done=NULL;}
    if (volume) IAudioEndpointVolume_Release(volume);
    if (capture) IAudioCaptureClient_Release(capture);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (com) CoUninitialize();
    if (FAILED(hr)) {
        a.result = hr;
        SetEvent(a.stop);
        if (a.window) PostMessageW(a.window, a.error_message, (WPARAM)(ULONG)hr, 0);
    }
    SetEvent(a.initialized);
    return 0;
#undef CHECK
}

static BOOL stopping(void) { return WaitForSingleObject(a.stop, 0) == WAIT_OBJECT_0; }

static DWORD WINAPI pump_thread(void *unused) {
    (void)unused;
    while (!stopping()) {
        fd_set reads;
        FD_ZERO(&reads); FD_SET(a.listener, &reads);
        struct timeval timeout = {0, 200000};
        if (select(0, &reads, NULL, NULL, &timeout) <= 0) continue;
        SOCKET peer = accept(a.listener, NULL, NULL);
        if (peer == INVALID_SOCKET) continue;
        DWORD ms = 500;
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
        setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
        BOOL no_delay = TRUE;
        setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay));
        char auth[65]; int got = 0;
        ULONGLONG deadline = GetTickCount64() + 1500;
        while (got < 65 && !stopping() && GetTickCount64() < deadline) {
            int n = recv(peer, auth + got, 65 - got, 0);
            if (n <= 0) break;
            got += n;
        }
        if (got != 65 || memcmp(auth, a.token, 64) || auth[64] != '\n') { closesocket(peer); continue; }
        ULONGLONG next = GetTickCount64();
        BYTE block[BLOCK_BYTES];
        while (!stopping()) {
            ULONGLONG now = GetTickCount64();
            if (now < next && WaitForSingleObject(a.stop, (DWORD)(next - now)) != WAIT_TIMEOUT) break;
            if (now > next + 60) next = now;
            next += 20;
            pull(block, InterlockedCompareExchange(&a.ready, 0, 0) != 0);
            int offset = 0;
            while (offset < BLOCK_BYTES && !stopping()) {
                int n = send(peer, (const char *)block + offset, BLOCK_BYTES - offset, 0);
                if (n <= 0) break;
                offset += n;
            }
            if (offset < BLOCK_BYTES) break;
        }
        shutdown(peer, SD_BOTH); closesocket(peer);
    }
    return 0;
}

BOOL audio_start(HWND window, UINT error_message, unsigned short *port, char token[65]) {
    if (a.active) return FALSE;
    ZeroMemory(&a, sizeof(a));
    a.listener = INVALID_SOCKET;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2,2), &data)) return FALSE;
    a.active = TRUE;
    InitializeCriticalSection(&a.lock);
    a.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    a.initialized = CreateEventW(NULL, TRUE, FALSE, NULL);
    a.window = window; a.error_message = error_message; a.result = E_PENDING;
    BYTE random[32];
    if (!a.stop || !a.initialized || BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) goto fail;
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { a.token[i*2] = hex[random[i] >> 4]; a.token[i*2+1] = hex[random[i] & 15]; }
    memcpy(token, a.token, 65);
    a.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (a.listener == INVALID_SOCKET) goto fail;
    BOOL exclusive = TRUE;
    setsockopt(a.listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(a.listener, (struct sockaddr *)&address, sizeof(address)) || listen(a.listener, 2)) goto fail;
    int length = sizeof(address);
    if (getsockname(a.listener, (struct sockaddr *)&address, &length)) goto fail;
    *port = ntohs(address.sin_port);
    a.capture = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!a.capture || WaitForSingleObject(a.initialized, 5000) != WAIT_OBJECT_0 || FAILED(a.result)) goto fail;
    a.pump = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);
    if (!a.pump) goto fail;
    return TRUE;
fail:
    audio_stop(); return FALSE;
}

void audio_set_remote_ready(BOOL ready) {
    if (!a.active) return;
    InterlockedExchange(&a.ready, ready);
    EnterCriticalSection(&a.lock); a.count = 0; a.read = 0; LeaveCriticalSection(&a.lock);
}

void audio_stop(void) {
    if (!a.active) return;
    SetEvent(a.stop);
    if (a.capture) { WaitForSingleObject(a.capture, INFINITE); CloseHandle(a.capture); }
    if (a.pump) { WaitForSingleObject(a.pump, INFINITE); CloseHandle(a.pump); }
    if (a.listener != INVALID_SOCKET) closesocket(a.listener);
    if (a.stop) CloseHandle(a.stop);
    if (a.initialized) CloseHandle(a.initialized);
    DeleteCriticalSection(&a.lock);
    WSACleanup(); a.active = FALSE;
}

BOOL audio_running(void) { return a.active && WaitForSingleObject(a.stop, 0) == WAIT_TIMEOUT; }
