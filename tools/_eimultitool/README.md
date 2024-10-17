# EI Multitool
### Developed by Kyr4l

## How to use
This is a special tool i made to automate compilation of the EI assets, they use WINE and some basic commands available on Linux
I don't plan to port this tool to Windows as I do not use it (and I hate batch)

To use it, open a bash terminal in this directy, and run `bash makemod.sh` or `bash makemod-replace.sh`
The difference between these two is that `makemod-replace.sh` replaced the content of the compiled mod in the repo's root folder, meaning it makes it easier to make slight changes and recompile everything quickly without having to do copy pastes, as the compiled mod folder can be symlinked into the modload's mod folder.
