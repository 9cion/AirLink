#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "audio.h"
#include "i18n.h"

#define APP_NAME L"AirLink"
#define WM_HOST_LINE (WM_APP+1)
#define WM_HOST_EXIT (WM_APP+2)
#define WM_AUDIO_ERROR (WM_APP+3)
#define WM_TRAY (WM_APP+4)
#define ID_THEME 101
#define ID_CLOSE 102
#define ID_DEVICE 103
#define ID_START 104
#define ID_VOLUME 105
#define ID_LANGUAGE 107
#define ID_REFRESH 201
#define ID_OPEN 202
#define ID_EXIT 203
#define ID_DEVICE_BASE 1000
#define MAX_DEVICES 64

typedef struct { char id[65]; WCHAR name[220]; BOOL locked; } Device;
static struct {
    HWND window, language_button, theme_button, close_button, device_button, start_button, slider;
    HANDLE host, job, input, reader, log, mutex;
    HFONT normal, small, title, symbols, status_symbols;
    HICON icon;
    HBRUSH background;
    COLORREF bg, fg, sub, line, field;
    Device devices[MAX_DEVICES];
    int device_count, selected, volume, dpi;
    BOOL dark, english, host_ready, scanning, wanted, streaming, exiting, tray_added, keyboard_focus;
    WCHAR status[260], config[MAX_PATH], log_path[MAX_PATH];
    char last_device[65];
} app;

static int px(int n) { return MulDiv(n, app.dpi, 96); }
static const WCHAR *tr(const WCHAR *source){return airlink_translate(source,app.english);}
static void invalidate(void) { if(app.window) RedrawWindow(app.window, NULL, NULL, RDW_INVALIDATE|RDW_ALLCHILDREN); }
static void status(const WCHAR *text) { lstrcpynW(app.status,text,260); invalidate(); }

static void save_settings(void) {
#ifdef AIRLINK_UI_CHECK
    return;
#endif
    WCHAR text[80];
    WritePrivateProfileStringW(L"Appearance",L"Dark",app.dark?L"1":L"0",app.config);
    WritePrivateProfileStringW(L"Appearance",L"Language",app.english?L"en":L"de",app.config);
    swprintf(text,80,L"%d",app.volume);
    WritePrivateProfileStringW(L"Audio",L"Volume",text,app.config);
    MultiByteToWideChar(CP_UTF8,0,app.last_device,-1,text,80);
    WritePrivateProfileStringW(L"Audio",L"Device",text,app.config);
}

static void theme(void) {
    app.bg=app.dark?RGB(21,21,21):RGB(250,250,250);
    app.fg=app.dark?RGB(244,244,244):RGB(22,22,22);
    app.sub=app.dark?RGB(174,174,174):RGB(101,101,101);
    app.line=app.dark?RGB(54,54,54):RGB(221,221,221);
    app.field=app.dark?RGB(34,34,34):RGB(239,239,239);
    if(app.background)DeleteObject(app.background);
    app.background=CreateSolidBrush(app.bg);
    BOOL dark=app.dark;
    DwmSetWindowAttribute(app.window,20,&dark,sizeof(dark));
    invalidate();
}

static BOOL send_command(const char *text) {
    if(!app.input)return FALSE;
    DWORD count=0; size_t length=strlen(text);
    return WriteFile(app.input,text,(DWORD)length,&count,NULL)&&count==length;
}

static DWORD WINAPI read_host(void *context) {
    HANDLE output=context;
    char line[2048]; int used=0; char chunk[1024]; DWORD count;
    while(ReadFile(output,chunk,sizeof(chunk),&count,NULL)&&count){
        for(DWORD i=0;i<count;i++){
            char c=chunk[i];
            if(c=='\n'){
                line[used]=0;
                char *message=HeapAlloc(GetProcessHeap(),0,(SIZE_T)used+1);
                if(message){memcpy(message,line,(size_t)used+1);if(!PostMessageW(app.window,WM_HOST_LINE,0,(LPARAM)message))HeapFree(GetProcessHeap(),0,message);}
                used=0;
            }else if(c!='\r'){
                if(used >= (int)sizeof(line)-1)goto done;
                line[used++]=c;
            }
        }
    }
done:
    CloseHandle(output);
    PostMessageW(app.window,WM_HOST_EXIT,0,0);
    return 0;
}

