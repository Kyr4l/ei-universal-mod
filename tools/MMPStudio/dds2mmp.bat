@echo off

setlocal

set folder_path=".\dds\"

cd /d %folder_path%
for %%f in (*) do (
    echo Processing file: %%f
    .\MMPS.exe "%%f"
)

echo All files have been processed.

endlocal
