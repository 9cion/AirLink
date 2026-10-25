/* Exercise the real language command against hidden controls, without starting
   audio, transport, a tray icon, or writing the user's preferences. */
#define AIRLINK_UI_CHECK
#define wWinMain airlink_app_entry
#include "../src/main.c"
#undef wWinMain

static BOOL caption_is(HWND control,const WCHAR *expected){
    WCHAR actual[240];GetWindowTextW(control,actual,240);return !wcscmp(actual,expected);
}

int main(void){
    app.dpi=96;app.selected=0;app.device_count=1;app.host_ready=TRUE;
    app.wanted=TRUE;app.streaming=TRUE;app.english=TRUE;
    lstrcpyW(app.devices[0].name,L"OP-1");
    app.window=CreateWindowW(L"STATIC",L"",WS_POPUP,0,0,380,386,NULL,NULL,NULL,NULL);
    if(!app.window)return 1;
    HWND *controls[]={&app.language_button,&app.theme_button,
        &app.close_button,&app.slider,&app.start_button,&app.device_button};
    for(size_t i=0;i<sizeof(controls)/sizeof(controls[0]);i++){
        *controls[i]=CreateWindowW(L"BUTTON",L"",WS_CHILD,0,0,32,32,app.window,NULL,NULL,NULL);
        if(!*controls[i]){DestroyWindow(app.window);return 1;}
    }
    update_controls();
    BOOL ok=caption_is(app.language_button,L"en")&&caption_is(app.start_button,L"Stop streaming")
        &&caption_is(app.slider,L"Volume")
        &&!wcscmp(tr(L"Systemaudio wird übertragen"),L"Streaming system audio")
        &&caption_is(app.device_button,L"OP-1")&&app.wanted&&app.streaming;
    window_proc(app.window,WM_COMMAND,ID_LANGUAGE,0);
    ok=ok&&!app.english&&caption_is(app.language_button,L"de")&&caption_is(app.start_button,L"Übertragung stoppen")
        &&caption_is(app.slider,L"Lautstärke")&&app.wanted&&app.streaming;
    window_proc(app.window,WM_COMMAND,ID_LANGUAGE,0);
    ok=ok&&app.english&&caption_is(app.language_button,L"en")&&caption_is(app.start_button,L"Stop streaming")
        &&caption_is(app.slider,L"Volume")&&app.wanted&&app.streaming;
    DestroyWindow(app.window);
    puts(ok?"PASS: DE/EN switching, captions, status, receiver and active route preserved":"FAIL: language switch");
    return ok?0:1;
}