static BOOL start_host(void) {
    WCHAR exe[MAX_PATH],folder[MAX_PATH],command[MAX_PATH+4];
    GetModuleFileNameW(NULL,folder,MAX_PATH);
    WCHAR *slash=wcsrchr(folder,L'\\');if(!slash)return FALSE;*slash=0;
    if(swprintf(exe,MAX_PATH,L"%ls\\AirLink.Transport.exe",folder)<0)return FALSE;
    swprintf(command,MAX_PATH+4,L"\"%ls\"",exe);
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE input_read=NULL,output_read=NULL,output_write=NULL;
    if(!CreatePipe(&input_read,&app.input,&sa,0))return FALSE;
    if(!CreatePipe(&output_read,&output_write,&sa,0))goto fail;
    SetHandleInformation(app.input,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(output_read,HANDLE_FLAG_INHERIT,0);
    /* Fresh per-launch diagnostic log. */
    app.log=CreateFileW(app.log_path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(app.log==INVALID_HANDLE_VALUE){app.log=NULL;goto fail;}
    STARTUPINFOW si={0};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    si.wShowWindow=SW_HIDE;si.hStdInput=input_read;si.hStdOutput=output_write;si.hStdError=app.log;
    PROCESS_INFORMATION pi={0};
    app.job=CreateJobObjectW(NULL,NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!app.job||!SetInformationJobObject(app.job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))goto fail;
    if(!CreateProcessW(exe,command,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,NULL,folder,&si,&pi))goto fail;
    if(!AssignProcessToJobObject(app.job,pi.hProcess)){
        TerminateProcess(pi.hProcess,1);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);goto fail;
    }
    app.host=pi.hProcess;
    ResumeThread(pi.hThread);CloseHandle(pi.hThread);
    CloseHandle(input_read);CloseHandle(output_write);
    app.reader=CreateThread(NULL,0,read_host,output_read,0,NULL);
    if(!app.reader){CloseHandle(output_read);TerminateJobObject(app.job,1);return FALSE;}
    return TRUE;
fail:
    if(input_read)CloseHandle(input_read);
    if(output_read)CloseHandle(output_read);
    if(output_write)CloseHandle(output_write);
    if(app.input){CloseHandle(app.input);app.input=NULL;}
    if(app.job){CloseHandle(app.job);app.job=NULL;}
    if(app.log){CloseHandle(app.log);app.log=NULL;}
    return FALSE;
}

static void update_controls(void){
    EnableWindow(app.device_button,app.host_ready&&!app.scanning&&!app.wanted);
    BOOL enabled=app.wanted||(app.host_ready&&!app.scanning&&app.selected>=0&&!app.devices[app.selected].locked);
    EnableWindow(app.start_button,enabled);
    SetWindowTextW(app.language_button,app.english?L"en":L"de");
    SetWindowTextW(app.theme_button,tr(L"Hell/Dunkel umschalten"));
    SetWindowTextW(app.close_button,tr(L"In den Tray minimieren"));
    SetWindowTextW(app.slider,tr(L"Lautstärke"));
    SetWindowTextW(app.start_button,tr(app.wanted?L"Übertragung stoppen":L"Übertragung starten"));
    SetWindowTextW(app.device_button,app.selected>=0?app.devices[app.selected].name:
        tr(app.scanning?L"Lautsprecher werden gesucht …":L"Lautsprecher auswählen"));
    invalidate();
}

static void stop_route(void){
    app.wanted=FALSE;
    app.streaming=FALSE;
    KillTimer(app.window,3);
    /* Stop capture first. This restores the local output even if transport hangs. */
    audio_stop();
    send_command("STOP\n");
    status(L"Bereit zum Übertragen");update_controls();
}

static void start_route(void){
    if(app.selected<0||!app.host_ready||app.scanning||app.devices[app.selected].locked)return;
    unsigned short port;char token[65],command[256];
    status(L"Audioaufnahme wird vorbereitet …");
    if(!audio_start(app.window,WM_AUDIO_ERROR,&port,token)){
        status(L"Audioaufnahme fehlgeschlagen. Windows-Ausgabegerät prüfen.");return;
    }
    app.wanted=TRUE;
    strcpy_s(app.last_device,sizeof(app.last_device),app.devices[app.selected].id);save_settings();
    snprintf(command,sizeof(command),"START\t%s\t%u\t%s\t%d\n",app.devices[app.selected].id,port,token,app.volume);
    if(!send_command(command)){stop_route();status(L"AirPlay-Hilfsprogramm antwortet nicht.");return;}
    status(L"Verbindung wird hergestellt …");update_controls();
}

static void discover(void){
    if(!app.host_ready||app.scanning||app.wanted)return;
    app.device_count=0;app.selected=-1;app.scanning=TRUE;
    status(L"AirPlay-Lautsprecher werden gesucht …");update_controls();
    if(!send_command("DISCOVER\n")){app.scanning=FALSE;status(L"Lautsprechersuche konnte nicht gestartet werden.");update_controls();}
}

static void host_line(char *line){
    char *context=NULL;
    char *kind=strtok_s(line,"\t",&context);if(!kind)return;
    if(!strcmp(kind,"READY")){app.host_ready=TRUE;discover();}
    else if(!strcmp(kind,"DEVICE")){
        char *id=strtok_s(NULL,"\t",&context),*name=strtok_s(NULL,"\t",&context),*locked=strtok_s(NULL,"\t",&context);
        if(!id||!name||strlen(id)!=64||app.device_count>=MAX_DEVICES)return;
        for(int i=0;i<64;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return;
        Device *device=&app.devices[app.device_count];
        strcpy_s(device->id,sizeof(device->id),id);
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name,-1,device->name,220);
        device->locked=locked&&atoi(locked)!=0;
        if(!strcmp(id,app.last_device))app.selected=app.device_count;
        app.device_count++;
    }else if(!strcmp(kind,"SCAN_END")){
        app.scanning=FALSE;
        if(app.selected<0&&app.device_count>0)app.selected=0;
        status(app.device_count?L"Bereit zum Übertragen":L"Keine Lautsprecher gefunden. Gleiches Netzwerk prüfen.");update_controls();
    }else if(!strcmp(kind,"STATE")){
        char *state=strtok_s(NULL,"\t",&context);if(!state||!app.wanted)return;
        if(!strcmp(state,"streaming")){app.streaming=TRUE;audio_set_remote_ready(TRUE);status(L"Systemaudio wird übertragen");}
        else if(!strcmp(state,"reconnecting")){app.streaming=FALSE;audio_set_remote_ready(FALSE);status(L"Verbindung unterbrochen. Verbinde erneut …");}
        else if(!strcmp(state,"connecting")){app.streaming=FALSE;audio_set_remote_ready(FALSE);status(L"Verbindung wird hergestellt …");}
    }else if(!strcmp(kind,"ERROR")||!strcmp(kind,"NOTICE")){
        WCHAR message[260]=L"AirPlay-Fehler. Bitte erneut versuchen.";
        if(context&&*context)MultiByteToWideChar(CP_UTF8,0,context,-1,message,260);
        if(!strcmp(kind,"ERROR")&&app.wanted)stop_route();
        status(message);
    }
}

static void device_menu(void){
    HMENU menu=CreatePopupMenu();
    for(int i=0;i<app.device_count;i++){
        WCHAR label[250];swprintf(label,250,L"%ls%ls",app.devices[i].name,app.devices[i].locked?tr(L" (Kopplung erforderlich)"):L"");
        AppendMenuW(menu,MF_STRING|(i==app.selected?MF_CHECKED:0),ID_DEVICE_BASE+i,label);
    }
    if(app.device_count)AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING,ID_REFRESH,tr(L"Erneut suchen"));
    RECT r;GetWindowRect(app.device_button,&r);SetForegroundWindow(app.window);
    int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN|TPM_TOPALIGN,r.left,r.bottom,0,app.window,NULL);
    DestroyMenu(menu);
    if(id==ID_REFRESH)discover();
    else if(id>=ID_DEVICE_BASE&&id<ID_DEVICE_BASE+app.device_count){
        app.selected=id-ID_DEVICE_BASE;
        strcpy_s(app.last_device,sizeof(app.last_device),app.devices[app.selected].id);save_settings();
        status(app.devices[app.selected].locked?L"Dieser Empfänger benötigt Kopplung; noch nicht unterstützt.":L"Bereit zum Übertragen");update_controls();
    }
}

