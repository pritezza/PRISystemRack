; PRI System Rack のインストーラー（Inno Setup 6）。Tools/Scripts/build-installer.ps1 から作る（直接コンパイルするときは下の定義を渡す）。
;
; インストール:
;   1. 動いている本体を終了させる（PRISystemRackSetup.exe quit-app "{app}\PRISystemRack.exe"。× では通知領域にしまうだけなので、
;      インストール先の本体を --quit 付きで起動して頼む。インストール先にまだ本体がなければ、利用者に終了してもらう）
;   2. ファイルを {app}（C:\Program Files\PRISystemRack）にコピーする。APO の DLL は中身が変わったときだけ置き換え、
;      すでに入っていた DLL を置き換えるときだけ、その直前に Windows Audio を止める（audiodg.exe が読み込んでいて置き換えられないため）
;   3. APO を登録する（register: ログフォルダーの権限、DisableProtectedAudioDG、DllRegisterServer）。止めていたら Windows Audio を動かす
;   再生デバイスへの付け外しはしない（アプリでデバイスを選んで［有効］を押す。今まで有効だったデバイスはそのまま有効）
;
; アンインストール:
;   本体を終了させ、uninstall（すべてのデバイスを元に戻す・登録を消す・Windows Audio を再起動）を行ってからファイルを消す。
;   最後に、設定・プリセット・ログも消すかを聞く（既定は残す）。Windows の起動時の登録は、この場所の exe を指していれば消す

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppDir
  #define AppDir "..\build\PRISystemRack_artefacts\Release"
#endif
#ifndef ApoDir
  #define ApoDir "..\build\bin\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{1CC3BBF8-C626-4862-B15A-4FE2A8CFC369}
