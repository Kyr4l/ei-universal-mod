                 Evil Islands
             curse of the lost soul
=====================================================
               February 23, 2001


Copyright 2000 Nival Interactive,
Copyright 2000 Ravensburger Interactive Media GmbH
All rights reserved

Fishtank Interactive is a registered trademark of 
Ravensburger Interactive Media GmbH


Contents of Readme
==================
1. Hints about game settings
2. Multiplayer FAQ
3. Troubleshooting
4. Technical support


=====================================================
1. Hints about game settings
============================

1.1 Autorun programme's "Options" section - parameters

a) Terrain quality
   Low - switching details gradation at distances of
55 m and 90 m accordingly; no waves. Visual quality 
is good.
   Medium - switching details gradation at distances 
of 90 m and 120 m accordingly; no waves. Visual quality
is very good. 
   High - switching details gradation at distances of
90 m and 120 m accordingly; waves. Visual quality is 
very good.

FPS increase: High => Medium ~70%, Medium => Low ~20%.
High and Medium value have practically no visual dif-
ferences but fps decreases by ~1.5-2 times.

b) Lightning frequency
   Rare - once per minute. Low visual quality but takes 
practically no processor time.
   Medium - once every 10 seconds. Better than Low but
still leaves many artifacts visible. 
   Frequent - once per second. Best possible quality. 

FPS increase: Frequent => Medium ~20-30%, 
Medium => Rare ~10-15%

c) Texture quality
   Low - very bad quality, minimum video memory usage. 
   Medium - medium texture quality, video memory usage 
is ~3-4 times lower. Suitable for video adapters with 
8 Mb of RAM or more. 
   High - best possible texture quality, highest video 
memory usage. Recommended for video adapters with 16 Mb
of RAM or more (preferably 32 Mb or more). 

If texture quality value is set no higher than recom-
mended one, this setting practically doesn't affect 
the performance (app. 5% difference), because all 
textures fit into video memory anyway. However, if 
texture quality is set higher than recommended value, 
performance decreases due to the fact that textures 
load through AGP bus from the range reserved for video
adapter (app. 10% performance decrease), or load from 
the system memory (fps decreases by 4 times).

d) Filtering
   Point - poor picture quality 
   Bilinear - good picture quality 
   Trilinear - very good picture quality 

In terms of fps differences between Point and Bilinear 
values is almost negligible; when changing from Bilinear 
to Trilinear values, fps may drop by ~30%.

e) Mipmapping
When switched off, allows to save about 25% of video 
memory, but decreases picture quality significantly 
and somewhat reduces general performance.

f) Dithering
Improves picture quality in 16-bit colour mode. 
No performance decrease has been detected.

g) Stretch video
With many video adapters this option causes movies 
to play jerkily... Best course of action is to try 
it on each individual machine. This parameter doesn't 
affect performance in other game screens.

h) Quick cursor
Reduces performance by app. 30%, but ensures that 
cursor is redrawn ~16 times per second, independently 
from fps. Recommended for less powerful machines.

i) Colour depth
   16 bit - good speed and acceptable quality 
   32 bit - with almost all video adapters (except 
ATI Rage 128) fps drops by app. 2 times, but picture 
quality improves significantly. 


1.2 Game "Options" parameters which affect game speed

We recommend leaving only shadows from characters, 
since shadows from objects and trees take a lot of 
video memory (and of processor time, depending on 
Lightning frequency).


1.3 Windows swap-file parameters

To ensure smooth running of the game, we do not 
recommend setting fixed size of Windows swap file, 
but let Windows manage it automatically. When the game
starts, it checks the volume of free disk space needed
for normal running; if you see a warning on the screen,
you'll have to free the necessary amount of your hard
disk. Minimum hard disk space required is app. 300 Mb, 
but we recommend freeing about 500 Mb. Also, please 
check your disk defragmentation: optimising this 
parameter can significantly increase game speed and 
the speed of loading game zones. Use the Defrag system 
utility after the game has been successfully installed. 
We strongly recommend setting the game up on an uncom-
pressed drive (or part of the drive).


