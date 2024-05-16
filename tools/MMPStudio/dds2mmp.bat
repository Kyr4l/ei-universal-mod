@echo off

setlocal

set folder_path=".\dds2mmp\"

cd /d %folder_path%
for %%f in (*.dds) do (
    echo Processing file: %%f
    ..\MMPS.exe "%%f"
)

echo All files have been processed.

endlocal