static void fill(HDC dc,RECT rect,COLORREF color){HBRUSH b=CreateSolidBrush(color);FillRect(dc,&rect,b);DeleteObject(b);}
static void line(HDC dc,int x,int y,int x2,int y2,COLORREF color,int width){
    HPEN p=CreatePen(PS_SOLID,width,color);HGDIOBJ old=SelectObject(dc,p);MoveToEx(dc,x,y,NULL);LineTo(dc,x2,y2);SelectObject(dc,old);DeleteObject(p);
}
static void text(HDC dc,const WCHAR *value,RECT rect,HFONT font,COLORREF color,UINT format){
    HGDIOBJ old=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,value,-1,&rect,format);SelectObject(dc,old);
}
static RECT rect(int x,int y,int w,int h){RECT r={px(x),px(y),px(x+w),px(y+h)};return r;}

static void focus_outline(HDC dc,RECT bounds,COLORREF color,int radius){
    HPEN pen=CreatePen(PS_SOLID,px(1),color);
    HGDIOBJ old_pen=SelectObject(dc,pen),old_brush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
    if(radius)RoundRect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom,radius*2,radius*2);
    else Rectangle(dc,bounds.left,bounds.top,bounds.right,bounds.bottom);
    SelectObject(dc,old_brush);SelectObject(dc,old_pen);DeleteObject(pen);
}

