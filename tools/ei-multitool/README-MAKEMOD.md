# EI Multitool

## How to use

This is a special tool i made to automate compilation of the EI assets, they use WINE and some basic commands available on Linux
I don't plan to port this tool to Windows as I do not use it (and I hate batch)

To use it, open a bash terminal in this directy, and run `bash makemod.sh`

## WARNINGS

- Do not delete directories, the script uses many hardcoded paths to function properly.
- Resouces must be symlinked instead of being copy pasted, paths mustbe relative and not absolute.
- `makemod.sh` must be executed from this directory, otherwise it will not function properly.

### Developed by Kyr4l
