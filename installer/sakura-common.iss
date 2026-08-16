#if VER < EncodeVer(6,7,0)
  #error Inno Setup 6.7.0 or newer is required to compile the Sakura Editor NEXT installer
#endif

#define MySendTo "{usersendto}"
#define MyAppVer GetVersionNumbersString("sakura\sakura.exe")
#define MyAppVerH StringChange(MyAppVer, ".", "-")

[Setup]
ArchitecturesInstallIn64BitMode={#MyArchitecture}
ArchitecturesAllowed={#MyArchitecture}
AppName={cm:AppName}
AppId=sakura editor
AppVersion={#MyAppVer}
AppVerName={cm:AppVerName} {#MyAppVer} ({#MyArchitecture})
AppPublisher={cm:AppPublisher}
AppPublisherURL=https://github.com/tsuyoshi-otake/sakura-editor-next
AppSupportURL=https://github.com/tsuyoshi-otake/sakura-editor-next/issues
AppUpdatesURL=https://github.com/tsuyoshi-otake/sakura-editor-next/releases
DefaultDirName={autopf}\Sakura Editor NEXT
DefaultGroupName={cm:AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\sakura.exe
InfoBeforeFile="instmaterials\info.txt"
LanguageDetectionMethod=uilanguage 
ShowLanguageDialog=no
SolidCompression=yes

; Sakura Editor NEXT brand theme: a charcoal surface rather than pure black.
; Inno Setup automatically disables custom styling for Windows high-contrast themes.
WizardStyle=modern dark includetitlebar hidebevels
WizardImageFile="instmaterials\sakura-installer-image.png"
WizardSmallImageFile="..\src\main\resources\images\sakura_editor_next.png"
SetupIconFile="instmaterials\icon_debug.ico"
DisableStartupPrompt=yes
DisableWelcomePage=no

; Install for the current user only.  Do not expose an elevation or
; all-users override: this package must remain usable without admin rights.
PrivilegesRequired=lowest

; 出力先ディレクトリ
OutputDir=Output-{#OutputSuffix}

; エディタのバージョンに応じて書き換える場所
OutputBaseFilename=sakura_install{#MyAppVerH}-{#MyArchitecture}
VersionInfoVersion={#MyAppVer}
VersionInfoProductVersion={#MyAppVer}

; OSバージョン制限(Windows 11 build 22000 以降に対応)
MinVersion=10.0.22000

[Languages]
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "instmaterials\info_us.txt"
Name: "zh_hans"; MessagesFile: "Languages\ChineseSimplified.isl"; InfoBeforeFile: "instmaterials\info_zh_hans.txt"
Name: "zh_hant"; MessagesFile: "Languages\ChineseTraditional.isl"; InfoBeforeFile: "instmaterials\info_zh_hant_utf8.txt"

[Messages]
ja.WelcomeLabel1=Sakura Editor NEXT のセットアップ
ja.WelcomeLabel2=VS Code級の機能を、ネイティブの速さで。%n%nセットアップを始める前に、作業中のファイルを保存し、起動中の Sakura Editor NEXT を終了してください。
ja.WizardInfoBefore=インストール前の確認
ja.InfoBeforeLabel=Sakura Editor NEXT をインストールする前にご確認ください。
ja.InfoBeforeClickLabel=内容を確認したら、「次へ」を選択してください。
ja.WizardSelectDir=インストール先
ja.SelectDirDesc=Sakura Editor NEXT のインストール先を確認してください。
ja.SelectDirLabel3=標準の保存先を使用するか、任意のフォルダーを指定できます。
ja.SelectDirBrowseLabel=変更する場合は「参照」を選択してください。
ja.WizardSelectTasks=追加オプション
ja.SelectTasksDesc=Windows との連携方法を選択してください。
ja.SelectTasksLabel2=必要な項目だけを選択できます。後から設定を変更することもできます。
ja.WizardReady=インストール内容の確認
ja.ReadyLabel1=次の内容で Sakura Editor NEXT をインストールします。
ja.ReadyLabel2a=内容を確認して「インストール」を選択してください。変更する場合は「戻る」を選択します。
ja.ReadyLabel2b=内容を確認して「インストール」を選択してください。
ja.FinishedHeadingLabel=Sakura Editor NEXT の準備ができました
ja.FinishedLabel=Sakura Editor NEXT のインストールが完了しました。
ja.FinishedLabelNoIcons=Sakura Editor NEXT のインストールが完了しました。
ja.ClickFinish=「完了」を選択してセットアップを終了してください。
ja.ButtonNext=次へ(&N)

en.WelcomeLabel1=Set up Sakura Editor NEXT
en.WelcomeLabel2=VS Code-class features at native speed.%n%nSave your work and close any running Sakura Editor NEXT windows before continuing.
en.WizardInfoBefore=Before you install
en.InfoBeforeLabel=Review these details before installing Sakura Editor NEXT.
en.InfoBeforeClickLabel=When you are ready, select Next.
en.WizardSelectDir=Install location
en.SelectDirDesc=Confirm where Sakura Editor NEXT will be installed.
en.SelectDirLabel3=Use the recommended location or choose a custom folder.
en.SelectDirBrowseLabel=To change the location, select Browse.
en.WizardSelectTasks=Additional options
en.SelectTasksDesc=Choose how Sakura Editor NEXT integrates with Windows.
en.SelectTasksLabel2=Select only the options you need. You can change them later.
en.WizardReady=Review installation
en.ReadyLabel1=Sakura Editor NEXT is ready to install with these settings.
en.ReadyLabel2a=Select Install to continue, or Back to make changes.
en.ReadyLabel2b=Select Install to continue.
en.FinishedHeadingLabel=Sakura Editor NEXT is ready
en.FinishedLabel=Sakura Editor NEXT was installed successfully.
en.FinishedLabelNoIcons=Sakura Editor NEXT was installed successfully.
en.ClickFinish=Select Finish to close Setup.

zh_hans.WizardInfoBefore=安装前确认
zh_hans.InfoBeforeLabel=安装 Sakura Editor NEXT 前请确认以下内容。
zh_hans.InfoBeforeClickLabel=确认后请选择“下一步”。
zh_hans.WizardSelectDir=安装位置
zh_hans.SelectDirDesc=确认 Sakura Editor NEXT 的安装位置。
zh_hans.SelectDirLabel3=使用推荐位置，或指定自定义文件夹。
zh_hans.SelectDirBrowseLabel=如需更改位置，请选择“浏览”。
zh_hans.WizardSelectTasks=附加选项
zh_hans.SelectTasksDesc=选择 Sakura Editor NEXT 与 Windows 的集成方式。
zh_hans.FinishedHeadingLabel=Sakura Editor NEXT 已准备就绪
zh_hans.FinishedLabel=Sakura Editor NEXT 已成功安装。

zh_hant.WizardInfoBefore=安裝前確認
zh_hant.InfoBeforeLabel=安裝 Sakura Editor NEXT 前請確認以下內容。
zh_hant.InfoBeforeClickLabel=確認後請選擇「下一步」。
zh_hant.WizardSelectDir=安裝位置
zh_hant.SelectDirDesc=確認 Sakura Editor NEXT 的安裝位置。
zh_hant.SelectDirLabel3=使用建議位置，或指定自訂資料夾。
zh_hant.SelectDirBrowseLabel=如需變更位置，請選擇「瀏覽」。
zh_hant.WizardSelectTasks=附加選項
zh_hant.SelectTasksDesc=選擇 Sakura Editor NEXT 與 Windows 的整合方式。
zh_hant.FinishedHeadingLabel=Sakura Editor NEXT 已準備就緒
zh_hant.FinishedLabel=Sakura Editor NEXT 已成功安裝。

[CustomMessages]
en.AppName=Sakura Editor NEXT
ja.AppName=Sakura Editor NEXT
zh_hans.AppName=Sakura Editor NEXT
zh_hant.AppName=Sakura Editor NEXT

en.AppVerName=Sakura Editor NEXT
ja.AppVerName=Sakura Editor NEXT
zh_hans.AppVerName=Sakura Editor NEXT
zh_hant.AppVerName=Sakura Editor NEXT

en.AppPublisher=Sakura Editor NEXT developers
ja.AppPublisher=Sakura Editor NEXT 開発チーム
zh_hans.AppPublisher=Sakura Editor NEXT 开发团队
zh_hant.AppPublisher=Sakura Editor NEXT 開發團隊

en.ExitSetupPrompt=Setup is not complete. Exiting now will cancel the installation.%n%nExit Setup?
ja.ExitSetupPrompt=セットアップは完了していません。%n終了するとインストールされません。%n%nセットアップを終了しますか？
zh_hans.ExitSetupPrompt=安装尚未完成。现在退出将取消安装。%n%n要退出安装程序吗？
zh_hant.ExitSetupPrompt=安裝尚未完成。現在結束將取消安裝。%n%n要結束安裝程式嗎？

en.RunningAppTitle=Close Sakura Editor NEXT
ja.RunningAppTitle=Sakura Editor NEXT を終了してください
zh_hans.RunningAppTitle=请关闭 Sakura Editor NEXT
zh_hant.RunningAppTitle=請關閉 Sakura Editor NEXT

en.RunningAppPrompt=Sakura Editor NEXT is currently running.%nSave your work and close it, then select Check Again.
ja.RunningAppPrompt=Sakura Editor NEXT が実行中です。%n作業内容を保存して終了し、「再確認」を選択してください。
zh_hans.RunningAppPrompt=Sakura Editor NEXT 正在运行。%n请保存工作并关闭应用，然后选择“重新检查”。
zh_hant.RunningAppPrompt=Sakura Editor NEXT 正在執行。%n請儲存工作並關閉應用程式，然後選擇「重新檢查」。

en.RunningAppRetry=Check Again
ja.RunningAppRetry=再確認
zh_hans.RunningAppRetry=重新检查
zh_hant.RunningAppRetry=重新檢查

en.TypesAll=Recommended
ja.TypesAll=推奨
zh_hans.TypesAll=推荐
zh_hant.TypesAll=建議

en.TypesEditorWithHelp=Editor and Help
ja.TypesEditorWithHelp=本体とヘルプ
zh_hans.TypesEditorWithHelp=本体与帮助文件
zh_hant.TypesEditorWithHelp=本體與幫助檔

en.TypesEditorOnly=Minimal
ja.TypesEditorOnly=最小構成
zh_hans.TypesEditorOnly=仅安装本体
zh_hant.TypesEditorOnly=僅安裝本體

en.TypesCustom=Custom
ja.TypesCustom=カスタム
zh_hans.TypesCustom=自定义
zh_hant.TypesCustom=自定義

en.ComponentsMain=Sakura Editor NEXT itself
ja.ComponentsMain=Sakura Editor NEXT 本体
zh_hans.ComponentsMain=Sakura Editor NEXT
zh_hant.ComponentsMain=Sakura Editor NEXT

en.ComponentsHelp=help files
ja.ComponentsHelp=ヘルプファイル
zh_hans.ComponentsHelp=帮助文件
zh_hant.ComponentsHelp=幫助檔

en.ComponentsKeyword=Syntax and keyword definitions
ja.ComponentsKeyword=シンタックス・キーワード定義
zh_hans.ComponentsKeyword=语法提示文件
zh_hant.ComponentsKeyword=語法提示檔案

en.startmenu=Create a &Start menu shortcut
ja.startmenu=スタートメニューにショートカットを作成(&S)
zh_hans.startmenu=添加到开始菜单(&S)
zh_hant.startmenu=添加到開始菜單(&S)

en.proglist=Register in Windows' &Open with list
ja.proglist=Windows の「アプリで開く」に登録(&P)
zh_hans.proglist=添加到程序列表(&P)
zh_hant.proglist=添加到程式清單(&P)

en.fileassoc=Add "Open with Sakura Editor &NEXT" to file context menus
ja.fileassoc=ファイルの右クリックに「Sakura Editor NEXT で開く」を追加(&E)
zh_hans.fileassoc=添加“用 Sakura Editor NEXT 打开”(&E)
zh_hant.fileassoc=添加“用 Sakura Editor NEXT 打開”(&E)

en.sendto=Add Sakura Editor NEXT to the Send &to menu
ja.sendto=「送る」メニューに Sakura Editor NEXT を追加(&T)
zh_hans.sendto=添加到"发送到"菜单(&T)
zh_hant.sendto=添加到“發送到”選單(&T)

en.sakuragrep=Add "&Grep with Sakura Editor NEXT" to folder context menus
ja.sakuragrep=フォルダーの右クリックに「Sakura Editor NEXT で Grep」を追加(&G)
zh_hans.sakuragrep=添加到“Grep with Sakura Editor NEXT”(&G)
zh_hant.sakuragrep=添加到“Grep with Sakura Editor NEXT”(&G)

en.fileassocMenu=Open with Sakura Editor &NEXT
ja.fileassocMenu=Sakura Editor NEXTで開く(&E)
zh_hans.fileassocMenu=用 Sakura Editor NEXT 打开(&E)
zh_hant.fileassocMenu=用 Sakura Editor NEXT 打開(&E)

en.sakuragrepMenu=&Grep with Sakura Editor NEXT
ja.sakuragrepMenu=Sakura Editor NEXTでGrep(&G)
zh_hans.sakuragrepMenu=Grep with Sakura Editor NEXT(&G)
zh_hant.sakuragrepMenu=Grep with Sakura Editor NEXT(&G)

en.residentStartup=Start Sakura Editor NEXT in the background when I sign in(&R)
ja.residentStartup=サインイン時に Sakura Editor NEXT をバックグラウンドで起動(&R)
zh_hans.residentStartup=开机时启动(&R)
zh_hant.residentStartup=開機時啟動(&R)

en.IconPreferencefolder=Preference folder
ja.IconPreferencefolder=設定フォルダー
zh_hans.IconPreferencefolder=文件夹设置
zh_hant.IconPreferencefolder=資料夾設定

en.StartNow=Launch Sakura Editor NEXT
ja.StartNow=Sakura Editor NEXT を起動
zh_hans.StartNow=现在启动
zh_hant.StartNow=現在啟動

en.TasksShortcuts=Shortcuts:
ja.TasksShortcuts=ショートカット:
zh_hans.TasksShortcuts=快捷方式:
zh_hant.TasksShortcuts=捷徑:

en.TasksWindowsIntegration=Windows integration:
ja.TasksWindowsIntegration=Windows との連携:
zh_hans.TasksWindowsIntegration=Windows 集成:
zh_hant.TasksWindowsIntegration=Windows 整合:

en.TasksStartup=Startup:
ja.TasksStartup=起動:
zh_hans.TasksStartup=启动:
zh_hant.TasksStartup=啟動:

en.MultiUser=Saving settings beside the application is not recommended under Program Files because Windows may redirect those writes. Select OK to continue anyway, or Cancel to keep per-user settings.
ja.MultiUser=Program Files 内のアプリケーションと同じ場所へ設定を保存すると、Windows によって書き込み先が変更される場合があります。このまま続けるには「OK」、ユーザーごとの保存へ戻すには「キャンセル」を選択してください。
zh_hans.MultiUser=软件将会以兼容模式安装。使用非管理员用户编辑配置文件时，文件可能无法被管理员用户访问。(VirtualStore功能)
zh_hant.MultiUser=軟件將會以相容模式安裝。使用非管理員用戶編輯設定檔時，檔案可能無法被管理員用戶訪問。（VirtualStore功能）

en.InitWiz_Title=Settings location
ja.InitWiz_Title=設定の保存場所
zh_hans.InitWiz_Title=配置文件保存位置
zh_hant.InitWiz_Title=設定檔保存位置

en.InitWiz_SubTitle=Choose where Sakura Editor NEXT keeps your settings
ja.InitWiz_SubTitle=Sakura Editor NEXT の設定を保存する場所を選択してください
zh_hans.InitWiz_SubTitle=选择将Sukura配置文件保存至当前用户或软件目录内
zh_hant.InitWiz_SubTitle=選擇將Sukura設定檔保存至當前用戶或軟件目錄內

en.InitWiz_Comment=Per-user settings are recommended and work reliably with both install modes.
ja.InitWiz_Comment=通常はユーザーごとの保存を推奨します。どちらのインストール方法でも安全に利用できます。
zh_hans.InitWiz_Comment=若您不清楚此选项，请不要修改
zh_hant.InitWiz_Comment=若您不清楚此選項，請不要修改

en.InitWiz_Check=Save settings separately for each user (recommended)
ja.InitWiz_Check=設定をユーザーごとに保存する（推奨）
zh_hans.InitWiz_Check=将每个用户的配置文件单独保存
zh_hant.InitWiz_Check=將每個用戶的設定檔單獨保存

en.ReadyMemo_SaveLocation=Settings location:
ja.ReadyMemo_SaveLocation=設定の保存場所:
zh_hans.ReadyMemo_SaveLocation=设定文件保存位置
zh_hant.ReadyMemo_SaveLocation=設定檔案保存位置

en.ReadyMemo_UserProfileDir=Each user's profile (recommended)
ja.ReadyMemo_UserProfileDir=ユーザーごとのプロファイル（推奨）
zh_hans.ReadyMemo_UserProfileDir=用户配置文件目录
zh_hant.ReadyMemo_UserProfileDir=用戶設定檔目錄

en.ReadyMemo_VirtualStoreDisable=Disable
ja.ReadyMemo_VirtualStoreDisable=無効
zh_hans.ReadyMemo_VirtualStoreDisable=无效
zh_hant.ReadyMemo_VirtualStoreDisable=無效

en.ReadyMemo_ExecProfileDir=Beside the application (compatibility mode)
ja.ReadyMemo_ExecProfileDir=アプリケーションと同じ場所（互換モード）
zh_hans.ReadyMemo_ExecProfileDir=与可执行文件相同
zh_hant.ReadyMemo_ExecProfileDir=與可執行文件相同

en.ReadyMemo_VirtualStoreEnable=Enable
ja.ReadyMemo_VirtualStoreEnable=有効
zh_hans.ReadyMemo_VirtualStoreEnable=生效
zh_hant.ReadyMemo_VirtualStoreEnable=生效

en.AvxRequired=Sakura Editor NEXT requires a processor and operating system that support AVX instructions. This PC cannot run this release binary.
ja.AvxRequired=Sakura Editor NEXT を使用するには、AVX 命令をサポートするプロセッサーとオペレーティング システムが必要です。この PC では配布バイナリを実行できません。
zh_hans.AvxRequired=Sakura Editor NEXT 需要支持 AVX 指令的处理器和操作系统。此电脑无法运行此发行版二进制文件。
zh_hant.AvxRequired=Sakura Editor NEXT 需要支援 AVX 指令的處理器和作業系統。此電腦無法執行此發行版二進位檔案。




[Types]
Name: all;                 Description: "{cm:TypesAll}"
Name: TypesEditorWithHelp; Description: "{cm:TypesEditorWithHelp}"
Name: TypesEditorOnly;     Description: "{cm:TypesEditorOnly}"
Name: custom;              Description: "{cm:TypesCustom}";    Flags: iscustom

[Components]
Name: main;        Description: "{cm:ComponentsMain}";    Types: all TypesEditorWithHelp TypesEditorOnly custom; Flags: fixed
Name: help;        Description: "{cm:ComponentsHelp}";    Types: all TypesEditorWithHelp
Name: keyword;     Description: "{cm:ComponentsKeyword}"; Types: all

[Tasks]
Name: startmenu;   Description: "{cm:startmenu}";        GroupDescription: "{cm:TasksShortcuts}";           Components: main; Flags: checkedonce
Name: desktopicon; Description: "{cm:CreateDesktopIcon}";GroupDescription: "{cm:TasksShortcuts}";           Components: main; Flags: unchecked
Name: proglist;    Description: "{cm:proglist}";         GroupDescription: "{cm:TasksWindowsIntegration}";  Components: main; Flags: checkedonce
Name: fileassoc;   Description: "{cm:fileassoc}";        GroupDescription: "{cm:TasksWindowsIntegration}";  Components: main; Flags: unchecked
Name: sendto;      Description: "{cm:sendto}";           GroupDescription: "{cm:TasksWindowsIntegration}";  Components: main; Flags: unchecked
Name: sakuragrep;  Description: "{cm:sakuragrep}";       GroupDescription: "{cm:TasksWindowsIntegration}";  Components: main; Flags: unchecked
Name: startup;     Description: "{cm:residentStartup}";  GroupDescription: "{cm:TasksStartup}";             Components: main; Flags: unchecked

[Files]
Source: "sakura\sakura.exe";           DestDir: "{app}";                  Components: main; Flags: ignoreversion;
Source: "sakura\sakura_lang_en_US.dll";DestDir: "{app}";                  Components: main; Flags: ignoreversion;
Source: "sakura\sakura_lang_zh_CN.dll";DestDir: "{app}";                  Components: main; Flags: ignoreversion;
Source: "sakura\license\LICENSE";      DestDir: "{app}\license";          Components: main
Source: "sakura\bregonig.dll";         DestDir: "{app}";                  Components: main
Source: "sakura\migemo.dll";           DestDir: "{app}";                  Components: main
Source: "sakura\license\bregonig\*";   DestDir: "{app}\license\bregonig"; Components: main
Source: "sakura\ctags.exe";            DestDir: "{app}";                  Components: main
Source: "sakura\license\ctags\*";      DestDir: "{app}\license\ctags";    Components: main
Source: "sakura\license\windows-terminal\*"; DestDir: "{app}\license\windows-terminal"; Components: main
Source: "sakura\license\codicons\*"; DestDir: "{app}\license\codicons"; Components: main
Source: "sakura\license\fmt\*"; DestDir: "{app}\license\fmt"; Components: main
Source: "sakura\license\ms-gsl\*"; DestDir: "{app}\license\ms-gsl"; Components: main
Source: "sakura\license\wil\*"; DestDir: "{app}\license\wil"; Components: main
Source: "sakura\sakura.exe.manifest.x";DestDir: "{app}";                  Components: main; DestName: "sakura.exe.manifest"; Check: isMultiUserDisabled; Flags: onlyifdoesntexist;
Source: "sakura\sakura.exe.manifest.v";DestDir: "{app}";                  Components: main; DestName: "sakura.exe.manifest"; Check: isMultiUserEnabled; Flags: onlyifdoesntexist;
Source: "sakura\sakura.chm";           DestDir: "{app}";                  Components: help
Source: "sakura\macro.chm";            DestDir: "{app}";                  Components: help
Source: "sakura\plugin.chm";           DestDir: "{app}";                  Components: help
Source: "sakura\sakura.exe.ini";       DestDir: "{app}";                  Components: main; Check: isMultiUserEnabled; Flags: onlyifdoesntexist;
Source: "sakura\keyword\*";             DestDir: "{app}\keyword";         Components: keyword; Flags: recursesubdirs

[InstallDelete]
Type: filesandordirs; Name: "{app}\exthost"

[Registry]
; registry for all user (Admin only)
Root: HKLM; Subkey: "SOFTWARE\Classes\*\shell\sakuraeditor";                       ValueType: string; ValueName: "";     ValueData: "{cm:fileassocMenu}";          Tasks: fileassoc; Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\*\shell\sakuraeditor";                       ValueType: string; ValueName: "Icon"; ValueData: """{app}\sakura.exe""";        Tasks: fileassoc; Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\*\shell\sakuraeditor\command";               ValueType: string; ValueName: "";     ValueData: """{app}\sakura.exe"" ""%1"""; Tasks: fileassoc; Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\sakura.exe\shell\open\command"; ValueType: string; ValueName: "";     ValueData: """{app}\sakura.exe"" ""%1"""; Tasks: proglist;  Flags: uninsdeletekey; Check: CheckPrivilege(true)

Root: HKLM; Subkey: "SOFTWARE\Classes\directory\shell\sakuraGrep";         ValueType: string; ValueName: "";     ValueData: "{cm:sakuragrepMenu}";                 Tasks: sakuragrep; Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\directory\shell\sakuraGrep";         ValueType: string; ValueName: "Icon"; ValueData: """{app}\sakura.exe""";            Tasks: sakuragrep; Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\directory\shell\sakuraGrep\command"; ValueType: string; ValueName: "";     ValueData: """{app}\sakura.exe"" -GREPDLG -GREPMODE  -GFOLDER=""%1"" -GOPT=""SP"" -GCODE=99 "; Tasks: sakuragrep;  Flags: uninsdeletekey; Check: CheckPrivilege(true)

Root: HKLM; Subkey: "SOFTWARE\Classes\directory\BackGround\shell\sakuraGrep";         ValueType: string; ValueName: "";     ValueData: "{cm:sakuragrepMenu}";      Tasks: sakuragrep; Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\directory\BackGround\shell\sakuraGrep";         ValueType: string; ValueName: "Icon"; ValueData: """{app}\sakura.exe"""; Tasks: sakuragrep; Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\directory\BackGround\shell\sakuraGrep\command"; ValueType: string; ValueName: "";     ValueData: """{app}\sakura.exe"" -GREPDLG -GREPMODE  -GFOLDER=""%V"" -GOPT=""SP"" -GCODE=99  "; Tasks: sakuragrep; Flags: uninsdeletekey; Check: CheckPrivilege(true)

; add ProgID
; see https://www.glamenv-septzen.net/view/14#idf5215e
; see https://docs.microsoft.com/en-us/visualstudio/extensibility/registering-verbs-for-file-name-extensions?view=vs-2017
Root: HKLM; Subkey: "SOFTWARE\Classes\SakuraEditor.Document";                    ValueType: string; ValueName: "";       ValueData: "";                              Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\SakuraEditor.Document\shell";              ValueType: string; ValueName: "";       ValueData: "";                              Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\SakuraEditor.Document\shell\open";         ValueType: string; ValueName: "";       ValueData: "";                              Flags: uninsdeletekey; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\SakuraEditor.Document\shell\open\command"; ValueType: string; ValueName: "";       ValueData: """{app}\sakura.exe"" ""%1""";   Flags: uninsdeletekey; Check: CheckPrivilege(true)

; add File Handlers to each extensions
; see https://docs.microsoft.com/en-us/visualstudio/extensibility/specifying-file-handlers-for-file-name-extensions?view=vs-2017
Root: HKLM; Subkey: "SOFTWARE\Classes\.txt\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.log\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.c\OpenWithProgids";                       ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.cpp\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.cs\OpenWithProgids";                      ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.h\OpenWithProgids";                       ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.md\OpenWithProgids";                      ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.ini\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.java\OpenWithProgids";                    ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)
Root: HKLM; Subkey: "SOFTWARE\Classes\.rst\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(true)

; registry for the selected current user (non-admin install mode)
Root: HKCU; Subkey: "SOFTWARE\Classes\*\shell\sakuraeditor";                       ValueType: string; ValueName: "";     ValueData: "{cm:fileassocMenu}";          Tasks: fileassoc; Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\*\shell\sakuraeditor";                       ValueType: string; ValueName: "Icon"; ValueData: """{app}\sakura.exe""";        Tasks: fileassoc; Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\*\shell\sakuraeditor\command";               ValueType: string; ValueName: "";     ValueData: """{app}\sakura.exe"" ""%1"""; Tasks: fileassoc; Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\Applications\sakura.exe\shell\open\command"; ValueType: string; ValueName: "";     ValueData: """{app}\sakura.exe"" ""%1"""; Tasks: proglist;  Flags: uninsdeletekey; Check: CheckPrivilege(false)

Root: HKCU; Subkey: "SOFTWARE\Classes\directory\shell\sakuraGrep";         ValueType: string; ValueName: "";     ValueData: "{cm:sakuragrepMenu}";                 Tasks: sakuragrep; Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\directory\shell\sakuraGrep";         ValueType: string; ValueName: "Icon"; ValueData: """{app}\sakura.exe""";            Tasks: sakuragrep; Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\directory\shell\sakuraGrep\command"; ValueType: string; ValueName: "";     ValueData: """{app}\sakura.exe"" -GREPDLG -GREPMODE  -GFOLDER=""%1"" -GOPT=""SP"" -GCODE=99  "; Tasks: sakuragrep;  Flags: uninsdeletekey; Check: CheckPrivilege(false)

Root: HKCU; Subkey: "SOFTWARE\Classes\directory\BackGround\shell\sakuraGrep";         ValueType: string; ValueName: "";     ValueData: "{cm:sakuragrepMenu}";      Tasks: sakuragrep;  Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\directory\BackGround\shell\sakuraGrep";         ValueType: string; ValueName: "Icon"; ValueData: """{app}\sakura.exe"""; Tasks: sakuragrep;  Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\directory\BackGround\shell\sakuraGrep\command"; ValueType: string; ValueName: "";     ValueData: """{app}\sakura.exe"" -GREPDLG -GREPMODE  -GFOLDER=""%V"" -GOPT=""SP"" -GCODE=99  "; Tasks: sakuragrep;  Flags: uninsdeletekey; Check: CheckPrivilege(false)

; add ProgID
; see https://www.glamenv-septzen.net/view/14#idf5215e
; see https://docs.microsoft.com/en-us/visualstudio/extensibility/registering-verbs-for-file-name-extensions?view=vs-2017
Root: HKCU; Subkey: "SOFTWARE\Classes\SakuraEditor.Document";                    ValueType: string; ValueName: "";       ValueData: "";                              Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\SakuraEditor.Document\shell";              ValueType: string; ValueName: "";       ValueData: "";                              Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\SakuraEditor.Document\shell\open";         ValueType: string; ValueName: "";       ValueData: "";                              Flags: uninsdeletekey; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\SakuraEditor.Document\shell\open\command"; ValueType: string; ValueName: "";       ValueData: """{app}\sakura.exe"" ""%1""";   Flags: uninsdeletekey; Check: CheckPrivilege(false)

; add File Handlers to each extensions
; see https://docs.microsoft.com/en-us/visualstudio/extensibility/specifying-file-handlers-for-file-name-extensions?view=vs-2017
Root: HKCU; Subkey: "SOFTWARE\Classes\.txt\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.log\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.c\OpenWithProgids";                       ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.cpp\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.cs\OpenWithProgids";                      ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.h\OpenWithProgids";                       ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.md\OpenWithProgids";                      ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.ini\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.java\OpenWithProgids";                    ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)
Root: HKCU; Subkey: "SOFTWARE\Classes\.rst\OpenWithProgids";                     ValueType: string; ValueName: "SakuraEditor.Document"; ValueData: "";           Flags: uninsdeletevalue; Check: CheckPrivilege(false)

[Icons]
Name: "{group}\{cm:AppName}";                                                Filename: "{app}\sakura.exe";                         Components: main;                            Tasks: startmenu
Name: "{group}\{cm:ComponentsHelp}";                                         Filename: "{app}\sakura.chm";                         Components: help;                            Tasks: startmenu;
Name: "{group}\{cm:IconPreferencefolder}";                                   Filename: "{userappdata}\sakura";                     Components: main; Check: isMultiUserEnabled; Tasks: startmenu;
Name: "{autodesktop}\{cm:AppName}";                                          Filename: "{app}\sakura.exe";                         Components: main;                            Tasks: desktopicon;
Name: "{group}\{cm:UninstallProgram,{cm:AppName}}";                          Filename: "{uninstallexe}";                                                                        Tasks: startmenu;
Name: "{autostartup}\{cm:residentStartup}";                                  Filename: "{app}\sakura.exe";   Parameters: "-NOWIN"; Components: main;                            Tasks: startup;
Name: "{usersendto}\{cm:AppName}";                                           Filename: "{app}\sakura.exe";                         Components: main;                            Tasks: sendto;

[Run]
FileName: "{app}\sakura.exe"; Description: "{cm:StartNow}"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent runasoriginaluser
; In-application update relaunch.  Deliberately not `postinstall` and not
; `skipifsilent`: it is not a checkbox the user ticks, and the whole point is
; that it runs during the /VERYSILENT install the exiting editor started.  The
; entry above is skipped in that run, so exactly one of the two ever fires.
FileName: "{app}\sakura.exe"; WorkingDir: "{app}"; Flags: nowait runasoriginaluser; Check: ShouldRelaunchAfterUpdate

[UninstallDelete]
;Uninstall時に確認無く消されるのでコメントアウト
;Type: files; Name: "{app}\sakura.ini"
;Type: files; Name: "{userappdata}\sakura\sakura.ini"; Check: isMultiUserEnabled
;Type: files; Name: "{app}\sakura.ini"; Check: isMultiUserDisabled

[Dirs]
Name: "{userappdata}\sakura"; Components: main; Tasks: startmenu; Check: isMultiUserEnabled

[Code]
const
  PBM_SETBARCOLOR = $0409;
  PBM_SETBKCOLOR = $2001;
  PremiumProgressColor = $00C6942D;
  PremiumProgressTrackColor = $00323232;

#ifdef REQUIRE_AVX
const
  PF_AVX_INSTRUCTIONS_AVAILABLE = 39;

function IsProcessorFeaturePresent(ProcessorFeature: DWORD): Boolean;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';
#endif

function SetWindowTheme(Window: HWND; SubAppName, SubIdList: String): HRESULT;
  external 'SetWindowTheme@uxtheme.dll stdcall';

var
  MultiUserPage: TInputOptionWizardPage;
  MultiUserPageEnabled : Boolean;

{ **********************************
   Utility Functions
  ********************************** }

function isMultiUserEnabled : Boolean;
begin
  Result := False;
  if MultiUserPageEnabled then
    Result := MultiUserPage.Values[0];
end;

function isMultiUserDisabled : Boolean;
begin
  Result := not isMultiUserEnabled;
end;

function CheckPrivilege( admin: Boolean ) : Boolean;
begin
  if admin then
    Result := IsAdminInstallMode
  else
    Result := not IsAdminInstallMode;
end;

procedure PolishStandardWizardPages;
var
  ContentLeft, ContentWidth: Integer;
begin
  { Remove the legacy floating folder artwork and use the full content width. }
  ContentLeft := WizardForm.DirEdit.Left;
  ContentWidth := WizardForm.DirBrowseButton.Left +
    WizardForm.DirBrowseButton.Width - ContentLeft;
  WizardForm.SelectDirBitmapImage.Visible := False;
  WizardForm.SelectDirLabel.Left := ContentLeft;
  WizardForm.SelectDirLabel.Width := ContentWidth;
  WizardForm.SelectDirBrowseLabel.Left := ContentLeft;
  WizardForm.SelectDirBrowseLabel.Width := ContentWidth;

  { The bundled content is plain text.  Let the dark wizard style own the
    client, font, border, focus, selection, and scrollbar colors together. }
  WizardForm.InfoBeforeMemo.UseRichEdit := False;
  WizardForm.InfoBeforeMemo.ScrollBars := ssVertical;

  WizardForm.ReadyMemo.ScrollBars := ssVertical;
end;

procedure ApplyInstallProgressColors;
begin
  if not HighContrastActive then
  begin
    { The native normal progress state is green. Disable that control theme so
      the explicit restrained blue and charcoal colors are honored. }
    SetWindowTheme(WizardForm.ProgressGauge.Handle, '', '');
    SendMessage(WizardForm.ProgressGauge.Handle, PBM_SETBKCOLOR, 0,
      PremiumProgressTrackColor);
    SendMessage(WizardForm.ProgressGauge.Handle, PBM_SETBARCOLOR, 0,
      PremiumProgressColor);
  end;
end;

{ **********************************
   Custom Wizard Page
  ********************************** }

{ Callback event functions }

function NextButtonClickMultiUser( Sender : TWizardPage): Boolean;
var
  selected: Integer;
{  t : String;}
begin
  Result := True;
  if MultiUserPageEnabled then
  begin

{ DEBUG CODE
   t := 'MultiUser Setting :';
   if IsAdminLoggedOn then
     t := t + 'Administrator ';
   if ((GetWindowsVersion shr 24) >= 6 ) then
     t := t + 'Vista ';
   if MultiUserPage.Values[0] = False then
     t := t + 'SingleUser ';
   t := t + Format( 'WinVer: %.8x', [GetWindowsVersion] );
   MsgBox( t, mbConfirmation, MB_OK);
}

    { Alert if admin mode && multi user = False }
    if IsAdminInstallMode and
      ( MultiUserPage.Values[0] = False ) then
      begin
{
         Program Files等のシステムフォルダーへインストールする場合はUACを無効にしないと設定が保存できません。
}
         selected := MsgBox(
          CustomMessage('MultiUser'),
          mbConfirmation,
          ( MB_OKCANCEL ));
        if selected = IDCANCEL then
          Result := False;
      end;
  end;
end;

function ShouldSkipMultiUser(Sender : TWizardPage ): Boolean;
var
  selectdir : String;
begin
  Result := False;
    if ( not MultiUserPageEnabled ) then
      Result := True
    else
      begin
        selectdir := AddBackslash(ExpandConstant( '{app}' ));
        if FileExists( selectdir + 'sakura.exe' ) then
          begin
            Result := True;
            if GetIniInt( 'Settings', 'MultiUser', 0, 0, 1, selectdir + 'sakura.exe.ini' ) = 1 then
              MultiUserPage.Values[0] := True
            else
              MultiUserPage.Values[0] := False;
          end;
      end;
end;

{ **********************************
   System Event Functions
   ********************************** }

function ShowExitSetupConfirmation: Boolean;
var
  ConfirmForm: TSetupForm;
  MessageLabel: TNewStaticText;
  YesButton, NoButton: TNewButton;
begin
  ConfirmForm := CreateCustomForm(ScaleX(400), ScaleY(142), True, True);
  try
    ConfirmForm.Caption := SetupMessage(msgExitSetupTitle);
    ConfirmForm.BorderStyle := bsDialog;
    ConfirmForm.Position := poOwnerFormCenter;
    ConfirmForm.Color := WizardForm.Color;
    ConfirmForm.Font.Assign(WizardForm.Font);

    MessageLabel := TNewStaticText.Create(ConfirmForm);
    MessageLabel.Parent := ConfirmForm;
    MessageLabel.AutoSize := False;
    MessageLabel.WordWrap := True;
    MessageLabel.Caption := CustomMessage('ExitSetupPrompt');
    MessageLabel.SetBounds(ScaleX(24), ScaleY(16), ScaleX(352), ScaleY(72));

    NoButton := TNewButton.Create(ConfirmForm);
    NoButton.Parent := ConfirmForm;
    NoButton.Caption := SetupMessage(msgButtonNo);
    NoButton.ModalResult := mrNo;
    NoButton.Default := True;
    NoButton.Cancel := True;
    NoButton.SetBounds(ScaleX(296), ScaleY(102), ScaleX(80), ScaleY(28));

    YesButton := TNewButton.Create(ConfirmForm);
    YesButton.Parent := ConfirmForm;
    YesButton.Caption := SetupMessage(msgButtonYes);
    YesButton.ModalResult := mrYes;
    YesButton.SetBounds(ScaleX(208), ScaleY(102), ScaleX(80), ScaleY(28));

    ConfirmForm.ActiveControl := NoButton;
    Result := ConfirmForm.ShowModal = mrYes;
  finally
    ConfirmForm.Free;
  end;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if Confirm then
  begin
    Confirm := False;
    Cancel := ShowExitSetupConfirmation;
  end;
end;

function ShowRunningApplicationConfirmation: Boolean;
var
  RunningForm: TSetupForm;
  MessageLabel: TNewStaticText;
  RetryButton, CancelButton: TNewButton;
begin
  RunningForm := CreateCustomForm(ScaleX(400), ScaleY(118), True, True);
  try
    RunningForm.Caption := CustomMessage('RunningAppTitle');
    RunningForm.BorderStyle := bsDialog;
    RunningForm.Position := poScreenCenter;

    MessageLabel := TNewStaticText.Create(RunningForm);
    MessageLabel.Parent := RunningForm;
    MessageLabel.AutoSize := False;
    MessageLabel.WordWrap := True;
    MessageLabel.Caption := CustomMessage('RunningAppPrompt');
    MessageLabel.SetBounds(ScaleX(24), ScaleY(16), ScaleX(352), ScaleY(48));

    CancelButton := TNewButton.Create(RunningForm);
    CancelButton.Parent := RunningForm;
    CancelButton.Caption := SetupMessage(msgButtonCancel);
    CancelButton.ModalResult := mrCancel;
    CancelButton.Cancel := True;
    CancelButton.SetBounds(ScaleX(288), ScaleY(78), ScaleX(88), ScaleY(28));

    RetryButton := TNewButton.Create(RunningForm);
    RetryButton.Parent := RunningForm;
    RetryButton.Caption := CustomMessage('RunningAppRetry');
    RetryButton.ModalResult := mrOk;
    RetryButton.Default := True;
    RetryButton.SetBounds(ScaleX(184), ScaleY(78), ScaleX(96), ScaleY(28));

    RunningForm.ActiveControl := RetryButton;
    Result := RunningForm.ShowModal = mrOk;
  finally
    RunningForm.Free;
  end;
end;

{ True when the editor itself started this setup to apply a downloaded update.
  Read from the command line rather than from a file, so a stale marker can never
  make an ordinary interactive install relaunch the application. }
function ShouldRelaunchAfterUpdate: Boolean;
begin
  Result := ExpandConstant('{param:UPDATERELAUNCH|0}') = '1';
end;

{ Waits for the last editor process to release its mutex.  The update installer
  is started by the exiting application, so the mutex is normally held for the
  moment it takes that process to finish exiting: this is a race to wait out, not
  a user who left the editor open. }
function WaitForApplicationToClose(TimeoutMilliseconds: Integer): Boolean;
var
  Waited: Integer;
begin
  Waited := 0;
  while CheckForMutexes('MutexSakuraEditor') and (Waited < TimeoutMilliseconds) do
  begin
    Sleep(250);
    Waited := Waited + 250;
  end;
  Result := not CheckForMutexes('MutexSakuraEditor');
end;

function ConfirmApplicationIsClosed: Boolean;
begin
  { An update relaunch runs under /VERYSILENT /SUPPRESSMSGBOXES.  The retry
    dialog below is a custom form, which /SUPPRESSMSGBOXES does not suppress, so
    showing it there would block the install forever on a window nobody can see.
    Wait for a bounded time instead and fail the setup if the editor really is
    still running; the staged installer stays on disk and the next Ready gesture
    can try again. }
  if ShouldRelaunchAfterUpdate then
  begin
    Result := WaitForApplicationToClose(30000);
    if not Result then
      Log('The editor is still running after the update wait; aborting the update install.');
    Exit;
  end;

  Result := True;
  while CheckForMutexes('MutexSakuraEditor') do
  begin
    if not ShowRunningApplicationConfirmation then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function InitializeSetup: Boolean;
begin
  Result := ConfirmApplicationIsClosed;
  if not Result then
    Exit;

#ifdef REQUIRE_AVX
  { The single Release binary has an AVX baseline.  AVX2 and AVX-512 are
    selected internally after the application validates CPUID and XGETBV. }
  if IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE) then
  begin
    Log('AVX baseline is available; installing the runtime-dispatch binary.');
    Result := True;
  end
  else
  begin
    MsgBox(CustomMessage('AvxRequired'), mbError, MB_OK);
    Result := False;
  end;
#endif
end;

{ Add multi user selection page if supported }
procedure InitializeWizard;
begin
  PolishStandardWizardPages;
  ApplyInstallProgressColors;

  { Create multi user page }
  MultiUserPage := CreateInputOptionPage( wpSelectComponents, CustomMessage('InitWiz_Title'),
    CustomMessage('InitWiz_SubTitle'),
    CustomMessage('InitWiz_Comment'), False, False );
  MultiUserPage.Add( CustomMessage('InitWiz_Check') );
  MultiUserPage.Values[0] := True;
  MultiUserPage.OnShouldSkipPage := @ShouldSkipMultiUser;
  MultiUserPage.OnNextButtonClick := @NextButtonClickMultiUser;
  MultiUserPageEnabled := True;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  { Keep the install type available without presenting its combo box as the
    page's primary action. This also avoids an accent-colored focus rectangle
    around the default "recommended" value on first display. }
  if CurPageID = wpSelectComponents then
    WizardForm.ActiveControl := WizardForm.NextButton;

  if CurPageID = wpInstalling then
    ApplyInstallProgressColors;
end;

procedure AppendReadyMemoSection(var Memo: String; Section, NewLine: String);
begin
  if Section <> '' then
  begin
    if Memo <> '' then
      Memo := Memo + NewLine + NewLine;
    Memo := Memo + Section;
  end;
end;

{ Build List of installation configuration for ready page }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  MemoSettingsLocation: String;
begin
  MemoSettingsLocation := CustomMessage('ReadyMemo_SaveLocation') + NewLine + Space;
  if isMultiUserEnabled then
    MemoSettingsLocation := MemoSettingsLocation + CustomMessage('ReadyMemo_UserProfileDir')
  else
    MemoSettingsLocation := MemoSettingsLocation + CustomMessage('ReadyMemo_ExecProfileDir');

  Result := '';
  AppendReadyMemoSection(Result, MemoDirInfo, NewLine);
  AppendReadyMemoSection(Result, MemoTypeInfo, NewLine);
  AppendReadyMemoSection(Result, MemoSettingsLocation, NewLine);
  AppendReadyMemoSection(Result, MemoComponentsInfo, NewLine);
  AppendReadyMemoSection(Result, MemoTasksInfo, NewLine);
end;
