#ifndef AIRLINK_AUDIO_H
#define AIRLINK_AUDIO_H
#include <windows.h>

/* One receiver, bounded live PCM, no audio files. Call only from the UI thread. */
BOOL audio_start(HWND window, UINT error_message, unsigned short *port, char token[65]);
void audio_set_remote_ready(BOOL ready);
void audio_stop(void);
BOOL audio_running(void);
/* Internal crash recovery mode, invoked before normal UI initialization. */
int audio_mute_guard(int argc, WCHAR **argv);
#endif

