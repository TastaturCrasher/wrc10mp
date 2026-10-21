; wrc10mp setup: installs winhttp.dll, SETTINGS.cfg (generated from the player's
; own DUMMYS.DAT) and wrc10mp.ini into the WRC 10 folder. The uninstaller removes
; exactly these files plus the mod's logs; no original game file is touched.
; Build: ISCC installer\wrc10mp.iss  ->  dist\wrc10mp-setup-<version>.exe

#define AppVersion "1.0.0"
#define GameDirName "WRC 10 FIA World Rally Championship"

[Setup]
AppId={{6F1C2B7E-5A0D-4C8B-9E3F-2D7A41B0C9E5}
AppName=wrc10mp (WRC 10 Multiplayer-Mod)
AppVersion={#AppVersion}
AppPublisher=wrc10mp
AppPublisherURL=https://github.com/TastaturCrasher/wrc10mp
DefaultDirName={code:DefaultGameDir}
AppendDefaultDirName=no
DirExistsWarning=no
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
UninstallFilesDir={app}\wrc10mp
UninstallDisplayName=wrc10mp (WRC 10 Multiplayer-Mod)
UninstallDisplayIcon={app}\WRC10.exe
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
OutputDir=..\dist
OutputBaseFilename=wrc10mp-setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[Messages]
de.SelectDirLabel3=Das Setup installiert die Mod in den Spielordner von WRC 10 (der Ordner mit WRC10.exe).
de.SelectDirBrowseLabel=Der Ordner wurde automatisch gesucht. Falls er nicht stimmt, "Durchsuchen" klicken.

[Files]
Source: "..\build\winhttp.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "ANLEITUNG.txt"; DestDir: "{app}\wrc10mp"; Flags: ignoreversion isreadme
Source: "make_settings.ps1"; Flags: dontcopy

[UninstallDelete]
Type: files; Name: "{app}\Common\Settings\SETTINGS.cfg"
Type: files; Name: "{app}\wrc10mp.ini"
Type: files; Name: "{app}\wrc10mp_*.log"
Type: files; Name: "{app}\wrc10mp_crash.dmp"
Type: dirifempty; Name: "{app}\wrc10mp"

[Code]
var
  ModePage: TInputOptionWizardPage;
  HostPage: TInputQueryWizardPage;

function GameDirIn(Lib: String): String;
begin
  Result := AddBackslash(Lib) + 'steamapps\common\{#GameDirName}';
  if not FileExists(Result + '\WRC10.exe') then
    Result := '';
end;

{ Steam folder plus every library listed in libraryfolders.vdf. }
function FindGameDir(): String;
var
  Steam, Line, Lib: String;
  Lines: TArrayOfString;
  I, P: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Steam) then
    if not RegQueryStringValue(HKLM32, 'Software\Valve\Steam', 'InstallPath', Steam) then
      Exit;
  StringChangeEx(Steam, '/', '\', True);
  Result := GameDirIn(Steam);
  if Result <> '' then
    Exit;
  if not LoadStringsFromFile(Steam + '\steamapps\libraryfolders.vdf', Lines) then
    Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if Pos('"path"', Line) <> 1 then
      Continue;
    Delete(Line, 1, 6);
    P := Pos('"', Line);
    if P = 0 then
      Continue;
    Delete(Line, 1, P);
    P := Pos('"', Line);
    if P = 0 then
      Continue;
    Lib := Copy(Line, 1, P - 1);
    StringChangeEx(Lib, '\\', '\', True);
    Result := GameDirIn(Lib);
    if Result <> '' then
      Exit;
  end;
end;

function DefaultGameDir(Param: String): String;
begin
  Result := FindGameDir();
  if Result = '' then
    Result := ExpandConstant('{commonpf32}\Steam\steamapps\common\{#GameDirName}');
end;

function GameRunning(): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq WRC10.exe" /NH | find /I "WRC10.exe" >nul',
    '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

function IsIPv4(S: String): Boolean;
var
  Part: String;
  Dots, P, N: Integer;
begin
  Result := False;
  Dots := 0;
  S := S + '.';
  while S <> '' do
  begin
    P := Pos('.', S);
    Part := Copy(S, 1, P - 1);
    Delete(S, 1, P);
    N := StrToIntDef(Part, -1);
    if (Part = '') or (Length(Part) > 3) or (N < 0) or (N > 255) then
      Exit;
    Dots := Dots + 1;
  end;
  Result := Dots = 4;
end;

function IsLanMode(): Boolean;
begin
  Result := ModePage.SelectedValueIndex = 1;
end;

procedure InitializeWizard();
begin
  ModePage := CreateInputOptionPage(wpSelectDir, 'Spielmodus', 'Wie wollt ihr spielen?',
    'Alle Spieler muessen denselben Modus installieren. Zum Wechseln das Setup einfach erneut ausfuehren.',
    True, False);
  ModePage.Add('Steam (empfohlen): Lobby ueber Steam, Beitritt ueber Einladung/Freundesliste, kein Tailscale noetig');
  ModePage.Add('Tailscale: LAN-Modus ueber ein gemeinsames Tailscale-Netz');
  { /MODE=steam|lan and /HOSTIP=x.x.x.x preset the pages, e.g. for /VERYSILENT installs. }
  if CompareText(ExpandConstant('{param:MODE|}'), 'lan') = 0 then
    ModePage.SelectedValueIndex := 1
  else if CompareText(ExpandConstant('{param:MODE|}'), 'steam') = 0 then
    ModePage.SelectedValueIndex := 0
  else
    ModePage.SelectedValueIndex := StrToIntDef(GetPreviousData('Mode', '0'), 0);

  HostPage := CreateInputQueryPage(ModePage.ID, 'Tailscale-Modus', 'Wer erstellt die Lobby?',
    'Wenn DU die Lobby erstellst (Host): Feld leer lassen.' + #13#10 +
    'Wenn du BEITRITTST: die Tailscale-IP des Hosts eingeben (100.x.y.z).');
  HostPage.Add('Tailscale-IP des Hosts:', False);
  HostPage.Values[0] := ExpandConstant('{param:HOSTIP|' + GetPreviousData('HostIp', '') + '}');
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Mode', IntToStr(ModePage.SelectedValueIndex));
  SetPreviousData(PreviousDataKey, 'HostIp', Trim(HostPage.Values[0]));
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = HostPage.ID) and not IsLanMode();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Ip: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    if not FileExists(AddBackslash(WizardDirValue()) + 'WRC10.exe') then
    begin
      MsgBox('In diesem Ordner liegt keine WRC10.exe. Bitte den Spielordner von WRC 10 waehlen' + #13#10 +
        '(Steam: Rechtsklick auf das Spiel > Verwalten > Lokale Dateien durchsuchen).', mbError, MB_OK);
      Result := False;
    end
    else if not FileExists(AddBackslash(WizardDirValue()) + 'Common\Settings\DUMMYS.DAT') then
    begin
      MsgBox('Common\Settings\DUMMYS.DAT fehlt im Spielordner. Bitte die Spieldateien in Steam ueberpruefen lassen.',
        mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = HostPage.ID then
  begin
    Ip := Trim(HostPage.Values[0]);
    if (Ip <> '') and not IsIPv4(Ip) then
    begin
      MsgBox('"' + Ip + '" ist keine gueltige IP-Adresse.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Mode: String;
begin
  if not IsLanMode() then
    Mode := 'Steam'
  else if Trim(HostPage.Values[0]) = '' then
    Mode := 'Tailscale, du bist HOST'
  else
    Mode := 'Tailscale, BEITRETEN zu ' + Trim(HostPage.Values[0]);
  Result := MemoDirInfo + NewLine + NewLine + 'Modus:' + NewLine + Space + Mode;
end;

{ Generate SETTINGS.cfg before anything is copied, so a failure leaves the game untouched. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
  Mode, Params: String;
begin
  Result := '';
  if GameRunning() then
  begin
    Result := 'WRC 10 laeuft noch. Bitte das Spiel komplett beenden und dann "Zurueck" und erneut "Weiter" klicken.';
    Exit;
  end;
  ExtractTemporaryFile('make_settings.ps1');
  if IsLanMode() then Mode := 'lan' else Mode := 'steam';
  Params := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\make_settings.ps1') + '"' +
    ' -Dummys "' + AddBackslash(WizardDirValue()) + 'Common\Settings\DUMMYS.DAT"' +
    ' -Mode ' + Mode + ' -Out "' + ExpandConstant('{tmp}\SETTINGS.cfg') + '"';
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Params, '', SW_HIDE,
    ewWaitUntilTerminated, Code) or (Code <> 0) or not FileExists(ExpandConstant('{tmp}\SETTINGS.cfg')) then
    Result := 'SETTINGS.cfg konnte nicht aus DUMMYS.DAT erzeugt werden (Fehlercode ' + IntToStr(Code) + ').' + #13#10 +
      'Moeglicherweise eine andere Spielversion. Bitte die Spieldateien in Steam ueberpruefen lassen.';
end;

function IniText(): String;
var
  HostIp: String;
begin
  HostIp := '';
  if IsLanMode() then
    HostIp := Trim(HostPage.Values[0]);
  Result :=
    '; wrc10mp - erzeugt vom Setup. Zum Aendern das Setup erneut ausfuehren.' + #13#10 +
    '[general]' + #13#10 +
    '; HTTP-Mitschnitt zum Kylotonn-Server (enthaelt Login-Tokens), aus' + #13#10 +
    'log=0' + #13#10 +
    '' + #13#10 +
    '[lan]' + #13#10 +
    '; Tailscale-IP des Hosts. Leer = du bist Host.' + #13#10 +
    'host_ip=' + HostIp + #13#10 +
    'port=60000' + #13#10 +
    '; Jede Lobby-Antwort zusaetzlich mit/ohne Passwort "KyloPwd" zustellen' + #13#10 +
    'pong_variants=1' + #13#10 +
    '; Tailscale-Adressen als LAN-Adressen tarnen (nicht noetig)' + #13#10 +
    'alias=0' + #13#10 +
    '; Such- und Verbindungspakete nach wrc10mp_net.log schreiben' + #13#10 +
    'log=1' + #13#10 +
    '' + #13#10 +
    '[steam]' + #13#10 +
    '; Lobby-Parameter "Alle" (-1) beim Zuruecklesen aus Steam reparieren' + #13#10 +
    'atoi_wrap=1' + #13#10 +
    '; Zwangspause der Engine ueberwachen (wrc10mp_pause.log) ...' + #13#10 +
    'pause_watch=1' + #13#10 +
    '; ... und aufheben (Steam-Overlay haelt sonst Pausenmenue und Rennuhr fest)' + #13#10 +
    'clear_forced_pause=1' + #13#10;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  SettingsDir: String;
begin
  if CurStep = ssInstall then
  begin
    { Leftovers of manual installs that the game would ignore or misread. }
    SettingsDir := ExpandConstant('{app}\Common\Settings\');
    DeleteFile(SettingsDir + 'SETTINGS_steam.cfg');
    DeleteFile(ExpandConstant('{app}\Settings\SETTINGS.cfg'));
  end
  else if CurStep = ssPostInstall then
  begin
    SettingsDir := ExpandConstant('{app}\Common\Settings\');
    if not FileCopy(ExpandConstant('{tmp}\SETTINGS.cfg'), SettingsDir + 'SETTINGS.cfg', False) then
      MsgBox('SETTINGS.cfg konnte nicht nach ' + SettingsDir + ' kopiert werden.', mbError, MB_OK);
    if not SaveStringToFile(ExpandConstant('{app}\wrc10mp.ini'), IniText(), False) then
      MsgBox('wrc10mp.ini konnte nicht geschrieben werden.', mbError, MB_OK);
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  while GameRunning() do
    if MsgBox('WRC 10 laeuft noch. Bitte das Spiel beenden und dann OK klicken.', mbError, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;