=====================================================
2. Multiplayer FAQ
==================

Q: What do I need to play the game via the Internet?
A: First of all you'll need an Internet connection. 
The best option, especially if you are setting up your 
own server, is a dedicated line connection, though not
many people have access to those yet. With a permanent
Internet connection the machine usually gets static 
IP-address, which is a better option for game server. 
To play as a client it's sufficient to have a dial-up 
connection with at least 28 Kb/s speed. A high-speed 
modem (56 Kb/s) or an ISDN line (64/128 Kb/s) would 
improve things further. If you want to set up your 
own game server, you must have direct access to the 
Internet. 

Q: Can I use a satellite Internet connection?
A: Satellite Internet connections usually use proxy-
servers set up to support http and ftp protocols; 
it is not possible to play the game via those. Even 
if you have direct Internet access, satellite connec-
tions process signals after a certain delay, so it 
would be rather difficult to engage in joint actions 
with other players. 

Q: How do I find out my IP-address?
A: This question is usually ask by Dial-up connection 
users who are assigned different dynamic IP-address 
every time the connect to their providers. Launch 
winipcfg.exe programme (the file is located in your 
Windows folder). The programme will display the cur-
rent network configuration of your machine including 
IP-address. If you are connected to more than one 
network - e.g. LAN and dial-up - make sure that the 
top drop-down box with network drivers shows the 
driver which connects you to the Internet. If you are 
using an automatic dial-up programme (we strongly 
recommend to obtain one), after connection is made 
the programme will probably inform you of the current
IP-address.

Q: How do I know whether I have direct access to 
the Internet?
A: If you connect to the Internet only via a proxy-
server, your network administrator would usually 
inform you about that when connection is made, and 
prompt you to set up your browser accordingly (e.g. 
IP_proxy:3128). If you can work without a proxy-
server, then probably you have direct Internet access. 
However, if you connect via a NAT, you may also think 
that you have a direct connection while in reality 
your machine will not be accessible to other com-
puters in WAN. An easy way to find out about that 
is to check your IP-address (see previous answer) - 
LANs have certain ranges of addresses reserved:

10.0.0.0    - 10.255.255.255
169.254.0.0 - 169.254.255.255
172.16.0.0  - 172.31.255.255
192.168.0.0 - 192.168.255.255

If your IP-address falls within one of these ranges, 
you do not have direct access to the Internet and 
won't be able to set up a public EI game server. Other 
clients will be able to connect to your server only if 
they are connected to your LAN. If you connect via 
a NAT, you'll probably  be able to play as a client 
at other Internet game servers. 

Q: I have a dynamic IP-address. Can I set up my own 
game server?
A: Yes you can. It will be accessible to players who 
know its IP-address or the ones who can see your 
server in the master server's list (if you publish 
this information on master server). However, other 
players won't be able to enter your address in their 
address book; or, rather such an entry will not 
guarantee that they can connect to your server the 
next time. Some Internet providers reserve clients' 
IP-addresses for a certain period of time after they 
disconnect (usually for 10-15 minutes). In that case, 
if connection is broken and you reconnect (especially 
if you use an automatic dial-up programme) your 
machine will receive the same IP-address and your 
clients will be able to re-establish connection with 
your server more easily. If you don't have this 
service, you'll have to wait for about 1 minute, 
while information about your server is deleted from 
the master server's list. 

Q: How will the server connected via modem work?
A: If your server is connected to the Internet via 
an ordinary modem, do not set the maximum number of 
players to 6. If you do, your clients (including 
yourself) will experience very slow gameplay with 
lots of long delays. Rather, set the maximum number 
of players according to the actual connection speed 
your phone line provides. Please note that clients 
have much more of incoming traffic than outgoing, 
therefore the server's modem must be able to support 
large volume of outgoing traffic. So using server 
modem with asymmetric protocol 56 Кb/s (V90, 56K) 
may be inefficient; better use 33,6 Кb/s connection 
with ordinary symmetric protocol. On the other hand, 
for clients asymmetric protocol will work better. 