static void rounded_button_surface(HDC dc,RECT bounds,COLORREF background,COLORREF border){
    /* Supersample only the surface; keep text sharp at the display's native DPI. */
    const int scale=4;
    int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    HDC surface=CreateCompatibleDC(dc);
    HBITMAP bitmap=CreateCompatibleBitmap(dc,width*scale,height*scale);
    if(!surface||!bitmap){
        if(bitmap)DeleteObject(bitmap);if(surface)DeleteDC(surface);
        fill(dc,bounds,background);focus_outline(dc,bounds,border,px(8));return;
    }
    HGDIOBJ old_bitmap=SelectObject(surface,bitmap);
    RECT area={0,0,width*scale,height*scale};fill(surface,area,app.bg);
    /* 0.75 logical pixels, centered inside the bounds so no edge is clipped. */
    int stroke=px(3),inset=(stroke+1)/2;
    HPEN pen=CreatePen(PS_SOLID,stroke,border);
    HBRUSH brush=CreateSolidBrush(background);
    HGDIOBJ old_pen=SelectObject(surface,pen),old_brush=SelectObject(surface,brush);
    RoundRect(surface,inset,inset,area.right-inset,area.bottom-inset,px(16)*scale,px(16)*scale);
    SelectObject(surface,old_brush);SelectObject(surface,old_pen);DeleteObject(brush);DeleteObject(pen);
    int old_mode=SetStretchBltMode(dc,HALFTONE);POINT old_origin;
    SetBrushOrgEx(dc,0,0,&old_origin);
    StretchBlt(dc,bounds.left,bounds.top,width,height,surface,0,0,area.right,area.bottom,SRCCOPY);
    SetBrushOrgEx(dc,old_origin.x,old_origin.y,NULL);SetStretchBltMode(dc,old_mode);
    SelectObject(surface,old_bitmap);DeleteObject(bitmap);DeleteDC(surface);
}