AppName=PRI System Rack
AppVersion={#AppVersion}
AppVerName=PRI System Rack {#AppVersion}
AppPublisher=PRISystemRack
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\PRISystemRack
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=PRISystemRack-Setup-{#AppVersion}
#ifdef IconFile
SetupIconFile={#IconFile}
#endif
UninstallDisplayIcon={app}\PRISystemRack.exe
UninstallDisplayName=PRI System Rack
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 本体は × では終わらない（通知領域にしまうだけ）ので、Windows の再起動マネージャーには任せず quit-app で終了させる
CloseApplications=no
RestartApplications=no
UsedUserAreasWarning=no
; 言語の選択の画面は出さず、Windows の表示言語に合う言語（日本語の Windows なら日本語）を使う
ShowLanguageDialog=auto

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"

[CustomMessages]
en.AppDidNotQuit=PRI System Rack did not quit. Quit it from the notification area icon (right-click > Quit) and run the installer again.
ja.AppDidNotQuit=PRI System Rack が終了しませんでした。通知領域のアイコン（右クリック →［終了］）から終了してから、もう一度インストーラーを実行してください。
en.RegisterFailed=The audio component (APO) could not be registered. For details, see %1.
ja.RegisterFailed=音声の部品（APO）を登録できませんでした。詳しくは %1 を見てください。
en.RestoreFailed=Some playback devices could not be restored to their previous state. For details, see %1.
ja.RestoreFailed=一部の再生デバイスを元の状態に戻せませんでした。詳しくは %1 を見てください。
en.UninstallAppRunning=PRI System Rack could not be closed (it may be running for another Windows user). Uninstall anyway?%n%nFiles that are in use are removed after Windows restarts.
ja.UninstallAppRunning=PRI System Rack を終了できませんでした（ほかの Windows ユーザーが使っている可能性があります）。このままアンインストールしますか？%n%n使用中のファイルは、Windows を再起動したあとに削除されます。
en.DeleteUserData=Also delete the settings, presets and logs?%n%n%1%n%2
ja.DeleteUserData=設定・プリセット・ログも削除しますか？%n%n%1%n%2
en.NotesTitle=Notes:
ja.NotesTitle=ご注意:
en.NoteUnsigned=Windows is set to load audio components (APOs) that are not signed (DisableProtectedAudioDG). This setting is kept after uninstalling, because other audio tools may rely on it.
ja.NoteUnsigned=Windows が署名のない音声の部品（APO）を読み込めるように設定します（DisableProtectedAudioDG）。ほかの音声ツールが使っていることがあるため、アンインストールしても戻しません。
en.NoteAudioStops=When the audio component is updated, Windows sound stops for a few seconds.
ja.NoteAudioStops=音声の部品を更新するときは、Windows の音が数秒止まります。
en.NoteEnable=To process a device's sound, choose the playback device in the app and click [Enabled].
ja.NoteEnable=再生デバイスの音を処理するには、アプリで再生デバイスを選び［有効］を押します。

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#AppDir}\PRISystemRack.exe"; DestDir: "{app}"; Flags: ignoreversion
; 落ちたときの記録（CrashLog）に関数名と行を出すのに使う
Source: "{#AppDir}\PRISystemRack.pdb"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#AppDir}\PRISystemRackScanner.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppDir}\PRISystemRackSetup.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ApoDir}\PRISystemRackAPO.dll"; DestDir: "{app}"; Flags: ignoreversion; Check: ApoNeedsUpdate; BeforeInstall: StopAudioForApo
Source: "{#ApoDir}\PRISystemRackAPO.pdb"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Check: ApoNeedsUpdate

[Registry]
; インストーラーの言語（en / ja）。アプリは、まだ言語を選んだことがないとき（設定がないとき）だけ、これを最初の言語にする。
; HKLM\SOFTWARE\PRISystemRack はアンインストールの uninstall で消える
Root: HKLM; Subkey: "SOFTWARE\PRISystemRack"; ValueType: string; ValueName: "InstallLanguage"; ValueData: "{language}"

[Icons]
Name: "{autoprograms}\PRI System Rack"; Filename: "{app}\PRISystemRack.exe"
Name: "{autodesktop}\PRI System Rack"; Filename: "{app}\PRISystemRack.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PRISystemRack.exe"; Description: "{cm:LaunchProgram,PRI System Rack}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
var
  ApoChecked: Boolean;
  ApoChanged: Boolean;
  AudioStopped: Boolean;

function SetupLogPath: String;
begin
  Result := ExpandConstant('{commonappdata}\PRISystemRack\Logs\setup.log');
end;

// PRISystemRackSetup.exe を実行して終了コードを返す（起動できなければ -1）。内容は Inno Setup のログ（/LOG）にも残す
function RunSetupTool(const ToolPath, Params: String): Integer;
var
  Code: Integer;
begin
  if not Exec(ToolPath, Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  Log(Format('%s %s -> %d', [ToolPath, Params, Code]));
  Result := Code;
end;

function TempSetupTool: String;
begin
  Result := ExpandConstant('{tmp}\PRISystemRackSetup.exe');
end;

// ファイルをコピーする前に、動いている本体を終了させる
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  ExtractTemporaryFile('PRISystemRackSetup.exe');
  // --quit を送るのに起動するのは、インストール先（管理者しか書けない場所）の本体だけ。初めて入れるときにほかの場所の本体が
  // 動いていれば頼めないので、利用者に終了してもらう（AppDidNotQuit）
  if RunSetupTool(TempSetupTool, 'quit-app "' + ExpandConstant('{app}\PRISystemRack.exe') + '"') <> 0 then
    Result := CustomMessage('AppDidNotQuit');
end;

// APO の DLL を置き換える必要があるか（入っていない、または中身が違う）
function ApoNeedsUpdate: Boolean;
var
  Installed: String;
begin
  if not ApoChecked then
  begin
    ApoChecked := True;
    Installed := ExpandConstant('{app}\PRISystemRackAPO.dll');
    ExtractTemporaryFile('PRISystemRackAPO.dll');
    ApoChanged := (not FileExists(Installed))
                  or (GetSHA256OfFile(Installed) <> GetSHA256OfFile(ExpandConstant('{tmp}\PRISystemRackAPO.dll')));
    Log(Format('APO DLL needs update: %d', [Ord(ApoChanged)]));
  end;
  Result := ApoChanged;
end;

// すでに入っている DLL を置き換える直前に Windows Audio を止める（audiodg.exe が読み込んでいると置き換えられない）
procedure StopAudioForApo;
begin
  if FileExists(ExpandConstant('{app}\PRISystemRackAPO.dll')) then
  begin
    AudioStopped := True; // 失敗しても、最後に動かすのは試す
    RunSetupTool(TempSetupTool, 'stop-audio');
  end;
end;

procedure StartAudioIfStopped(const ToolPath: String);
begin
  if AudioStopped then
  begin
    if RunSetupTool(ToolPath, 'start-audio') = 0 then
      AudioStopped := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Tool: String;
  RegisterResult: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    Tool := ExpandConstant('{app}\PRISystemRackSetup.exe');
    RegisterResult := RunSetupTool(Tool, 'register "' + ExpandConstant('{app}\PRISystemRackAPO.dll') + '"');
    // 失敗を知らせる前に音を戻す（知らせを閉じるまで音が止まったままにしない）
    StartAudioIfStopped(Tool);
    if RegisterResult <> 0 then
      SuppressibleMsgBox(FmtMessage(CustomMessage('RegisterFailed'), [SetupLogPath]), mbError, MB_OK, IDOK);
  end;
end;

// 途中で失敗・取りやめても、止めた Windows Audio を動かしておく（Setup の start-audio なら、一緒に止めた依存するサービスも戻す。
// Setup がまだないときは sc で Windows Audio だけ動かす）
procedure DeinitializeSetup;
var
  Code: Integer;
begin
  if AudioStopped then
  begin
    if FileExists(ExpandConstant('{app}\PRISystemRackSetup.exe')) then
      StartAudioIfStopped(ExpandConstant('{app}\PRISystemRackSetup.exe'));
    if AudioStopped then
      Exec(ExpandConstant('{sys}\sc.exe'), 'start Audiosrv', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo;
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
  Result := Result + NewLine + NewLine + CustomMessage('NotesTitle')
            + NewLine + Space + CustomMessage('NoteEnable')
            + NewLine + Space + CustomMessage('NoteAudioStops')
            + NewLine + Space + CustomMessage('NoteUnsigned');
end;

// アンインストールを始める前に本体を終了させる。終わらなければ（ほかの Windows ユーザーが動かしているなど）、続けるかを聞く
// （続けると、動いている本体の exe は消せずに残り、Windows の再起動のあとに消える）
function InitializeUninstall: Boolean;
begin
  Result := True;
  if RunSetupTool(ExpandConstant('{app}\PRISystemRackSetup.exe'), 'quit-app') <> 0 then
    Result := UninstallSilent
              or (MsgBox(CustomMessage('UninstallAppRunning'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Tool, RunValue, UserData, CommonData: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    // ファイルを消す前に、デバイスを元に戻して APO の登録を消す（Windows Audio を再起動して DLL を手放させる。本体は InitializeUninstall で終了させた）
    Tool := ExpandConstant('{app}\PRISystemRackSetup.exe');
    if RunSetupTool(Tool, 'uninstall') <> 0 then
      SuppressibleMsgBox(FmtMessage(CustomMessage('RestoreFailed'), [SetupLogPath]), mbError, MB_OK, IDOK);
  end;

  if CurUninstallStep = usPostUninstall then
  begin
    // Windows の起動時に起動する登録（この場所の exe を指しているときだけ）
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'PRISystemRack', RunValue)
       and (Pos(Lowercase(ExpandConstant('{app}\PRISystemRack.exe')), Lowercase(RunValue)) > 0) then
      RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'PRISystemRack');

    // 注意: 行の先頭を [ にしない（Inno Setup がセクションの見出しと見なす）
    UserData := ExpandConstant('{userappdata}\PRISystemRack');
    CommonData := ExpandConstant('{commonappdata}\PRISystemRack');
    if (not UninstallSilent)
       and (MsgBox(FmtMessage(CustomMessage('DeleteUserData'), [UserData, CommonData]), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
    begin
      DelTree(UserData, True, True, True);
      DelTree(CommonData, True, True, True);
    end;
  end;
end;