Q: Can I play the game via a proxy-server?
A: It depends. You definitely won't be able to set up 
your own game server. To connect to another game 
server, your proxy-server must allow for a two-way 
traffic of UDP-packets with port address 8888, and 
to connect to master server - TCP-packets with port 
address 28006. The best option is to connect via a NAT 
which allows all kinds of packet traffic. Otherwise 
contact your network ad-ministrator to set up your 
proxy server accordingly. 

Q: Why my game server is not present in the master 
server's list?
A: First of all make sure your "Closed server" box is 
not ticked. You must have a good stable connection with 
the master server. Currently the master server is 
located in our headquarters, so check your connection 
with Nival by typing (in MS-DOS prompt window):

ping server.nival.com

If your connection is good enough, the problem probably 
is that one of the routers between you and the master 
server (on your side) is blocking TCP-packets with port 
address 28006. Contact your network administrator to 
fix the problem. 

Q: My game server is present in the list, but people 
cannot connect to it. Why?
A: Probably because they can't establish direct con-
nection with your machine. If you launch your server 
in a LAN which is separated from the Internet by a 
proxy-server or a NAT, in certain (rare) cases your 
server will be able to connect to the master server, 
while other players won't be able to connect to you. 
They will see your server in the list, but in the 
Ping column they'll see 9999 value. Another possibility 
is that one of the routers on your side is blocking 
UDP-packets (all of them or just the ones with port 
address 8888). So while other players will see your 
server, they won't be able to connect to it, and you 
won't be able to play on their servers. Contact your 
network administrator to fix the UDP-packets traffic 
problem. 

Q: I downloaded the master server list and some of 
the game servers have 9999 ping value. Why?
A: Possibly those servers have just disconnected but 
haven't yet been erased from the list. Wait for about 
a minute and refresh the list. If these servers are 
still there, see the answer to previous question. 


=====================================================
3. Troubleshooting
==================

In case of incorrect visualization we recommend using
drivers from the \Drivers directory on the game CD 1:

	ATI
	    Rage 3D, Rage Pro, etc (legacy HW)
	    Rage 128, Rage 128 Pro
	    Radeon
	3DFX
	    Voodoo III
	    Voodoo IV
	    Voodoo V
	Matrox
	    G100
	    G200
	    G400
	nVidia
	    Riva128
	    TNT
	    TNT2
	    GeFORCE256
	    GeFORCE2

Intel i740 video adapters
=========================
Official Intel drivers for i740 are not fully compa-
tible with DirectX 7.0, which results in the game 
error when entering a game zone, after a large amount 
of textures is loaded into the video memory. However, 
in the DirectX 8.0 this incompatibility seems to be 
fixed. In Windows 98 and 2000 the game can be started 
without problems (in the low screen mode 640x480x16), 
but we don't recommend using this accelerator because 
of limitation of 16-bit color, anti-aliasing and video 
performance. We also can't guarantee the compatibility 
of i740 with all versions of Windows (unlike the video 
adapters included in the recommended list).

Co-operative applications launching
===================================
For stable operation of game we highly recommend 
not to switch to other applications by using the 
Alt+Tab combination. In addition, before launching 
the game we recommend closing all other applications
(especially Microsoft Outlook).


=====================================================
4. Technical support
====================

Please send your questions, comments and suggestions 
to Fishtank Interactive by E-mail at the address:

support@fishtank-interactive.com

Our web sites
=============
www.evil-islands.com
www.fishtank-interactive.com
www.ravensburger.de/interactive
www.nival.com

ENJOY IT!