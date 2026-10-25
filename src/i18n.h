#ifndef AIRLINK_I18N_H
#define AIRLINK_I18N_H
#include <windows.h>
#include <wchar.h>

/* Canonical German messages stay intact so changing language also updates an
   already displayed status, including messages received from the transport. */
static const WCHAR *airlink_translate(const WCHAR *source,BOOL english){
    static const struct { const WCHAR *de,*en; } messages[]={
        {L"Übertragung starten",L"Start streaming"},
        {L"Übertragung stoppen",L"Stop streaming"},
        {L"Hell/Dunkel umschalten",L"Toggle light/dark mode"},
        {L"In den Tray minimieren",L"Minimize to tray"},
        {L"Lautsprecher auswählen",L"Select a speaker"},
        {L"Lautsprecher werden gesucht …",L"Searching for speakers …"},
        {L"L A U T S P R E C H E R",L"S P E A K E R"},
        {L"Lautstärke",L"Volume"},
        {L"Bereit zum Übertragen",L"Ready to stream"},
        {L"Systemaudio wird übertragen",L"Streaming system audio"},
        {L"Audioaufnahme wird vorbereitet …",L"Preparing audio capture …"},
        {L"Audioaufnahme fehlgeschlagen. Windows-Ausgabegerät prüfen.",L"Audio capture failed. Check the Windows output device."},
        {L"AirPlay-Hilfsprogramm antwortet nicht.",L"AirPlay helper is not responding."},
        {L"Verbindung wird hergestellt …",L"Connecting …"},
        {L"AirPlay-Lautsprecher werden gesucht …",L"Searching for AirPlay speakers …"},
        {L"Lautsprechersuche konnte nicht gestartet werden.",L"Could not start speaker discovery."},
        {L"Keine Lautsprecher gefunden. Gleiches Netzwerk prüfen.",L"No speakers found. Check that both devices share a network."},
        {L"Verbindung unterbrochen. Verbinde erneut …",L"Connection lost. Reconnecting …"},
        {L"AirPlay-Fehler. Bitte erneut versuchen.",L"AirPlay error. Please try again."},
        {L" (Kopplung erforderlich)",L" (pairing required)"},
        {L"Erneut suchen",L"Search again"},
        {L"Dieser Empfänger benötigt Kopplung; noch nicht unterstützt.",L"This receiver requires pairing, which is not yet supported."},
        {L"AirLink – Systemaudio über AirPlay",L"AirLink – System audio over AirPlay"},
        {L"AirPlay wird vorbereitet …",L"Preparing AirPlay …"},
        {L"Oberflächenprüfung · keine Audioverbindung",L"UI preview · no audio connection"},
        {L"AirPlay-Hilfsprogramm fehlt oder konnte nicht starten.",L"AirPlay helper is missing or could not start."},
        {L"AirPlay-Hilfsprogramm beendet. AirLink bitte neu starten.",L"AirPlay helper stopped. Please restart AirLink."},
        {L"Audioaufnahme unterbrochen. Ausgabegerät prüfen und neu starten.",L"Audio capture interrupted. Check the output device and start again."},
        {L"AirLink öffnen",L"Open AirLink"},
        {L"Beenden",L"Quit"},
        {L"Lautsprechersuche fehlgeschlagen. Netzwerk prüfen.",L"Speaker discovery failed. Check the network."},
        {L"Lautsprecher nicht mehr verfügbar. Bitte erneut suchen.",L"Speaker is no longer available. Please search again."},
        {L"Dieser Lautsprecher benötigt eine Kopplung. Diese Version unterstützt offene AirPlay-Empfänger.",L"This speaker requires pairing. This version supports open AirPlay receivers."},
        {L"Lautstärke konnte nicht geändert werden.",L"Could not change the volume."},
        {L"Ungültiger interner Befehl.",L"Invalid internal command."},
        {L"AirPlay-Befehl fehlgeschlagen.",L"AirPlay command failed."}
    };
    if(english)for(size_t i=0;i<sizeof(messages)/sizeof(messages[0]);i++)
        if(!wcscmp(source,messages[i].de))return messages[i].en;
    return source;
}
#endif