static void draw_button(DRAWITEMSTRUCT *item){
    HDC dc=item->hDC;RECT r=item->rcItem;
    BOOL primary=item->CtlID==ID_START,disabled=(item->itemState&ODS_DISABLED)!=0;
    COLORREF bg=primary?(app.dark?app.bg:RGB(255,255,255)):app.bg;
    COLORREF fg=primary?(disabled?app.sub:(app.dark?RGB(255,255,255):RGB(0,0,0))):app.fg;
    fill(dc,r,app.bg);
    if(primary)rounded_button_surface(dc,r,bg,disabled?app.line:fg);
    WCHAR label[240];GetWindowTextW(item->hwndItem,label,240);
    if(item->CtlID==ID_LANGUAGE){
        text(dc,label,r,app.normal,fg,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }else if(item->CtlID==ID_THEME){
        /* Segoe MDL2: QuietHours (moon) and Brightness (sun). */
        text(dc,app.dark?L"\xE708":L"\xE706",r,app.symbols,fg,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }else if(item->CtlID==ID_CLOSE){
        /* Segoe MDL2: Cancel, matching the theme and settings icons. */
        text(dc,L"\xE711",r,app.symbols,fg,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }else{
        RECT t=r;t.left+=px(primary?16:0);t.right-=px(28);
        text(dc,label,t,item->CtlID==ID_DEVICE?app.title:app.normal,disabled?app.sub:fg,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        int x=r.right-px(16),y=r.bottom/2;
        if(primary){
            if(app.wanted){RECT square={x-px(5),y-px(4),x+px(3),y+px(4)};fill(dc,square,fg);}
            else{
                POINT triangle[3]={{x-px(5),y-px(6)},{x+px(4),y},{x-px(5),y+px(6)}};
                HBRUSH brush=CreateSolidBrush(fg);HGDIOBJ old_brush=SelectObject(dc,brush),old_pen=SelectObject(dc,GetStockObject(NULL_PEN));
                Polygon(dc,triangle,3);SelectObject(dc,old_pen);SelectObject(dc,old_brush);DeleteObject(brush);
            }
        }else{line(dc,x-px(4),y-px(2),x,y+px(2),app.sub,px(1));line(dc,x,y+px(2),x+px(4),y-px(2),app.sub,px(1));line(dc,0,r.bottom-1,r.right,r.bottom-1,app.line,px(1));}
    }
    if(app.keyboard_focus&&(item->itemState&ODS_FOCUS)){
        InflateRect(&r,-px(3),-px(3));focus_outline(dc,r,fg,primary?px(5):0);
    }
}

static void paint(HWND window){
    PAINTSTRUCT ps;HDC dest=BeginPaint(window,&ps);RECT client;GetClientRect(window,&client);
    HDC dc=CreateCompatibleDC(dest);HBITMAP bitmap=CreateCompatibleBitmap(dest,client.right,client.bottom);HGDIOBJ old=SelectObject(dc,bitmap);
    fill(dc,client,app.bg);
    if(app.icon)DrawIconEx(dc,px(24),px(18),app.icon,px(20),px(20),0,NULL,DI_NORMAL);
    text(dc,L"A I R L I N K",rect(56,12,160,32),app.small,app.fg,DT_VCENTER|DT_SINGLELINE);
    line(dc,0,px(56),client.right,px(56),app.line,px(1));
    const WCHAR *status_text=tr(app.status);
    RECT s=rect(26,79,320,42);text(dc,status_text,s,app.small,app.sub,DT_WORDBREAK|DT_NOPREFIX);
    BOOL ready=!app.wanted&&!wcscmp(app.status,L"Bereit zum Übertragen");
    if(app.streaming||ready){
        /* Measure the current font so the indicator follows either language at every DPI. */
        SIZE extent;HGDIOBJ font=SelectObject(dc,app.small);
        GetTextExtentPoint32W(dc,status_text,lstrlenW(status_text),&extent);SelectObject(dc,font);
        if(ready){
            RECT check={s.left+extent.cx+px(6),s.top,s.left+extent.cx+px(20),s.top+extent.cy};
            text(dc,L"\xE73E",check,app.status_symbols,app.sub,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        }else{
        int radius=px(3),cx=s.left+extent.cx+px(10),cy=s.top+extent.cy/2;
        if(cx>s.right-radius)cx=s.right-radius;
        HBRUSH brush=CreateSolidBrush(app.dark?RGB(60,205,125):RGB(25,155,80));
        HGDIOBJ old_brush=SelectObject(dc,brush),old_pen=SelectObject(dc,GetStockObject(NULL_PEN));
        Ellipse(dc,cx-radius,cy-radius,cx+radius,cy+radius);
        SelectObject(dc,old_pen);SelectObject(dc,old_brush);DeleteObject(brush);
        }
    }
    text(dc,tr(L"L A U T S P R E C H E R"),rect(26,135,300,18),app.small,app.sub,DT_SINGLELINE);
    text(dc,tr(L"Lautstärke"),rect(26,225,220,20),app.small,app.fg,DT_SINGLELINE);
    WCHAR volume[20];swprintf(volume,20,L"%d %%",app.volume);text(dc,volume,rect(270,225,84,20),app.small,app.sub,DT_RIGHT|DT_SINGLELINE);
    line(dc,0,px(344),client.right,px(344),app.line,px(1));
    BitBlt(dest,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);EndPaint(window,&ps);
}

static void fonts_and_layout(void){
    if(app.normal)DeleteObject(app.normal);if(app.small)DeleteObject(app.small);if(app.title)DeleteObject(app.title);
    if(app.symbols)DeleteObject(app.symbols);
    if(app.status_symbols)DeleteObject(app.status_symbols);
    app.symbols=CreateFontW(-px(20),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe MDL2 Assets");
    app.status_symbols=CreateFontW(-px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe MDL2 Assets");
    app.normal=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    app.small=CreateFontW(-px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    app.title=CreateFontW(-px(20),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    MoveWindow(app.language_button,px(238),px(12),px(32),px(32),TRUE);
    MoveWindow(app.theme_button,px(280),px(12),px(32),px(32),TRUE);
    MoveWindow(app.close_button,px(322),px(12),px(32),px(32),TRUE);
    MoveWindow(app.device_button,px(26),px(157),px(328),px(44),TRUE);
    MoveWindow(app.slider,px(24),px(250),px(332),px(25),TRUE);
    MoveWindow(app.start_button,px(26),px(286),px(328),px(42),TRUE);
}

static HICON make_icon(void){
    HDC screen=GetDC(NULL),dc=CreateCompatibleDC(screen);HBITMAP color=CreateCompatibleBitmap(screen,32,32),mask=CreateBitmap(32,32,1,1,NULL);
    HGDIOBJ old=SelectObject(dc,color);RECT r={0,0,32,32};fill(dc,r,RGB(22,22,22));
    HFONT f=CreateFontW(-25,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI");text(dc,L"A",r,f,RGB(250,250,250),DT_CENTER|DT_VCENTER|DT_SINGLELINE);DeleteObject(f);
    SelectObject(dc,mask);PatBlt(dc,0,0,32,32,BLACKNESS);SelectObject(dc,old);
    ICONINFO info={TRUE,0,0,mask,color};HICON icon=CreateIconIndirect(&info);
    DeleteObject(color);DeleteObject(mask);DeleteDC(dc);ReleaseDC(NULL,screen);return icon;
}

static void add_tray(void){
    NOTIFYICONDATAW tray={0};tray.cbSize=sizeof(tray);tray.hWnd=app.window;tray.uID=1;
    tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray.uCallbackMessage=WM_TRAY;tray.hIcon=app.icon;lstrcpyW(tray.szTip,tr(L"AirLink – Systemaudio über AirPlay"));
    app.tray_added=Shell_NotifyIconW(NIM_ADD,&tray);
}

static void show_window(void){ShowWindow(app.window,SW_RESTORE);SetForegroundWindow(app.window);}

static void shutdown_app(void){
    app.exiting=TRUE;audio_stop();save_settings();send_command("QUIT\n");
    if(app.input){CloseHandle(app.input);app.input=NULL;}
    if(app.host&&WaitForSingleObject(app.host,2500)==WAIT_TIMEOUT&&app.job)TerminateJobObject(app.job,0);
    if(app.reader){WaitForSingleObject(app.reader,1000);CloseHandle(app.reader);}
    if(app.host)CloseHandle(app.host);if(app.job)CloseHandle(app.job);if(app.log)CloseHandle(app.log);
    NOTIFYICONDATAW tray={0};tray.cbSize=sizeof(tray);tray.hWnd=app.window;tray.uID=1;Shell_NotifyIconW(NIM_DELETE,&tray);
    /* Free protocol messages queued while waiting for the child to exit. */
    MSG msg;while(PeekMessageW(&msg,app.window,WM_HOST_LINE,WM_HOST_LINE,PM_REMOVE))HeapFree(GetProcessHeap(),0,(void*)msg.lParam);
}

static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    static UINT taskbar_created;
    if(taskbar_created&&message==taskbar_created){add_tray();return 0;}
    switch(message){
    case WM_CREATE:
        app.window=window;taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
        app.language_button=CreateWindowW(L"BUTTON",L"en",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,(HMENU)ID_LANGUAGE,NULL,NULL);
        app.theme_button=CreateWindowW(L"BUTTON",L"Hell/Dunkel umschalten",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,(HMENU)ID_THEME,NULL,NULL);
        app.close_button=CreateWindowW(L"BUTTON",L"In den Tray minimieren",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,(HMENU)ID_CLOSE,NULL,NULL);
        app.device_button=CreateWindowW(L"BUTTON",L"Lautsprecher auswählen",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,(HMENU)ID_DEVICE,NULL,NULL);
        app.start_button=CreateWindowW(L"BUTTON",L"Übertragung starten",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,(HMENU)ID_START,NULL,NULL);
        app.slider=CreateWindowW(TRACKBAR_CLASSW,L"Lautstärke",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_NOTICKS,0,0,0,0,window,(HMENU)ID_VOLUME,NULL,NULL);
        SendMessageW(app.slider,TBM_SETRANGE,TRUE,MAKELPARAM(0,100));SendMessageW(app.slider,TBM_SETPOS,TRUE,app.volume);SetWindowTheme(app.slider,L"",L"");
        fonts_and_layout();theme();app.icon=make_icon();SendMessageW(window,WM_SETICON,ICON_SMALL,(LPARAM)app.icon);add_tray();
        status(L"AirPlay wird vorbereitet …");update_controls();SetTimer(window,1,100,NULL);return 0;
    case WM_NCCALCSIZE:if(wparam)return 0;break;
    case WM_NCHITTEST:{POINT p={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ScreenToClient(window,&p);if(p.y<px(56)&&p.x<px(228))return HTCAPTION;return HTCLIENT;}
    case WM_GETMINMAXINFO: {MINMAXINFO *m=(MINMAXINFO*)lparam;m->ptMinTrackSize.x=m->ptMaxTrackSize.x=px(380);m->ptMinTrackSize.y=m->ptMaxTrackSize.y=px(386);return 0;}
    case WM_DPICHANGED:app.dpi=HIWORD(wparam);{RECT *r=(RECT*)lparam;SetWindowPos(window,NULL,r->left,r->top,px(380),px(386),SWP_NOZORDER|SWP_NOACTIVATE);fonts_and_layout();}return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:paint(window);return 0;
    case WM_DRAWITEM:draw_button((DRAWITEMSTRUCT*)lparam);return TRUE;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:SetBkColor((HDC)wparam,app.bg);SetTextColor((HDC)wparam,app.fg);return (LRESULT)app.background;
    case WM_NOTIFY:{NMHDR *hdr=(NMHDR*)lparam;if(hdr->hwndFrom==app.slider&&hdr->code==NM_CUSTOMDRAW){
        NMCUSTOMDRAW *draw=(NMCUSTOMDRAW*)lparam;
        if(draw->dwDrawStage==CDDS_PREPAINT){
            RECT bounds,track,thumb;GetClientRect(app.slider,&bounds);fill(draw->hdc,bounds,app.bg);
            SendMessageW(app.slider,TBM_GETCHANNELRECT,0,(LPARAM)&track);SendMessageW(app.slider,TBM_GETTHUMBRECT,0,(LPARAM)&thumb);
            int y=(thumb.top+thumb.bottom)/2;track.top=y-px(1);track.bottom=y+px(1);fill(draw->hdc,track,app.line);
            track.right=(thumb.left+thumb.right)/2;fill(draw->hdc,track,app.fg);
            HBRUSH b=CreateSolidBrush(app.fg);HGDIOBJ old=SelectObject(draw->hdc,b),pen=SelectObject(draw->hdc,GetStockObject(NULL_PEN));
            int cx=(thumb.left+thumb.right)/2;Ellipse(draw->hdc,cx-px(5),y-px(5),cx+px(5),y+px(5));
            SelectObject(draw->hdc,old);SelectObject(draw->hdc,pen);DeleteObject(b);
            if(app.keyboard_focus&&GetFocus()==app.slider){
                InflateRect(&bounds,-px(1),-px(1));focus_outline(draw->hdc,bounds,app.sub,0);
            }
            return CDRF_SKIPDEFAULT;
        }
    }break;}
    case WM_HSCROLL:if((HWND)lparam==app.slider){app.volume=(int)SendMessageW(app.slider,TBM_GETPOS,0,0);invalidate();SetTimer(window,2,180,NULL);}return 0;
    case WM_TIMER:
        if(wparam==1){KillTimer(window,1);
#ifdef AIRLINK_UI_CHECK
            status(L"Oberflächenprüfung · keine Audioverbindung");
#else
            if(!start_host()){status(L"AirPlay-Hilfsprogramm fehlt oder konnte nicht starten.");update_controls();}
#endif
        }
        if(wparam==2){KillTimer(window,2);save_settings();if(app.wanted){char command[40];snprintf(command,sizeof(command),"VOLUME\t%d\n",app.volume);send_command(command);}}
        return 0;
    case WM_HOST_LINE:if(!app.exiting)host_line((char*)lparam);HeapFree(GetProcessHeap(),0,(void*)lparam);return 0;
    case WM_HOST_EXIT:if(!app.exiting){stop_route();app.host_ready=FALSE;app.scanning=FALSE;status(L"AirPlay-Hilfsprogramm beendet. AirLink bitte neu starten.");update_controls();}return 0;
    case WM_AUDIO_ERROR:if(app.wanted){stop_route();status(L"Audioaufnahme unterbrochen. Ausgabegerät prüfen und neu starten.");}return 0;
    case WM_COMMAND:
        switch(LOWORD(wparam)){
        case ID_THEME:app.dark=!app.dark;theme();save_settings();break;
        case ID_LANGUAGE:{
            app.english=!app.english;save_settings();update_controls();
            NOTIFYICONDATAW tray={0};tray.cbSize=sizeof(tray);tray.hWnd=app.window;tray.uID=1;tray.uFlags=NIF_TIP;
            lstrcpyW(tray.szTip,tr(L"AirLink – Systemaudio über AirPlay"));Shell_NotifyIconW(NIM_MODIFY,&tray);
            break;
        }
        case ID_CLOSE:SendMessageW(window,WM_CLOSE,0,0);break;
        case ID_DEVICE:device_menu();break;
        case ID_START:if(app.wanted)stop_route();else start_route();break;
        case ID_EXIT:DestroyWindow(window);break;
        case ID_OPEN:show_window();break;
        }return 0;
    case WM_TRAY:
        if(lparam==WM_LBUTTONUP||lparam==WM_LBUTTONDBLCLK)show_window();
        if(lparam==WM_RBUTTONUP||lparam==WM_CONTEXTMENU){POINT p;GetCursorPos(&p);HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,ID_OPEN,tr(L"AirLink öffnen"));if(app.wanted)AppendMenuW(menu,MF_STRING,ID_START,tr(L"Übertragung stoppen"));AppendMenuW(menu,MF_SEPARATOR,0,NULL);AppendMenuW(menu,MF_STRING,ID_EXIT,tr(L"Beenden"));SetForegroundWindow(window);TrackPopupMenu(menu,TPM_RIGHTBUTTON,p.x,p.y,0,window,NULL);DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);}return 0;
    case WM_CLOSE:if(app.tray_added)ShowWindow(window,SW_HIDE);else DestroyWindow(window);return 0;
    case WM_QUERYENDSESSION:audio_stop();return TRUE;
    case WM_ENDSESSION:if(wparam)DestroyWindow(window);return 0;
    case WM_DESTROY:shutdown_app();PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show){
    (void)previous;(void)command;(void)show;
    int argc=0;WCHAR **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv&&argc>1&&!wcscmp(argv[1],L"--mute-guard")){int result=audio_mute_guard(argc,argv);LocalFree(argv);return result;}
    if(argv)LocalFree(argv);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    app.dpi=(int)GetDpiForSystem();app.selected=-1;
    app.mutex=CreateMutexW(NULL,FALSE,
#ifdef AIRLINK_UI_CHECK
        L"Local\\AirLink.UICheck"
#else
        L"Local\\AirLink.SingleInstance"
#endif
    );
    if(GetLastError()==ERROR_ALREADY_EXISTS){HWND existing=FindWindowW(L"AirLink.Window",APP_NAME);if(existing){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}if(app.mutex)CloseHandle(app.mutex);return 0;}
    WCHAR root[MAX_PATH];if(!GetEnvironmentVariableW(L"LOCALAPPDATA",root,MAX_PATH))return 1;
    WCHAR folder[MAX_PATH];if(swprintf(folder,MAX_PATH,L"%ls\\AirLink",root)<0)return 1;CreateDirectoryW(folder,NULL);
    swprintf(app.config,MAX_PATH,L"%ls\\settings.ini",folder);swprintf(app.log_path,MAX_PATH,L"%ls\\transport.log",folder);
    app.dark=GetPrivateProfileIntW(L"Appearance",L"Dark",0,app.config)!=0;
    WCHAR language[8];GetPrivateProfileStringW(L"Appearance",L"Language",L"en",language,8,app.config);app.english=wcscmp(language,L"de")!=0;
    app.volume=(int)GetPrivateProfileIntW(L"Audio",L"Volume",40,app.config);if(app.volume<0||app.volume>100)app.volume=40;
    WCHAR last[80];GetPrivateProfileStringW(L"Audio",L"Device",L"",last,80,app.config);WideCharToMultiByte(CP_UTF8,0,last,-1,app.last_device,65,NULL,NULL);
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_BAR_CLASSES};InitCommonControlsEx(&controls);
    WNDCLASSW cls={0};cls.hInstance=instance;cls.lpfnWndProc=window_proc;cls.lpszClassName=L"AirLink.Window";cls.hCursor=LoadCursorW(NULL,IDC_ARROW);RegisterClassW(&cls);
    RECT work;SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    int x=work.left+(work.right-work.left-px(380))/2,y=work.top+(work.bottom-work.top-px(386))/2;
    HWND window=CreateWindowExW(WS_EX_APPWINDOW,cls.lpszClassName,APP_NAME,WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,x,y,px(380),px(386),NULL,NULL,instance,NULL);
    if(!window)return 1;
    int corner=2;DwmSetWindowAttribute(window,33,&corner,sizeof(corner));
    ShowWindow(window,SW_SHOWNORMAL);UpdateWindow(window);
    MSG msg;while(GetMessageW(&msg,NULL,0,0)>0){
        BOOL keyboard=msg.message==WM_KEYDOWN||msg.message==WM_SYSKEYDOWN;
        BOOL mouse=msg.message==WM_LBUTTONDOWN||msg.message==WM_RBUTTONDOWN||msg.message==WM_MBUTTONDOWN||msg.message==WM_POINTERDOWN;
        if((keyboard&&!app.keyboard_focus)||(mouse&&app.keyboard_focus)){
            app.keyboard_focus=keyboard;invalidate();
        }
        if(msg.message==WM_KEYDOWN&&msg.wParam=='Q'&&(GetKeyState(VK_CONTROL)&0x8000)){DestroyWindow(window);continue;}
        if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    if(app.background)DeleteObject(app.background);if(app.normal)DeleteObject(app.normal);if(app.small)DeleteObject(app.small);if(app.title)DeleteObject(app.title);if(app.symbols)DeleteObject(app.symbols);if(app.status_symbols)DeleteObject(app.status_symbols);if(app.icon)DestroyIcon(app.icon);if(app.mutex)CloseHandle(app.mutex);
    return 0;
}
