# wrc10mp – Multiplayer für WRC 10 (Steam) ohne Kylotonn-Server

Der Online-Modus von WRC 10 hing an Kylotonns dedizierten Servern, und die sind abgeschaltet. Das Spiel enthält aber einen vollständigen, abgeschalteten **LAN-Modus** über RakNet. Diese Mod schaltet ihn ein und bringt ihn über Tailscale (oder ein anderes VPN) ins Internet. Die EXE und die Originaldateien bleiben unverändert. Alles läuft über zusätzliche Dateien, die man löschen kann.

## So funktioniert es

1. **`SETTINGS.cfg`:** Das Spiel lädt `Common\Settings\SETTINGS.cfg` (Klartext-Lua) bevorzugt vor der verschleierten `DUMMYS.DAT`. Unsere Datei ist eine Kopie davon mit `WRC.UseLanNetwork = true`.
2. **LAN-Lobby:** Der Host lauscht auf **UDP 60000** (bei belegtem Port 60001 usw.). Die Suche schickt RakNet-Broadcast-Pings an 60000–60015 und verbindet sich mit dem ersten Host, dessen Antwort passt.
3. **`winhttp.dll`:** Wird vom Spiel geladen und hakt sich in die Importtabelle für `sendto`/`recvfrom` ein.
   - Jeden Such-Broadcast schickt sie zusätzlich per Unicast an die Host-IP aus `wrc10mp.ini` (`[lan] host_ip`).
   - Jede Lobby-Antwort stellt sie dem Spiel **zweimal** zu: einmal original, einmal mit bzw. ohne das Passwort `KyloPwd` im Lobbynamen. Grund: Die Suche nimmt nur Lobbys an, deren Name genau dem eigenen entspricht. Das eigene Passwort hängt aber davon ab, ob man in dieser Spielsitzung schon einmal selbst eine (private) Lobby erstellt hat. Ein frisch gestartetes Spiel hat deshalb jede private Lobby verworfen. Das war der Fehler, an dem der Beitritt über das Internet scheiterte.
   - Such- und Verbindungspakete landen in `wrc10mp_net.log`.


## Zwei Modi

| Modus | Datei | Lobby | Spieldaten | Status |
|---|---|---|---|---|
| **Steam** | `SETTINGS_steam.cfg` (wird als `SETTINGS.cfg` installiert) | Steam-Lobby, Beitritt über die Steam-Freundesliste | Steam-P2P (über Valves Relays) | **empfohlen**, mit 2 Spielern getestet (Etappen gefahren und gewertet) |
| **Tailscale** | `SETTINGS.cfg` | LAN-Suche, per DLL an die Host-IP gespiegelt | direkt UDP über Tailscale | mit 2 Spielern getestet, Verbindung hält |

Der Steam-Modus nutzt einen Netzwerkpfad, der fertig in der EXE steckt: RakNet mit „Lobby2 Steam“. Das Spiel wählt ihn, wenn weder `WRC.UseLanNetwork` noch `Engine.UseKtOSMultiplayer` gesetzt ist. Standardmäßig ist `UseKtOSMultiplayer` an, dann landet das Spiel bei den abgeschalteten Kylotonn-Servern. `SETTINGS_steam.cfg` setzt es auf `false`. Tailscale und Portfreigaben braucht es dafür nicht.

Der Steam-Pfad wurde nie ausgeliefert und hat Fehler. Bekannt und in der DLL behoben: Lobby-Parameter mit dem Wert „Alle“ (-1) gehen bei der Rundreise durch die Steam-Lobby-Daten kaputt („%u“ geschrieben, mit `atoi` gelesen), und das Spiel stürzt beim Erstellen der Lobby ab. Die DLL korrigiert das Zurücklesen (`[steam] atoi_wrap=1`). Außerdem schreibt sie bei Abstürzen `wrc10mp_crash.log` und `wrc10mp_crash.dmp` (`[general] crash_log=1`).

## Installation

1. Unter [Releases](../../releases) die neueste `wrc10mp-setup-<version>.exe` herunterladen.
2. WRC 10 beenden und das Setup ausführen. Windows SmartScreen warnt eventuell, weil das Setup nicht signiert ist: „Weitere Informationen“ → „Trotzdem ausführen“.
3. Das Setup findet den Spielordner über die Steam-Bibliotheken und fragt nach dem Modus (Steam oder Tailscale), im Tailscale-Modus zusätzlich nach der Host-IP (leer lassen, wer hostet). **Alle Spieler müssen denselben Modus wählen.**
4. Modus oder Rolle wechseln: Setup erneut ausführen.

