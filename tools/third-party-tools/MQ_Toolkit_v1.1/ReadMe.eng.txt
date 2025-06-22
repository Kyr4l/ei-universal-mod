
Tools which can help to create Quests for MP game.

        * * *

Folders and files description:

Texts          - Texts for quests
Texts\English  - English quests
Texts\Russian  - Russian quests
Texts\German   - German quests
\Quests        - Config for zone such as quest place, zone exit etc.
\Utils         - Utils to compile .mq files
BildAll.bat    - Batch file to create .mq
ReadMe.eng.txt - Documentation in English.
ReadMe.rus.txt - Documentation in Russian.

     * * *

Start BuildAll.bat and as a result you will see:
English .MQ in folder: "mq\English"
Russian .MQ in folder: "mq\Russian"
German .MQ in folder:  "mq\German"

     * * * 

This is an example for "Brigands loot" quest:

"z3q1" this is a quest code.
Let's describe:
z3 - quest require zone3.
q1 - ordinal number of quest for zone3

Configuration files placed in folder: "\Quests\z3q1"
\Quests\z3q1\map.txt   - Game zone places for Enter end Exit and quest point coordinates.
\Quests\z3q1\quest.Ini - Quest and Briefing configuration.

English language briefing sceanrio in folder "\English"
\Rus\briefing z3q1_1   - Dialogue 1 (Get quest) - "Brigands' loot"
\Rus\briefing z3q1_2   - Dialogue 2 (Refuse quest) - "Brigands are too strong!"
\Rus\briefing z3q1_3   - Dialogue 3 (Quest compleate) - "Done!"
\Rus\quest z3q1        - Sub quests description. "Brigands' loot"

Also Russian in folder "\Russian" and German in folder "\German"

   * * 

map.txt file description

## 				// Comment

#zone zone_name allod_name type // Zone type, must be one of game, brief, edge (Game zone, Briefing zone, Place of passage in global map.)
#res 				
MprFile MobFile                 // Files to load

#maps
Xsize Ysize                     // Size of zone in sectors
MinimapTexture ObjTexture       // minimap and quest-map textures

#figure

#weather
none | rain | snow              // Weather in zone. Default: rain. Don't mix any weather! Only one for zone

#sky
normal | cave                   // Sky for zone. Default: normal

#exit 1
neighbour_zone_name NExit       // Neighbour zone and exit

#exit 2
neighbour_zone_name NExit       // Next neighbour zone and exit

#exit ...
neighbour_zone_name NExit       // ... neighbour zone and exit

#deploy
x1 y1 x2 y2                     // Rectangle coordinates for zone enter place

#remove
x1 y1 x2 y2                     // Rectangle coordinates for zone exit place

#view
Angle                           // Angle camera when enter zone

Undocumented:

#allod gipat | ingos | suslanger // Textures for allod

#deployangle			// 
range				// range - number


#restrict			// View camera contorl.
x y z				// 
