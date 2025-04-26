# EI Multitool

## How to use

This is a special tool I've made to automate packing the EI assets, it uses WINE, Rsync and GNU Parallel.
This tool can't run on Windows nor be ported to it.

To use it, open a bash terminal in this directy, and run `bash makemod.sh`

## WARNINGS

- Do not delete directories, the script uses many hardcoded paths to function properly.
- Resouces must be symlinked instead of being copy pasted, paths mustbe relative and not absolute.
- `makemod.sh` must be executed from this directory, otherwise it will not function properly.

### Developed by Kyr4l