Das Setup installiert `winhttp.dll` und `wrc10mp.ini` in den Spielordner und erzeugt `Common\Settings\SETTINGS.cfg` aus der `DUMMYS.DAT` des eigenen Spiels. Spieldateien werden nicht verändert und nicht mitgeliefert.

**Deinstallation:** Windows-Einstellungen → Apps → „wrc10mp (WRC 10 Multiplayer-Mod)“ → Deinstallieren. Das entfernt alle Dateien der Mod samt Logs.

Stille Installation (z. B. für Tests): `wrc10mp-setup-<version>.exe /VERYSILENT /MODE=steam` bzw. `/MODE=lan /HOSTIP=100.x.y.z`, optional `/DIR="<Spielordner>"` und `/CURRENTUSER` (ohne Admin-Rechte, wenn der Spielordner beschreibbar ist).

## Spielen im Steam-Modus

- Der Host öffnet den Online-Mehrspieler und erstellt eine (private) Lobby.
- Die anderen nehmen eine Einladung an (Steam-Overlay, Umschalt+Tab) oder treten über die Steam-Freundesliste bei („Spiel beitreten“ beim Host). Einen Einladen-Knopf im Spiel gibt es nicht.
- Das Steam-Overlay setzt beim Beitretenden eine Zwangspause, die das Spiel online nie aufhebt. Die DLL hebt sie auf (`[steam] clear_forced_pause=1`). Nebenwirkung: Fokusverlust pausiert das Spiel auch im Einzelspieler nicht mehr automatisch.
- Bei Problemen ins `WRC10.log` und die `wrc10mp_*.log` im Spielordner schauen.

## Spielen im Tailscale-Modus

**Host:**
- Startet WRC 10 und erstellt eine Lobby.
- Muss in der Windows-Firewall eingehendes UDP für `WRC10.exe` erlauben. Meist fragt Windows beim ersten Mal nach.
- Teilt seine Tailscale-IP mit.

**Beitretende:**
- WRC 10 starten, „Lobby suchen“, in „Lobby-Einstellungen“ alles auf „Alle“ lassen, „Bestätigen“.
- Kommt „Kein Spiel gefunden“: mit `lobby_testen.bat` prüfen, ob die Lobby des Hosts über Tailscale antwortet, und `wrc10mp_net.log` ansehen.

## wrc10mp.ini

```ini
[general]
log=0              ; HTTP-Mitschnitt (enthält Login-Tokens), aus
[lan]
host_ip=           ; Tailscale-IP des Hosts, leer = selbst Host
port=60000
pong_variants=1    ; Lobby-Antworten mit/ohne Passwort zustellen (der eigentliche Fix)
alias=0            ; Tailscale-Adressen als LAN-Adressen tarnen, nicht nötig
log=1              ; wrc10mp_net.log
```

## Werkzeuge

| Datei | Zweck |
|---|---|
| `build.bat` | baut `build\winhttp.dll` (MSVC 2022 Build Tools, x64) |
| `installer/wrc10mp.iss` | Inno-Setup-Skript: `ISCC installer\wrc10mp.iss` erzeugt `dist\wrc10mp-setup-<version>.exe` |
| `installer/make_settings.ps1` | erzeugt beim Installieren `SETTINGS.cfg` aus `DUMMYS.DAT` (wie `tools/make_settings.py`) |
| `tools/make_settings.py` | erzeugt `settings/SETTINGS.cfg` aus `DUMMYS.DAT` |
| `tools/dummydat.py` | ent- und verschlüsselt `DUMMY*.DAT` (32-Byte-XOR) |
| `tools/raknet_ping.py` | prüft, ob eine Lobby antwortet (`raknet_ping.py <ip>`) |
| `tools/fake_host.py` | Attrappe einer Lobby zum Testen der Suche |

Neu erzeugen der `SETTINGS.cfg` (z. B. nach einem Spiel-Update):

```bash
python tools/make_settings.py "<Spielordner>/Common/Settings/DUMMYS.DAT"
```

## Bekannte Grenzen / offen

- Beide Modi sind mit zwei Spielern getestet. Im Steam-Modus sieht der Host gelegentlich ein kurzes Standbild (ca. 0,5 s), Ursache noch offen.
- Bei 3+ Spielern verbinden sich die Clients laut Code zusätzlich direkt miteinander (Adressliste aus einem RakNet-Plugin, vermutlich FullyConnectedMesh2). Über Tailscale sollte das funktionieren, weil jeder jeden per IP erreicht. Im Steam-Modus läuft es über Steam-P2P. Getestet ist beides noch nicht.
- Online-Leaderboards, Clubs usw. laufen weiter über Kylotonn, solange deren Server antworten.
