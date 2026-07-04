<!-- : Addon command line interface
@echo off
set ADDON_CLI_RUNNER=%~nx0
set ADDON_WORKDIR=%CD%

rem Run addon check by default if valid full file path passed
if /I "x%~1"=="x%~dpnx1" (
    if exist "%~1" (
        cmd /c "cscript //nologo """%~f0?.wsf""" //job:VBS check %*"
        pause
        exit
    )
)

cscript //nologo "%~f0?.wsf" //job:VBS %*
if errorlevel 1 (
    exit /b 1
)
exit /b 0
--->
<package><job id="VBS"><script language="VBScript">

Set fso = WScript.CreateObject("Scripting.FileSystemObject")
Set sh = WScript.CreateObject("WScript.Shell")
Set args = WScript.Arguments.Unnamed
q = Chr(34)

Function RegRead(regkey, default)
    On Error Resume Next
    value = sh.RegRead(regkey)
    If err.number = 0 Then
        RegRead = value
    Else
        RegRead = default
    End If
End Function

starter_dir = RegRead("HKCU\Software\Gipat.Ru\EI_Starter\path", "")
game_dir = RegRead("HKCU\Software\Gipat.Ru\EI_Starter\EvilIslands\Path Settings\WORK PATH", "")

If starter_dir = "" Or Not fso.FileExists(fso.BuildPath(starter_dir, "engine\game.exe")) Then
    If starter_dir <> "" And fso.FileExists(fso.BuildPath(starter_dir, "EIStarter.exe")) Then
        WScript.Echo "It seems that old starter is installed." &_
                     " Install starter 1.045 or newer to use this script"
    Else
        WScript.Echo "Cannot find a valid starter directory"
    End If
    WScript.Quit 1
End If
If game_dir = "" Or Not fso.FileExists(fso.BuildPath(game_dir, "game.exe")) Then
    WScript.Echo "Cannot find a valid EI directory"
    WScript.Quit 1
End If

command = q & fso.BuildPath(starter_dir, "engine\game.exe") & q & " addon"
For i = 0 to args.count - 1
    command = command & " " & q & args.item(i) & q
Next

sh.CurrentDirectory = game_dir
Set exec_obj = sh.Exec(command)
Do While Not exec_obj.StdOut.AtEndOfStream
    WScript.Echo exec_obj.StdOut.ReadLine()
Loop

</script></job></package>
