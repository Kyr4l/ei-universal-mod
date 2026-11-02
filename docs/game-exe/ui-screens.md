# The game screens and the generic controls in game.exe (0x602000-0x65F000)

Written 2026-10-31 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x602000-0x65F000: the
dialog screen, the camp, the menus, save / load, the world map, the quest log, the options, the tutorial, the battle HUD and its strips,
the scroll bars / text list / list box / edit / tooltip controls). The screen manager (`CInterface`), the widget base (`CUIWindow`,
`UIQuadMesh`, `CUIBlitSurface`, `UIButtonAnim`), the info panels, the console, the cursor and the cameras are the framework page
(`docs/game-exe/ui-framework.md`, 0x5E2000-0x602000); the names of those functions are used here as given in `functions.tsv`.
Data tables were read out of the exe file (`.data` at VA 0x783000 = file 0x383000).

Tags: **[V]** read in the decompile, **[I]** inferred from several functions, **[?]** guess. Addresses are VAs; offsets are bytes
from the object start. "sub-object" = the `IUIEvents` object embedded at `+0x94` of every window (its handlers see `this - 0x94`).

## How to re-implement it (short version)

1. **Every screen is a `CUIWindow`** (0x164 bytes of base) with two vtables: the window vtable (slots 20 Draw, 22 UpdateElements,
   24 Update, 25 Build (called by `CInterface::PushScreen`), 26 OnClose, 27 OnCommand (the result code of a closed child), 41
   GetTutorialKey; screens add 28..54) and the event vtable at `+0x94` (1 Deactivate, 2 LButtonDown, 3 LButtonUp, 4 MouseMove,
   5 RButtonDown, 6 RButtonUp, 7 LButtonDblClk, 9 KeyDown(vk, char, scancode), 10 Char, 11 KeyUp, 12 Wheel). Verified on every
   class below; the convention of the framework page holds without exception [V].
2. **Widgets are arrays on the window**: `+0xF8` surface figures (textures), `+0x108` quads (`UIQuadMesh::InitRect / InitFrame16 /
   InitQuadUV`, all in 800x600 units, `+0x220` = visible, `+0xCC` alpha, `+0xD4` colour, `+0x224..+0x22C` scale), `+0x118` GDI
   blit surfaces (`GetDC / FillBackground / DrawText / ReleaseDC`), `+0x128` buttons (`UIHitRegion::SetRect`, tooltip by text index),
   `+0x138` animated figure holders, `+0x148` owned child controls. A handler finds the clicked button with `CUIWindow::ButtonAtPoint`
   and switches on its index; the index tables below are therefore the UI "API" for a bot.
3. **Modal boxes** are pushed windows (`CMessageBox`, 0x180 bytes): they close with `resultBase | 1` (OK) or `| 2` (Cancel) and the
   parent receives that code in slot 27. Every "yes" flow in this page is such a code (0x10001, 0x20001 ...).
4. **Orders** leave the HUD as `CPlayerClient::SendMsgNN_*` calls (`world-client-players.md`); the camp edits the `CParty` / `HeroRecord`
   directly and sends nothing until the player leaves (`Session_LeaveGame`); the dialog talks to the server with `Order_MoveTo`,
   `SendMsg19` (talk markers) and `SendMsg0F_Move` (camera cuts); saves are `Game_SaveToSlot_Wrap(folder)` + two small files.
5. **Text comes from `CTextManager`** (`GetString(key)`, `OpenText("quest <id>")`, `SeekSection("#screen")`, `ReadLineValue`,
   `ReadUntilHash`), positions from `map.txt`, options from the globals listed in section 11.

> Slot names: where this page says 24 Update / 25 Build / 27 OnCommand / 9 KeyDown(vk, char, scancode), the authoritative names are those of ui-framework.md 5.2 (18 Build hook, 24 Tick, 25 OnOpen, 27 OnChildClosed(code), 9 OnKeyDown(vk, repeat, flags)); the slot numbers agree, only the names differ (slot names follow ui-framework.md).

## 1. Conventions

| Thing | Value |
|---|---|
| screen coordinates | 800x600 virtual units everywhere (`UiScale::ScalePoint / UnscalePoint` to pixels) [V] |
| colours in DrawText | COLORREF BGR: 0xA7D7E4 = the beige body text, 0x31B3FF = the golden title / highlight, 0x82F5FF = tutorial yellow, 0xFF = red [V] |
| fonts | 0 small, 1 normal (default of every control), 2 large (`CInterface +0x70` holders) [V] |
| click sounds | `UI_PlayClickSound("buttons\\<group>\\<name>.wav")`: messbox\ok / cancel, save\select / delete, camp\put_on / put_off / buy / slot / mode / scroll / move / change / hero / perk, objectives\select / enter, tutorial\ok / next, globalmap\transit / move / zone / wind, battle\click / on_off / weapon / nomagic / sling / clock, menu\ok / stone / shift [V] |
| closing a screen | `CInterface::RequestPopScreen(code)` (0x5F74E0): code 0 = plain pop, else the parent's slot 27 gets it [V] |
| tutorial key | slot 41 (GetTutorialKey) returns the key (camp_*, global_map, quest_mission, options ...); `CInterface::ShowTutorialForTopScreen` 0x5F86F0, called at the end of every Build, pushes the CTutorialScreen for it when the page was not shown yet (tutorial map `CInterface +0x98`) [V] |
| command ids | 0x10001 / 0x20001 / 0x30001 ... = message box bases 0x10000.. + 1 (OK); 0x100001 = the save / load window cancelled, 0x100002 = it saved, 0x110001 = options closed, 0x1000001 = lobby back [V] |

## 2. Screens, ids, sizes

`CInterface::CreateScreenById` 0x5F5BA0 (the game-mode message `CPlayerClient::OnMsg07_GameMode`) builds [V]:

| id | class | vtable / sub | size | notes |
|---|---|---|---|---|
| 1 | CBattleHud | 0x740764 / 0x740730 | 0x230 | the zone HUD (section 13) |
| 2 | CMainMenu | 0x740688 / 0x740654 | 0x17C | section 7 |
| 3 | DialogScreen | 0x7405AC / 0x740578 | 0x2A4 | zone "talk" mode (section 3) |
| 4 | CWorldMapScreen | 0x740B3C / 0x740B08 | 0x294 | section 9 |
| 5 | CCampScreen | 0x74091C / 0x7408E8 | 0x260 | section 5 |
| 7 | CFreeCameraScreen | 0x740840 / 0x74080C | 0x168 | observer / editor camera, no HUD (Build 0x62A830) |
| 8 | CCreditsScreen | 0x740C3C / 0x740C08 | 0x1FC | `CUIVideoWindow` subclass, "Titles" ini (section 7) |
| 9 | CExchangeScreen | 0x740D18 / 0x740CE4 | 0x1B8 | lobby item exchange (section 6) |

Pushed by other screens: CEscMenu 0x7412A4 / 0x741270 (0x168), CSaveLoadWin 0x741388 / 0x741354 (0x194), COptionsScreen 0x741FAC /
0x741F78 (0x418), CQuestLogWin 0x7411BC / 0x741188 (0x1F0), CTutorialScreen 0x740E28 / 0x740DF4 (0x194), CMessageBox 0x742348 /
0x742314 (0x180), CDifficultyBox 0x74225C / 0x742228 (0x184), CConnectProgressWin 0x741DD8 / 0x741DA4, CCampScreen2 0x74214C /
0x742118 (0x220), CLobbyScreen 0x741CD0 / 0x741C9C (0x260), CIntroScreen 0x73F7F8 / 0x73F7C4, CLobbyStatusBar 0x741478 / 0x741444
(0x170, a child of the dialog screen).

## 3. The dialog screen (DialogScreen, screen id 3)

The in-zone "talk" mode: the player walks, clicks NPCs, picks topics, watches briefings. Fields in `batch-screens-structs.tsv`.

| Build 0x6024B0 [V] | |
|---|---|
| art | figure "saveload"; text panel quads (200..600 x 430..570) + InitFrame16 frame; topic panel (155..416 x 580..645) |
| surfaces | [0] text (200,0x1AE)-(600,0x23A), [1] topic list (0,0)-(0xFA,0x1E) |
| controls | `+0x260` CScrollBar (0x248,0x1B8)-(0x252,0x230); `+0x25C` CTextListWin (0xA5,0x1AA)-(0x27B,0x23A) line 10 |
| state | `+0x16C` mode = 0, `+0x168` = 1, `+0x164` = 0; camera distance g_0x79BD34 saved in `+0x268` and set to 4.0; CMusicZone::PlayBattle; `+0x27D` = zone record +0x44 (single-speaker zone); `+0x280` CLobbyStatusBar when Lobby_IsAlive; `+0x284` = the party hero ids |
| children | a 0xC-byte camera link (vt 0x741154) and a UIControl740208 camera controller |

Modes `+0x16C` [V]: 0 world, 1 topic list, 2 walking into place, 3 briefing text running, 4 line finished.

| Event | What happens (address) |
|---|---|
| L click, mode 0 | unit under the cursor whose server brain is idle (`+0x190` 0 / 0xB and `+0x1B4` == 9) -> click1.wav + `BuildAnswerList` 0x608B60 (topics = every `b.<unit>.<topic>` variable == 1, "goodbye" appended, 5 row buttons (0xD2,0x1B8+0x18 i)-(0x23A,..), mode 1); else terrain point -> `Order_MoveTo(player brain, pt, run=0)` unless `+0x27D` (0x6030F0) |
| double click, mode 0 | same, but a terrain point runs (`Order_MoveTo(.., 1)`) (0x603360) |
| L click, mode 1 | row i + page `+0x264` -> `+0x184` = topic, `SelectAnswer(0,1)` 0x6064E0 |
| L click, mode 3 / 4 | 3 = `ParseDialogScript(0)` next phrase; 4 = `StartDialogLine` (end the briefing) |
| keys (0x6034A0) | Esc: mode 0 pushes CEscMenu, 3 skip, 4 end, 2 `SelectAnswer(1,0)`, 1 -> 0; Space: 3 next phrase, 4 end; Enter: 3 skips the whole line; keyboard.ini actions in mode 0: 4 (obj) opens CQuestLogWin when a `q.<zone>.<x>` == 1 names the zone, 0xB camera to the player, 0x35 QuickSave("saveq"), 0x36 SaveGame_BeginLoad("saveq") |
| mouse move (0x603D00) | hover unit with topics -> cursor frame 0xE and its name drawn at (x-0x7D, y-0x14); a zone exit whose `z.<target>` != 1 -> cursor 0xF; mode 1 hovered row -> `+0x188` + DrawTopicList |
| OnCommand 0x606F40 | 0x10001 (leave-zone yes) = `Session_RequestMapChange(+0x164)`; 0x20001 = push CSaveLoadWin in save mode; 0x90001 / 0x100001 = leave the game (`CInterface +0x44` = 1, `LobbySession_ReleaseActive(2)`) |

`SelectAnswer` 0x6064E0 [V]: "goodbye" -> mode 0; a "constr*" topic -> `World_SetConstrCurrent(id)` + `Game_SendLeave(5)` (enter the camp
/ constructor); else opens the text `briefing <topic>`, reads its `#show` lines to name the second (`+0x1A8`) and third (`+0x1AC`)
speaker, computes the two facing poses 2.5 m + proto radius apart (`+0x1B0 / +0x1BC`, centre `+0x1C8`, dirs `+0x1D4..`), saves the
pre-dialog poses (`+0x210 / +0x228`), `PlaceUnit` (walk, 0x6089D0: a 0x30-byte node in the `+0x240` list, `Order_MoveTo`; teleport after
30 s) and mode = 2 (Update 0x604130 turns it into `StartBriefing` 0x608210 when everybody stands).

`ParseDialogScript` 0x604C50 [V] reads the section line by line: `#nolips`, `#show <unit> [idx] [weapon]` -> `AddSpeaker` 0x605DE0
(a live unit joins `+0x294`, `SendMsg19(unit, 1)`; a non-unit name = an NPC portrait "quitem%04d" on a quad at the slot table
0x74111C/0x741138), `#hide <unit>` -> `RemoveSpeakerByName` 0x6062C0, `#animation <n>`, `#camera <shot> [yaw dist height]`, `#phrase
<speaker> [voice]`: label = unit name or the `pers` map `+0x26C`, text appended to `+0x248` as "<label>: <text>\n" with the colour
prefix 0x79349C, voice "<root>\briefing\<name>\<n>.mp3" (`PlaySound3D` -> `+0x250`, timer `+0x24C` = len * 0.05 s), camera
`DialogCamera::SetShot` 0x607EC0 (shots 1..12: 1 both from 3 x d at 1.5 m, 2 player close-up -30 deg 4 m, 3 second +30, 4 third,
5 both 2.7 d 15 deg h 3, 6 / 7 +-45 deg 3.5 m, 8 third 15 deg, 9 both 2.4 d -15 deg, 10..12 with the others sunk to -100 by
`World_SetUnitHeight`) or `FocusOn` 0x607BB0 (= `SendMsg0F_Move(target, eye, target)`), speaker animation
(`StartSpeakerAnimation` 0x607A60: entries 3 / 1 / 0..2 of the model, "unhuma" tables 0x7410FC / 0x74110C), mouth open. At the end of
the section the briefing rewards of the quest record `g_0x7C87D4[topic]` are printed: exp +0xC "brief_get_exp", money +0x10
"brief_get_money", items +0x14 "brief_get_item", lost +0x20 "brief_lost_item", quests +0x2C "brief_get_quest", completed +0x38
"brief_complete_quest", specials +0x68 "brief_get_specials_%d". `StartDialogLine` 0x608450 ends it: `SendMsg19(unit, 2)` for every
speaker, poses restored, every `b.<x>.<topic>` variable equal to the topic fires `Script::NamedEventSource(world, 0, name)`,
`SendMsg19(0, 0)`, camera back (`SendMsg0F_Move(+0x204, +0x1F8)` or `SendMsg04_Position` after an auto briefing).

Update 0x604130 [V]: every `b.<unit>.<topic>` == 1 (topic not constr*) of a party unit keeps `CPlayerServer::SendMsg19(unit, 1 or 2)`
(the "can talk" marker); when `+0x168` and the exit `+0x164` is allowed and `Game_CollectHeroesInZoneExit` finds the party inside ->
`Session_RequestMapChange`. `OnPartyChanged` 0x602C90 auto-starts a briefing on zone entry when a `b.%s` key with 3 parts is 1.0
(`+0x18C` = 1). `ShowGameOver` 0x608C70 = gameover.wav + CMessageBox("game_over", base 0x20000).

**CLobbyStatusBar** 0x608D20 (0x170 bytes, multiplayer only): a strip (0x1D0,0)-(800,0xA0) with one portrait button per lobby player
(`AddPortraitSlot` 0x608F90: face "infa%s%s%dface" quadrant into a 256x256 figure, button (0x2E8-n*0x38, 0)-(800-n*0x38, 0x50)),
status text per player (`+0x24` of the record: lmp_status_zone / base / camp / connect, "we offer" / "they offer"); a click
(0x6097E0) sends `LobbyClient::SendNamedEvent(4 or 5)` = an exchange request (or releases the current one).

## 4. The camp (CCampScreen, screen id 5)

Entered by `Game_SendLeave(5)` with `constr.current` naming the merchant `CGSPlayer` (`g_0x7AA5D8`). `Build` 0x60C870 pauses the server
(`Game_SetWorldServerField6C(1)`), reads the merchant flags (`+0xC / +0xD` item / spell constructor, variables i.on/offitemconstr,
i.on/offspellconstr, i.noconstr), the 9 price coefficients (`+0x21C` = CGSPlayer +0x10..+0x30) and the two stock parties
(`+0x1B4` items = +0x3C, `+0x1C8` spells = +0xAC), creates the camp hero units (`Game_CreateHeroUnits` of the proto-type 0x32 records,
`+0x1A0`), `SelectNextHero` and the first enabled tab. A first visit (`CGSPlayer +0x38`) seeds the stock bounds from the party's max /
min magic skills and `CollectUnitsIntoParty`; a merchant without stock gets `FillConstructorStock` 0x612CF0 (every Instruction
Weapon / Armor / Quick Item blueprint, Material, Prototype, Rune and "wand" sample). `OnClose` 0x60E6B0 unpauses, removes the camp
units, releases the previews and the 8 basket parties. `OnButton(7)` = leave: baskets moved back (`Party_MoveAllItems`),
transit.wav, `Session_LeaveGame`; Esc does the same (0x60F600). Lobby: Backspace clears / Enter opens the chat.

| Buttons (Build) [V] | rect / meaning |
|---|---|
| 0 / 1 | prev / next hero arrows (0xC,0x1C2)-(0x34,0x1EA) / (0x94..0xBC): press sets `+0x25C` = 1 / 2 and repeats every 0.5 s while held (Update 0x60F6B0); hidden in tab 0 without the item constructor |
| 2 / 3 | confirm (300,0x1D6)-(0x14A,500) / cancel (0x1D6,0x1D6)-(500,500) -> `OnBuyOrRepair` 0x611220 / `OnCancel` 0x611760 when `+0x210 / +0x211` allow |
| 4..11 | 8 tabs across x 0x26C..0x30C (20 px each), y 0x1AE..0x1EA: 0 weapons (tip 0x4E84) 1 skills 2 spell trade 3 spell constr 4 item trade 5 item constr 6 repair 7 leave; disabled tabs fall back (`OnButton` 0x60E9E0) |
| sub-windows (ItemAtPoint slot 25, kinds) | party bar `+0x1E0` kind 1, spell stock bar `+0x1EC` kind 2, item stock bar `+0x1F0` kind 3, hero weapon / armor slots `+0x1E4` kinds 2 / 3 ("kind 0" = hero slot in OnDropItem), quick bar `+0x1E8` kind 1, trade baskets kinds 0x11 / 0x1C, repair 0x19, spell components 0x13, item components 0xD |

**Item moves** [V]: a click on an item of a bar reaches `CCampScreen::OnDropItem` 0x610AD0 (slot 42) with the bar kind; `CanDropItem`
0x6100E0 (slot 43) decides per tab (table in the batch row: weapons / armor / quick only in tab 0, broken weapons refused; spell containers
need the hero's magic skill in tab 1; the constructor rules below); the move is `Party_TransferItem(item, from, to, flags)` or, in tab 0,
`PutOnItem` 0x60FCA0 / `UnequipItem` 0x60FE40 (`HeroRecord::AddWeapon / AddArmor + CUnit::EquipArmor / AddQuickItem / AddSpellItem`, 4
weapons, 4 quick, 8 spells, one armor per type; the first weapon becomes the slot in hand `+0x774`; `CUnitInfoWin::Redraw`). Shift
(`CInterface +0x7D`) moves a whole material stack. Every move calls `CCampBar2::RebuildList` on the bars concerned.

**Prices** (`GetItemPrice` 0x60FAB0, slot 54, kind 0..8 = coefficient index; x87 code the decompile drops, verified live in
`game-memory.md`): base = item `GetPrice`; weapon / armor: round(base * coef[k] - base * coef[8] * (1 - durability / max)); kind 8 repair =
max(1, round(base * coef[8] * wear)) or 0; others round(base * coef[k]). `EnterMode` 0x611930 recomputes `+0x20C` total, `+0x210 / +0x211`
and the title / "+%d" / "camp_current_money %d" line per tab: 0 "camp_repair_all" (sum kind 8 over the hero's items, item-constructor
merchant only), 1 "camp_level_up" (points left), 2 "camp_spell_trade" (kind 0 of the buy basket - kind 1 of the sell basket), 3 spell
constructor: a container in `+0x1D4` -> "camp_spell_deconstr" kind 3 (+ kind 0 when the material flag), a blueprint ->
`RefreshSpellComponents` 0x612290 builds the preview `+0x218` ("camp_spell_constr", kind 2 per component), 4 "camp_item_trade" (kind 4 -
kind 5), 5 item constructor: usable item -> "camp_item_deconstr" kind 7, blueprint -> `RefreshItemComponents` 0x612600 -> `+0x214`
("camp_item_constr", kind 6 per component), 6 "camp_item_repair" (kind 8 of `+0x1C4`). Confirm refused when the total exceeds the
party money `+0x50`.

**Confirm** (`OnBuyOrRepair` 0x611220, buy.wav) [V]: tab 0 repairs the hero's weapons / inventory (durability = max); 1 commits the
level-up (`CCampSkillsWin::SelectNextAvailable(1,0)`); 2 / 4 register the sold items in the merchant registry (`CItemRegistry` +0x1D4:
spells `AddUnique_Loot`, blueprints / runes `AddUnique_Quest`, items by proto row `AddUnique_Armor / Weapon / Quick`, empty 0x3007
containers deleted), then bought -> party, sold -> stock; 3 = `DeconstructSpell` 0x612500 (container -> a "Prototype" + one "Rune" per
perk) or the preview replaces the components; 5 = `DeconstructItem` 0x6129C0 (item -> "Instruction Weapon/Armor/Quick Item" with
`+0x54` = proto row, one "material.<name>" per proto Components, a "spell container" with its spell) or the preview replaces the
components; 6 repairs the basket; finally money -= total. **Cancel** returns the baskets / components to where they came from.

**Constructor rules** (`CanDropItem` tabs 3 / 5) [V]: spell: a blueprint only into an empty component party and only when its school
fits `GetMagicTotals` 0x610020 (slot 45: the party maximum of `UnitStats_MagicSkill(school)` and of max mana); a rune needs a
blueprint, no container, < 8 runes, the blueprint's slot mask `+0x78[PerkProto +0xC]` and sum(complexity `+0x18`) within the mana
maximum. Item: a blueprint or a usable item only when empty; materials must match the blueprint's material type (`+0x10` M.Type)
and count < proto Components `+0x4C`; a spell container needs the preview (`+0x214`) to exist, be a quick item or the spell to have
perk ic / it, spell level `+0x50` <= preview slots `+0x40`, spell mana `+0x40` <= preview mana `+0x2C`. `RefreshItemComponents` then
drives `CItemConstructor` (`BeginCreateItem(kind, -1) / SetProto / SetMaterial / SetSpell / FinishItem`, `items.md`).

**The bars** [V]: `CCampBar2` 0x620380 (vt 0x74197C; one strip of 0x40-px items at y 0 or 500: scroll arrows buttons 0 / 1 with
auto-repeat, 6 category icons 2..7 (`SetTab` 0x620960, memory per tab), row area 8 with drag scrolling; classifier by mode `+0x194`:
0 `Item_GetCampCategory` 0x6218F0, 1 `Item_GetSpellContainerGroup` 0x6219A0 (spell school 0..2, rune 3 / 4, other 5), 2
`Item_GetSlotGroup` 0x621A80; filter `+0x1B0` 9 = all; `switch_filters` g_0x79BE84 jumps to the category of the moved item).
`CCampBar` 0x622000 (figures camp1..4, 8 slot buttons: 0..3 weapons, 4..7 armor; right click on a weapon slot = the slot in hand
`+0x774`, change.wav 0x622960). `CCampQuickBar` 0x622A80 (8 quick / spell slots). `CUnitInfoWin` 0x622DB0 (stats pane with the row
buttons; mode 1 = skills edit spends points, 0.25 s blink). `CCampSkillsWin` 0x624F10 (figures skills / perks / perks_av: 10 skill
rows, 11 available-perk rows, learned perks; per-hero edit records 0x3C: six stat bytes + spent exp + dirty; `RefreshPerks`
0x625880 uses `Perk_ComputeCost / UnitStats_CanLearnPerk / UnitStats_LearnPerk`, costs printed by `FormatNumberKM` 0x626090).
`CCampTradeWin` 0x6270A0 (trade1..4, two 4x5 grids), `CCampRepairWin` 0x6279E0 (repair1..4, 16 slots), `CCampSpellConstrWin`
0x6280C0 (constrspell1..4), `CCampItemConstrWin` 0x628CC0 (constritem1..4).

## 5. Party screen, lobby, exchange (multiplayer)

**CCampScreen2** 0x613EA0 (pushed by the main menu button 5, modes `Layout` 0x615D30: 9 slot list, 10 new-hero edit, 11 equip and start):
`PartyStore::LoadAll(g_0x7C82A0, "mp\")` fills the slot list `g_0x7C82A4` of 0x74-byte `PartySlot` (CParty + name); buttons in mode 9:
0 back, 1 start (`OnStart` 0x6183B0 -> "lmp_no_pers" box or `PartyStore::SaveAll` + push CLobbyScreen), 5 new / 6 rename / 7 delete
("lmp_deletepers" box -> 0x10001 `DeleteSlot`) / 8 redress (pushes a CCampScreen for the slot), 9..0x12 rows; mode 10: 2..4 random
hero (25 / 25 / 25 stats, height 0.7), 9..0x13 voice rows (lmp_male_voice / lmp_female_voice sets "%s*.mp3"), `OnConfirm` 0x6186F0
needs str + dex + int = 75 and a name; mode 11: 1 `StartGame` 0x617A50 (strips spells the hero cannot cast, copies the voice set,
`PartySlot::SaveToFile`), 4 = reset equipment. Slot 42 `SetEquipped`, 43 `GetSpellCost`, 47 `CommitUnit`, 51 `GetHeroRecords`, 52
`RebuildHeroSlots`, 53 `HeroRecord::GetCarryWeight` [V].

**CLobbyScreen** 0x61B2C0 [V]: reads `config\addrbook.ini` (`Archive_Open`, `NetServerRecords_Serialize`) into `NetClient::SetServerList`,
`LobbyServer_Create`; buttons 0 join (`JoinSelected` 0x61CCE0: row 0 = host with `Lobby_StartServerOrClient(name, players, allod,
password, showFull)`, a password row -> box 0x50000 -> `Lobby_BeginConnectFromAddress`, wrong protocol -> "lmp_wrong_protocol"), 1 back
(`AddrBook_Save` 0x61D1C0 writes the ini, close 0x1000001), 2 direct connect (box 0x30000 / 0x40000: `NetAddress::FromString` +
`NetClient::OnServerFound`), 3 delete server, 4 show-full toggle, 5 password box, 6..9 column sort, 10..13 allod tabs, 14.. rows
(0x34-byte server records). `Update` 0x61C430 polls `NetClient::TickDiscovery` every 0.5 s and shows lmp_server_full /
lmp_you_are_banned / lmp_wrong_password / lmp_cannot_connect on a failed connect. `CConnectProgressWin` 0x61E2F0 (a CMessageBox with a
TcpStream and a progress quad) closes when `LobbyClient::OnServerInfoPacket` is done.

**CExchangeScreen** 0x62C250 (screen id 9): offer parties `+0x190 / +0x194 / +0x198`, money edit `+0x1A8` (its dirty flag pushes
`+0x50` + `LobbyClient::FillParty`), buttons 0 accept (`LobbyClient::ConfirmPair` when the remote money <= ours), 1 retract, 2 leave;
item click 0x62D100 moves between party and offer; `Exchange` 0x62D210 (slot 37) swaps the parties and `Session_LeaveGame`.

## 6. Main menu, esc menu, boxes, credits, intro

| Screen | Build / buttons [V] |
|---|---|
| CMainMenu 0x6326B0 (id 2) | `Databases_Reload`, version text (0x6335E0 "version_name" + build), 6 buttons from the rect table g_0x7C80A8: 0 new game -> CDifficultyBox (base 0x20000; 0x20001 = `Databases_Reload`, Movies\Intro.bik, new-game start 0x5816C0), 1 load -> CSaveLoadWin(load, no slide), 2 quit -> Movies\Exit.bik + `App_PostQuit`, 3 options, 4 credits (CCreditsScreen "Titles"), 5 multiplayer -> CCampScreen2; camera "MainMenu.cam"; hovering animates the menu figure (stone.wav 0x633400); Esc = quit box (0x10001 = quit) |
| CEscMenu 0x62AB20 | figure "escMenu00" / "escMenu00labels" at (400,700) depth 4.8 tilted by g_0x7C7FB0; 5 buttons (g_0x7C80A8): 0 save -> CSaveLoadWin(save, slide) (not in a lobby), 1 load, 2 continue (close), 3 quit -> box "quit" (0x10001 = `LobbySession_ReleaseActive2`), 4 options -> COptionsScreen(slide); hover plays stone.wav and switches the figure animation 2..6 (0x62AD10); 0x100001 / 0x110001 from a child closes the menu too (0x62ACC0) |
| CDifficultyBox 0x6337B0 | "tip option_difficulty", choices "difficulty_easy" / "difficulty_hard" (buttons 2 / 3), OK writes g_0x79BEC0 (0x633DD0) |
| CMessageBox 0x634650 | (ui, titleKey, textKey, base, flags, align): flags 1 OK, 2 Cancel, 4 edit (15 chars, copied into `+0x17C` on OK), 0x10 align; `Build` 0x634790 measures the texts, OK (0x107,y+0x1A)-(0x151,y+0x4A) + Cancel (0x1CF..0x219) or OK alone (0x16B..0x1B5); Enter / Esc = OK / Cancel; base -0x10000 (the fatal quit box) plays Exit.bik and quits |
| CCreditsScreen 0x62B7D0 (id 8) | `Movies_PlayIniList(ini)` + a `CRichEditSurface` scroller fed from "outro.res" (`StreamInFromResource` 0x65EF30, "#scroll <speed>" entries, `RichEditScroller::Start`), screensaver disabled (SPI 0x4A / 0x4B), Bink frames in Draw 0x62BD80; a click / Esc / Space skips (`LobbySession_ReleaseActive2`) |
| CIntroScreen 0x602050.. | plays Movies\<name>.bik or the autoplay.apl camera flights (clip list `+0x1A3`, .cam files, "Can't find camera file %s"); Esc ends, Space = next clip |

## 7. Save / load (CSaveLoadWin 0x741388, 0x194 bytes)

Constructed inline by the callers (CEscMenu 0x62AE90, CMainMenu 0x632D70, DialogScreen / CBattleHud command 0x20001): `+0x178` save mode,
`+0x179` slide-in (in game). Files: `<saves root>\saves\<folder>\info.sav` and `shot.sav`; the game state itself is written by
`Game_SaveToSlot_Wrap(folder)` (outside this page) [V].

| Build 0x653DD0 [V] | |
|---|---|
| buttons | 0 load / save (0xA3,0x20E)-(0xED,0x23E) (tips 0xEAC5 / 0xEAC4), 1 cancel (0x233..0x27D, 0xEAC7), 2 delete (0x16B..0x1B5, 0xEAC6), 3..18 = 16 slot rows (0x6C..0x172 x 0x6C + 0x18 r) |
| quads | 0 / 1 / 2 button art, 3 the screenshot (410,100)-(700,320), 4 cursor; frames list / shot / details |
| controls | `+0x180` CScrollBar (0x6C,0x6C)-(0x180,0x1EC); figure "escMenu00" parts load + loadlabel / save + savelabel |
| list | `ScanSaves` 0x655D40: every `saves\save*` directory -> a 0x44-byte `SaveSlotRecord` (label, folder, CTime, gtime, zone title, map; `+0x40` error = no info.sav "Error Save"), sorted newest first; save mode inserts a new slot 0 "save<n+1>" with the live screenshot (`CInterface +0x5C` thumbnail) |

`info.sav` [V] = `Stream_WriteHeader4(0x111, version 3)`, float gtime, string zone title, string map name, u32 time_t (v >= 2), string
label (v >= 3) (`SaveSlotRecord::Serialize` 0x6539C0; `ScanSaves` reads it with the version gates). `shot.sav` = 0x18000 bytes =
256 x 192 x 16-bit pixels (`CUIBlitSurface::StretchTo(256,192)` in `QuickSave` 0x6570E0; the live figure `+0x184` in `DoSave` 0x655550;
read back into the scratch figure by `UpdateSaveScreenshot` 0x656E10, "Can't lock surface." fatal).

| Action | Flow [V] |
|---|---|
| select | row click / Up / Down / PgUp / PgDn (16) -> `+0x170`, the label copied into the edit string `+0x188`, list + shot refreshed (`ClampSelection` 0x655430) |
| type a name (save mode) | `OnChar` 0x655050: a normal slot (not saveq / savea / saveb, `IsSpecialSlot` 0x657820) starts editing (`+0x18C`, text cleared), < 60 chars and < 0xB2 px wide (font 1); Backspace / Delete / Home / End / Left / Right in `OnKeyDown` 0x6548A0 |
| confirm | button 0 / Enter / double click -> `OnConfirm` 0x655290: save: slot 0 -> `DoSave`, other -> box "overwrite" (0x20001 -> `DoSave`); load -> `OnLoad` 0x655A10: `ShowMessageBar("notify_loading")`, `Game_StopAllSounds`, `SessionRequest::SetLoadSave(g_0x7AFE20, folder, 0) 0x5816D0 (kind 2)` (the session loop loads it) |
| DoSave 0x655550 | free space (`Disk_GetFreeBytes` vs `SavesCurrent_ScanSize`, else box "no_disc_space" 0x30001 -> close 0x100002), `ShowMessageBar("notify_saving")`, `Game_SaveToSlot_Wrap(folder)`, cursor frame 3, record label = `+0x188` (slot 0 unedited = empty -> MakeLabel), time = now, info.sav, shot.sav, close with 0x100002 |
| delete | button 2 -> delete.wav + box "delete" (0x10001 -> `DeleteSelectedSave` 0x655B20: `saves\<folder>\*.*` DeleteFileA + RemoveDirectoryA, rescan); refused for the new slot |
| cancel | Esc / button 1 -> close 0x100001 |

Quick / auto saves [V]: `CSaveLoadWin::QuickSave(folder, renderFrame)` 0x6570E0 is static (ecx = the folder name): "saveq" from the
quicksave key (action 0x35, HUD / dialog / world map), "savea" by `CBattleHud::OnZoneEnter` 0x6360F0 when the option autosave
g_0x79BE7C is on, "saveb" by `CWorldMapScreen::Build`. Labels: "save_quick" / "save_auto_enter" / "save_auto_exit" / zone title
(`SaveSlotRecord::MakeLabel` 0x653A50, zone title = first line of "zone <map>" or "save_camp"). `SaveGame_BeginLoad(folder)` 0x6575B0 =
the quickload key: `_access` the directory, "notify_loading", `SessionRequest::SetLoadSave`. Not in lobby games.

## 8. The world map (CWorldMapScreen 0x740B3C, screen id 4)

`Build` 0x62D350 [V]: current zone (`Records5C_FindByName(g_0x79BF00)`) -> allod record -> figures "Map<allod>" / "MapG<allod>" and the
zone's `map.txt` position (`MapTxt_LoadZonePosition` 0x630E00: section `#zone <id>` then `#position` "x y z" ints; `MapTxt_LoadQuestPosition`
0x630CC0: section `#quest <id>`, first line "x y z"); 0x10-byte entries `+0x190` (figure, zone index or -1, name): visited zones
(`z.<zone>` == 1), quests (`q.<zone>.<quest>` == 1, label from "quest <id>"), reachable zones (`z.` value 2 and zone record +0x28 == 2)
with click buttons; three map layer quads 700..100 x 0..600, a picking surface `+0x210` (entry id in pixel bits 4..7), the game-time
panel "save_day" "%s  %d,  %d:%02d" at (20,20)-(170,70), the zone table `+0x1A4` (0x18 rows: name, travel time) of the allods ingos /
suslanger / gipat, FlameNoise `+0x214`, wind.wav loop, and the autosave "saveb". Buttons: 0 / 1 rotate (0x2E1,0x219)-(0x311,0x249) /
(0x2B1..0x2E1), 2 camp (0xF,0x20D)-(0x6B,0x249) = `World_SetConstrCurrent(0)` + `Game_SendLeave(5)` unless i.nocamp, 3.. = zone
buttons. `OnLButtonDown` 0x62F950: a picked zone entry when travelling is allowed (`+0x1DC`) -> `World_AddGameTime(travel time)` +
`Game_SendString(zone name, 0)` (the travel order); a quest entry or non-travel mode -> CQuestLogWin with `SetCaption(name, +0x1E0
zone points, 1, ..)`. Right drag rotates the map (`+0x168` of the sub-object, dx / 300), `Update` 0x62F050 re-projects the entry
figures and buttons; Esc = CEscMenu; 0x35 / 0x36 quick save / load. Tutorial key "global_map" / "global_map_nocamp" (0x62EE70).

## 9. The quest log / travel window (CQuestLogWin 0x7411BC, 0x1F0 bytes)

One window for three uses (`SetCaption` 0x64BE40 (name, travelTimes, globalMapMode, allowTravel)) [V]: the in-zone quest log (keyboard
action 4 "obj" from the HUD / dialog: mission mode, travel allowed only when `q.<zone>.<x>` names the zone), the "pass time / travel"
dialog of the clock strip (`CHudClockStrip::OpenTimeDialog` 0x648F10) and the world-map quest view (global-map mode with the travel-time
map `+0x178`). Build 0x64BE80: scrollbar `+0x1BC` (quests, step 3), a CTextListWin (0x3C,0xBE)-(0x180,0x1FE) for the objectives, art
"saveload" + "cross", quads 0 OK art (163..237 global / 363..437 mission), 1 cancel art (global only), 2 cursor, 3 map picture, 14 / 15
prev / next map arrows, 16..20 quest state markers; buttons 0 OK (0xA3..0xED global / 0x16B..0x1B5), 1 cancel (0x233..0x27D), 2 / 3
prev / next map (0x19E..0x1BB / 0x2CD..0x2EA x 0x180..0x194), 4..8 quest rows (0x3C..0x168 x 0x1E + 0x18 i), 9.. exit arrows. With
allowTravel the map list `+0x164` is every zone of the `z.` variables in the current allod (any zone in a lobby).

`ShowMap` 0x64CC10 [V]: zone record `+0x14` = map figure drawn at (580,190), 150 px for the larger zone size (sectors `+0x20 / +0x24`);
every zone exit of the record (`+0x4C`, 0x3C bytes: rect `+4..+0x10`, target `+0x28`, "none" skipped) becomes a 0x1C-byte entry (enabled
when a travel time is in `+0x178`, else when `z.<target>` != 1; time `+8`; screen pos 580 + dx * 300 / size, 190 - dy * 300 / size) with
an arrow quad rotated towards the edge and a button; the nearest exit is pre-selected (`SelectExit` 0x64E990: 1.2x scale). Quests =
every `q.<map>.<id>` variable != 0 (3 dot parts), sorted; selected = `g_0x7C8178` (last viewed) or `CInterface +0xAC` (the last named
value of the zone). The "zone <map>" title / description go on surface [1]. `DrawQuestList` 0x64D9B0: 5 rows, title = first line of
"quest <id>", marker quad coloured by `q.<map>.<id>` (2 done, 3 failed). `FillObjectives` 0x64DC60: the quest text (description after the
title, rewards from the quest record `g_0x7C87B4` as "obj_cost_exp %d" / "obj_cost_money %d", then every `#subobj <n>` whose
`q.<map>.<id>.<n>` != 0 with " obj_completed" / " obj_failed") into the CTextListWin, and the quest markers of `map.txt` section
`#quest <id>` (first line = "x y x y ..." ints, `LoadQuestMarkers` 0x64EA80; lobby: the quest record `+0x20..+0x24`) as 20 px quads.

OK (button 0 / Enter, 0x64E330 / 0x64E5A0) [V]: global-map mode with an exit selected -> `World_AddGameTime(hours)` +
`Game_SendString(map, exitIndex)` = travel; otherwise close. Esc closes; the obj key closes in mission mode. Up / Down / PgUp / PgDn
move the quest selection (select.wav, `ClampSelection` 0x64E8B0). `Update` 0x64E760 pulses the markers (cos 5 t) and blinks the arrows.
`OnClose` 0x64D970 restores render state 0x18. Tutorial keys "quest_mission" / "quest_global_map" (0x64D990).

## 10. Options (COptionsScreen 0x741FAC, 0x418 bytes)

`Build` 0x6503C0 [V]: buttons 0 OK (0xD5,0x20E)-(0x11F,0x23E) tip 0x111D4, 1 cancel (0x201..0x24B) 0x111D5, 2..12 = the 11 group tabs
(100..250 x 0x9C + 0x18 i; tooltip "tip option_group_<name>", label "option_group_<name>"), 13..26 = 14 value rows (0xFA..0x208 x
0x9C + 0x18 r), surface [0] (100,100)-(700,500), figure "escMenu00" parts options / optionslabel. `BuildOptionsList` 0x651940 creates one
0x14-byte record per option (table `g_0x787430`, 26 x 0x18: index, name, kind, max, tab, row): kind 0 = `CHScrollBar` (0x212, 0xA3 +
0x18 row)-(0x2A8, +10) with max = table max, kind 1 = `CListBoxWin` (0x212,0xA0 + 0x18 row)-(0x2B2,0xB0) with "option_off" /
"option_on" (difficulty: "difficulty_hard" / "difficulty_easy"); labels = "option_<name>". `BuildKeyList` 0x651600 creates one 0x18-byte
record per keyboard action (table `g_0x787080`, 59 x 0x10: action code, name, tab, row; `KeyActionRow`) with the keys bound to it in the
working table `+0x164` (copy of `g_0x79BF70`: scancode -> action) joined as "A | B" (key names from `g_0x7857B0`, 88 x 0x48: scancode,
name; `KeyNameRow`). `SetTab` 0x651D70 fills 14 row slots `+0x384 / +0x388` for the tab.

| Group (tab) | Options (index: name, kind, max, global) and key actions [V] |
|---|---|
| 0 video | 7 shadow_units sw g_0x79BD14, 8 shadow_buildings sw g_0x79BD18, 9 shadow_flora sw g_0x79BD1C, 10 marks sw g_0x79BD50, 11 footprints sw g_0x79BD4C, 12 select_type sw g_0x79BE64, 13 show_path sw g_0x79BE70, 17 show_flying_hp sw g_0x79BE74, 14 brightness 100 g_0x79BCFC, 15 contrast 100 g_0x79BD00, 16 gamma 100 g_0x79BD04 (value * 0.02 - 1, applied live through the render globals vt+0x58) |
| 1 sound | 0 volume_sfx 100 g_0x79BCC8 (`CSoundManager::SetMasterVolume` live), 1 volume_stream 100 g_0x79BCC4 (SetMusicVolume), 2 volume_voice 100 g_0x79BCCC (SetEffectsVolume), 24 reverse_stereo sw g_0x79BD08 (`CSoundManager +0x8A4` live) |
| 2 sens | 3 power_kbd 100 g_0x79BDC0 (* 0.02, copied to g_0x79BDCC / D8 / E4), 4 power_mouse 100 g_0x79BDBC (* 0.02, -> C8 / D4 / E0), 5 scroll_border 10 g_0x79BDAC, 6 rubber_select 10 g_0x79BDB0 (* 10 px), 20 tooltip_time 20 g_0x79BE80 (* 0.25 s), 22 camera_reverse_x sw g_0x79BD64 (+-1.0), 23 camera_reverse_y sw g_0x79BD68 |
| 3 game | 25 difficulty (easy / hard) g_0x79BEC0 (+ g_0x79BEC4; not in a lobby), 19 autosave sw g_0x79BE7C, 21 switch_filters sw g_0x79BE84; keys pause 1, decel 2, accel 3, obj 4, quicksave 0x35, quickload 0x36 |
| 4 select | keys select1..3 0x37..0x39, select_all 0x3A |
| 5 actions | keys follow 0x1C, use_science 0x1D, swarm 0x1B, run 0x1A, walk 0x19, sneak 0x18, crawl 0x17 |
| 6 camera | keys camera_track 0xB, camera_up / down / left / right 5..8, zoom_in / out 9 / 0xA, camera_norm 0xC, camera1..4 0xD..0x10 |
| 7 items | keys weapon1..4 0x26..0x29, item1..4 0x2A..0x2D |
| 8 spells | keys spell1..8 0x1E..0x25 |
| 9 cshots | keys cs_head / body / rleg / lleg / rhand / lhand 0x11..0x16 |
| 10 windows | 18 show_tutorial sw g_0x79BE78; keys w_text1 / 2 0x32 / 0x33, w_info1..4 0x2E..0x31, w_minimap 0x34, tutorial_script 0x3B |

Key names (`g_0x7857B0`): "1".."0" (0x02..0x0B), "-" "+" TAB, Q..P [ ] A..L ; ' \ Z..M , . /, KP_STAR, SPACE, CAPSLOCK, F1..F12,
PAUSE, KP_HOME .. KP_DEL, extended KP_SLASH 0x135, KP_NUMLOCK 0x145, HOME UP PGUP LEFT RIGHT END DOWN PGDN INS DEL 0x147..0x153 [V].

Flows [V]: a slider or list change is read in `Update` 0x650A30 (`GetChanged`) into the record value and previewed live (volumes,
gamma, stereo); shadows set `+0x410`. A double click or Enter on a key row sets `+0x3F4` (the row turns red); the next `OnKeyDown`
0x650E40 looks the scancode up in the key table, refuses unknown keys, and if the key belongs to another action pushes the box
"key_already_mapped" (0x10001 -> `ApplyPendingKey` 0x652000 moves it), else rebinds at once (the action loses its other keys). OK
(button 0 / `ApplyToGlobals` 0x6524A0) writes `+0x164` back to `g_0x79BF70` and the values to the globals, `EnsureObjectRenderData` when
shadows changed, close 0x110001; cancel (button 1 / Esc) restores the previewed volumes / gamma from the globals and closes 0x110001.
Persisting them (`config\keyboard.ini` and the options ini) is the framework's job [I]. Up / Down move between filled rows, Left /
Right nudge the slider, PgUp / PgDn change the tab.

## 11. Tutorial (CTutorialScreen 0x740E28, 0x194 bytes)

Two constructors in CInterface [V]: `ShowTutorialForTopScreen` 0x5F86F0 (the screen's own tutorial, key = slot 41 of the top screen:
`+0x164` = 0, the page comes from the tutorial map `CInterface +0x98` of `config\tutorial.ini`) and `ShowQueuedTutorial` 0x5F8A80 (the
queued script command `tutorial <name>` / the F1 key tutorial_script, shown only when the option show_tutorial g_0x79BE78 is on or
forced: `+0x164` = 1, page `CInterface +0xA8`). `Build` 0x658E10 reads the text
"tutorial <key>" (first line = title, every `#screen` first line = page title, titles replaced by "tutor <title>" texts) and
`BuildPage` 0x659420 lays out page `+0x184`: `#position x y w [lines]` sets the panel (default (200,50)-(600,560)), `#rectangle l t r b`
adds a pulsing red frame over the screen area it explains (quads list `+0x188`), `#text <icon>` + the next line (a text key) adds a
paragraph with a 64x64 picture "tutorial_<icon>" (decoded 4 per row into the scratch texture) drawn with `DrawWrappedText` (0xBBF9FF).
Buttons 0 prev / 1 OK / 2 next (50 px, bottom row, tips 0x7594..0x7596); the footer shows the neighbour page titles and
"show_tutorial_key <F1 name>". Enter = next page / close on the last, Esc or the tutorial_script key = close; closing stores the page
(`TutorialMapFind` 0x65A770 or `CInterface +0xA8`). Render state 0x18 is forced to 1 while the window is up (0x65A6D0).

## 12. The battle HUD (CBattleHud 0x740764 / 0x740730, screen id 1)

`Build` 0x635250 [V]: hero ids `+0x1DC` from the party or, on a load, from the UI stream `g_0x7AFDF8` (ids, pause flag, zone name (v >
0x46), zone exit (v > 0x61)); figure "battle00"; widgets `+0x194` CHudWeaponBar, `+0x198` CHudQuickBar (8 belt slots), `+0x19C`
CHudSpellList, `+0x1A0` CHudAbilityList, `+0x1A4` CHudMainPanel (portraits + minimap frame), `+0x1A8` CHudUnitInfoPanel, `+0x1AC`
CHudMinimap, `+0x1B0` CHudClockStrip, `+0x1B4` CHudStanceStrip, `+0x1B8` CTextListWin combat log, `+0x1BC` CHudMessagePanel, a
UIControl740208 camera child; personality labels "pers *" (`+0x1F0`). `Update` 0x638280: the controlled units come from the client
player's unit list (hero first; `UnitIdLists_Equals` 0x6381E0 detects changes -> `BuildUnitPortraits`, camera carrier = the hero), the
leave-zone prompt (`Game_CollectHeroesInZoneExit` -> box "leave_zone", 0x10001 -> `Session_RequestMapChange(+0x188)`), quest sounds
(quest.wav / subcomplete.wav / complete.wav). Slots 28..35: `OnLootMessage` 0x636150 (items / money into the message panel),
`OnUnitAdded` 0x6361B0, `OnUnitRemoved` 0x6364E0, `ShowGameOver` 0x6365E0 ("game_over" / "end_of_game", base 0x90000 -> leave),
`OnUnitEvent` 0x6366F0 (unit voices, "say" lines into the log), `ShowQuestMessage` 0x6369D0 ("combat_get_quest" /
"combat_complete_quest" / "combat_failed_quest" + title), `OnZoneEnter` 0x6360F0 (autosave "savea"), 40 `UpdateUnitList`.

**Selection** [V]: `SelectUnitIndex` 0x6379E0 (shift toggles; `CWorldClient::HideObject` = CUnit flag 0x10 = selected, net message 0x19;
host party records `+0x778` mirror it), `DeselectAll` 0x637C60, a rubber band (`+0x1C1`, rect `+0x1CC`, released in `OnLButtonUp`
0x638B30 -> `CollectVisibleRecordObjects`), keys select1..3 / select_all, portrait clicks (CHudMainPanel 0x641E20). `RefreshPanels`
0x637DC0 feeds the first selected unit to the four bars.

**A click becomes an order** (`IssueCommandAt` 0x638EF0 (window, target object or 0, x, y), called by `OnLButtonUp` with
`PickUnitAt`) [V]: no selected unit -> nothing; alt (`CInterface +0x7E`) -> `SendMoveOrder(point, formation 0, shift)`; a held
called-shot key -> `SendAttackOrder(target)` or `SendMoveOrder(point, 1)`; then by `+0x17C` unless ctrl: 0 / 1 `CastOnTarget` 0x637500
(`SendMsg33_CastOn(slot, unit, target)` per selected caster, usability -> portrait flash), 2 `CastAtPoint` 0x637750 (`SendMsg32_CastAt`),
3 / 4 the belt item variants (`SendMsg33_UseWeaponOn / SendMsg32_UseWeaponAt`), 5 `SendMsg36_InteractOne(first unit, object, 0,
+0x184)` (use / science), 6 the same for objects with vt+0xA8 (loot), 7 `SendMsg39_SetLeader(unit, target)` + `OnUnitEvent(unit, 7)`
(follow), 8 InteractOne with -1; default: a controlled unit = `SelectUnitIndex`, a usable object = `SendMsg36_InteractOne(.., 0, -1)`,
terrain = `SendMoveOrder` 0x636F10 (`SendMsg30_Move(ids, shift, pos)` or `SendMsg3A_MoveFormation`; a zone exit under the point sets
`+0x188 / +0x18C`) + console "Terrain Coords x y z", another unit = `SendInteractOrder` 0x6373A0 (`SendMsg36_Interact(ids, 0, id,
target)`) and, when `CUnit::IsPlayerInMask` says hostile, `SendAttackOrder` 0x637190 (`SendMsg31_Attack(ids, shift, target, hit
location 0..5 or 6 any)`, warning event 10 when the target is > 5 levels above) else a move to it. Every order resets `+0x17C / +0x180 /
+0x184` and the slot highlights. `SetCursorTarget` 0x639EF0 picks the cursor frame per mode (attack, use 4, exit 0xF, forbidden 0xD,
select 0x18).

**Keys** (`OnKeyDown` 0x63A560, keyboard.ini actions) [V]: Esc cancels the command mode or pushes CEscMenu; 1 pause (clock.wav,
`Game_SendLeaveFlag(!paused)` = message 0x10), 2 decel = normal (tick g_0x78FF34 = 0x37 ms), 3 accel = fast (0x1B ms, single player), 4
obj = CQuestLogWin, 0xB camera_track (carrier = first selected / -1), 0xC camera_norm, 0x11..0x16 called shots held (`+0x18D..`),
0x17..0x1A crawl / sneak / walk / run (`SendMsg35_SetStance 0..3` per selected unit), 0x1B swarm = the battle button, 0x1C follow / 0x1D
use_science (`SelectAbilityRow` 0x63FB50 -> mode 7 / 5), 0x1E..0x25 spell1-8 (`UseSpellSlot` 0x63EF60: self / party-target spells cast at
once, others enter mode 1 / 2 via `SelectSpellSlot` 0x63EC90; nomagic.wav when unusable), 0x26..0x29 weapon1-4
(`CHudWeaponBar::SetSlotSelected` + `SendMsg34_SelectWeapon`), 0x2A..0x2D item1-4 (`UseBeltSlot` 0x63E020 / `SelectBeltSlot` 0x63DD70:
mode 3 / 4), 0x2E..0x31 w_info1-4 (unit info tabs), 0x32 / 0x33 w_text1-2 (message panel tabs), 0x34 w_minimap (sling toggle), 0x35
quicksave, 0x36 quickload, 0x37..0x39 select1-3, 0x3A select_all, 0x3B tutorial_script (`ShowQueuedTutorial(1)`). Lobby: Enter chat,
Backspace clears it.

**The strips** [V]:

| Widget | Buttons and effects |
|---|---|
| CHudWeaponBar 0x63BAB0 | 4 slots at ((i*5+10)*8, 0x1EF)-((i*5+15)*8, 600); release = `SetSlotSelected` + weapon.wav + `SendMsg34_SelectWeapon(unit, slot)`; follows `CUnit +0x270` in Update |
| CHudQuickBar 0x63C800 | 8 belt slots (tooltips `SlotTooltipText` 0x63CEE0 "<name> %d/%d (%d)" charges); release = `UseBeltSlot`, right / shift = `SelectBeltSlot`; highlight = alpha 0.65 / 0.55 (0x63CD20) |
| CHudSpellList 0x63E2A0 | spell rows x 0x2F8..800 from y 0x1D6 (0x28 each, "%s %d" name / cost, "school_%d" tips); release = `UseSpellSlot`, right = `SelectSpellSlot` |
| CHudAbilityList 0x63F300 | rows: 0 follow ("skill0009"), 1 Science ("skill0011") -> `SelectAbilityRow` |
| CHudMainPanel 0x63FE70 | portraits (0x78-byte records: tilt quaternion, flash, HP / mana bars, face "me"/"fa"/"th" by state) built by `BuildUnitPortraits` 0x6407E0; click = portrait unit as the click target (lobby: `SendNamedEvent("%d")`); frame "battle00", minimap mesh (`CHudMinimap::Init` 0x642E80, markers `DrawMinimapUnits` 0x643DC0) |
| CHudMinimap 0x645060.. | 0 sling toggle (scroll direction), 1 / 2 map click -> `CCamera::SetCarrierId(-1)` + `SetTarget(world point)`, 5 reset yaw; wheel = zoom `+0xD8` |
| CHudUnitInfoPanel 0x6456E0 | sliding panel (0.3 s): 0 slide toggle (sling.wav), 1..4 tabs (w_info keys): figure with damaged parts, bars, body parts "skull/torso/arm/leg" (0x647B70), stats "move_%d", "dmg_%d", "consider_more%d / less%d" (0x647040), spell info (0x646B90) |
| CHudClockStrip 0x648520 | (0x2D0,0x208)-(800,600): 0 pause (clock.wav, `Game_SendLeaveFlag(!paused)`), 1 normal (g_0x78FF34 = 0x37), 2 fast (0x1B, single player), 3 the time dialog (CQuestLogWin); clock hands from "gtime" (0x6488E0); lobby: no speed buttons |
| CHudStanceStrip 0x649190 | (0,0x208)-(0x50,600): 0 = the battle button (`OnSelect` 0x649810: if every selected unit has `CUnit +0x438` == 1 -> 0 else 1, `SendMsg3B_SetField438(unit, v)`; the "battle mode" / Regroup of `unit-ai.md`), 1..4 crawl / sneak / walk / run = `SendMsg35_SetStance`; marker from `CUnit +0x268` |
| CHudMessagePanel 0x649AF0 | (0xB4,0)-(0x26C,100): 0 slide, 1 / 2 tabs (w_text keys); the combat log text `+0x164` (`AddCombatMessage` "combat_picked_up" / "combat_crippled", `AddMoneyMessage` "format_money", `FlushCombatMessages` 0x64A7A0) via `CTextListWin::Refresh` |

## 13. The generic controls (0x65AEA0-0x65EF80)

| Control | Construction and behaviour [V] |
|---|---|
| CScrollBar 0x65AEA0 (vt 0x742C18 / 0x742BE4, 0x18C) | (ui, l, t, r, b, depth, max, step): art "Scrollbar", quads track / thumb / up / down, buttons 0 up, 1 down, 2 track. `SetValue(max)` 0x65AFE0 sets the range (0 hides the bar), `SetPosition(v)` slot 25 (0x65B0C0) clamps and moves the thumb, `GetChanged(&v)` 0x65B090 returns 1 once; thumb drag (0x65B240 / 0x65B1A0), held arrows repeat step * 16 / s and the wheel adds step per notch into the accumulator `+0x188` (0x65B360, 0x65B430) |
| CHScrollBar 0x65BC30 (0x742CBC / 0x742C88) | the horizontal twin (quads rotated 90 deg, 0x65B8B0); the options sliders |
| CTextListWin 0x65CC00 (0x742E9C / 0x742E68, 0x1C8, a CScrollBar) | (ui, text, l, t, r, b, depth): the text is tokenised by `ScrollText_ParseEscape` 0x65D970 (`\n` paragraph, `\aF<d>` font, `\aC<rgb bytes>` colour, other `\a` = fatal "ScrollText::Incorrect special symbol") into word nodes (width by MeasureText) and format nodes (`FormatLines` 0x65D4B0), wrapped to width - 14 (`Relayout` 0x65DAA0, scroll range = total height - view), drawn line by line with full justification of inner lines (`Redraw` 0x65D150). `SetText` 0x65CFE0 restarts at 0, `Refresh` 0x65CF30 appends keeping the view anchored |
| CListBoxWin 0x65BCD0 (0x742D5C / 0x742D28, 0x174) | (ui, l, t, r, b, first string): a one-line switch: `AddString` 0x65BE70, a click cycles `SetSelection(sel + 1)` 0x65BEE0 (wraps), `GetChanged` 0x65BEB0 |
| CEditWin 0x65C010 (0x742DF4 / 0x742DC0) | (ui, l, t, r, b): `Init` 0x65C1A0 (char mask: 1 letters, 2 digits / space, 4 other, 8 . :, centred, font, max chars, text colour, caret colour; defaults 0xF / 0 / 1 / 50 / 0xFFFFFF / 0xFF); `OnChar` 0x65C930 inserts at the caret when the class is allowed, ctrl is not held, length < max and the text fits the rect (MeasureText); `OnKeyDown` 0x65C610 Backspace / Delete / Home / End / Left / Right; the first key on a fresh box clears it (`BeginEdit` 0x65C5C0); `SetText` 0x65C290, `SetActive` 0x65C2D0; the owner polls the dirty flag `+0x17A` |
| CTooltipControl 0x65E320 (0x742F64 / 0x742F30) | (ui, owner, text, normalLines, actionCode): lines split at `\n`; `OnMouseMove` 0x65E580 is the state machine: armed -> after tooltip_time g_0x79BE80 seconds a surface is created at the cursor (+ hot spot g_0x7C8258), kept inside 800x600, with " (<key>)" of the bound action appended to line 0; fading out over `+0x17C`; `Redraw` 0x65E880: fill 0x1C2B, frame 0x31B3FF, lines in 0xA7D7E4 then 0x31B3FF from `+0x164` |
| CRichEditSurface 0x65ED30 (0x742FD8, 0x58, a CUIBlitSurface2) | a hidden Win32 "RichEdit20A" (Riched20.dll) used as a text formatter: `StreamInFile` 0x65EE70 / `StreamInBuffer` 0x65EF80 / `StreamInFromResource` 0x65EF30 send EM_STREAMIN (0x449, SF_RTF); the credits scroller (`RichEditScroller::FormatRange / Draw / Start` 0x65F020.., outside this range) renders it |
| DInputMouse 0x65EAD0 | DirectInput 7 mouse: `Create` (GUID_SysMouse, c_dfDIMouse, NONEXCLUSIVE | FOREGROUND, fatal messages), `Acquire` 0x65EC70, `GetState` 0x65ECB0 (dx, dy, wheel, buttons at `+0x10`), `Release` 0x65EC30; polled by CSoftCursor (framework) |

## 14. What is not known

- The tooltip text indices passed to `UIButton::SetTooltipById` (0x4E84.., 0x9CA4.., 0xEAC4.., 0x111D4.., 0x138E4.., 0x7594..)
  are resolved by the framework; the mapping index -> text key is not read here [?].
- ~~GetMagicTotals second output~~ settled [V] from the machine code (0x610020: `call UnitStats_MaxMana`, `fstps`, `call
  UnitStats_ManaCostFactor`, `fdivrs`, `_ftol`): `trunc(MaxMana / ManaCostFactor)` = max mana x intelligence / 25, a QUOTIENT (the
  earlier "product" inference was wrong).
- `CTooltipControl` size (0x184 assumed), `CEditWin` size (0x17C assumed): their `operator new` sites are in the framework.
- Where the option globals are persisted (keyboard.ini is written by the framework's key-table code; the options ini writer was not found
  in these ranges).
- `CSaveLoadWin::ScanSaves`: the number parsed from the folder names (the next "save<n>") and the info.sav version share one decompiler
  variable; the version gates (time at v >= 2, label at v >= 3) are [I].
- `COptionsScreen::GetTutorialKey` 0x650900 returns "options" only while waiting for a key — as coded, probably a bug of the original.
- ~~CCampScreen2 +0x1A0..+0x220~~ settled [V]: the full CCampScreen2 layout (+0x164..+0x21C, 33 fields) is in structs.tsv (struct
  completion 2026-11-02). The 0x168..0x18F region of `CCampScreen` (the `CUIWindowWith2Lists28` base lists) is still not mapped.

## 15. Corrections to earlier notes

| File | Wrong | Right |
|---|---|---|
| findings.md (0x60E000 section) | "CSaveScreen (vt 0x740B3C)" | vt 0x740B3C is `CWorldMapScreen` (Build 0x62D350 reads map.txt, "Map<allod>" figures); the save / load window is vt 0x741388 (0x6070A0..0x657820) |
| findings.md / functions.tsv (0x606F40) | DialogScreen::OnCommand 0x20001 "opens the 0x194-byte quest / topic window vt 0x741388" | 0x741388 is `CSaveLoadWin` pushed in save mode; the quest log is vt 0x7411BC (0x1F0 bytes) |
| functions.tsv 0x603B90 | "DialogScreen::DtorBody" | it is `CQuestLogWin::DtorBody` (frees +0x1CC / +0x188 / +0x184 / +0x170 / +0x164; called by the vt 0x7411BC deleting dtor 0x603B70) |
| functions.tsv 0x654430 | "COptionsScreen::OnClose" | vt 0x741388 slot 26 = `CSaveLoadWin::OnClose` (releases the screenshot figure +0x184) |
| functions.tsv 0x65BEB0 | "CHScrollBar::GetChanged" | reads +0x15C / +0x160 of a `CListBoxWin` = `CListBoxWin::GetChanged` |
| functions.tsv 0x65EE70 / 0x65EF80 | "EM_FORMATRANGE (0x449)" | 0x449 is EM_STREAMIN (SF_RTF): the functions load text into the rich edit (`StreamInFile` / `StreamInBuffer`) |
| functions.tsv 0x65EF30 | "DInput_EnumDevices" | `CRichEditSurface::StreamInFromResource`: locks a resource (vt+0x14 / +0x18) and streams it in; the only caller is `CCreditsScreen::Draw` |
| functions.tsv 0x65EA50 / 0x65EA80 | "CTutorialWin::Draw / UpdateElements" | vt 0x742F64 slots 20 / 22 = `CTooltipControl::Draw / UpdateElements` (draw only in states >= 2) |
| functions.tsv 0x610020 | "sum mana / magic skill values" | the party **maximum** of the magic skill and of the mana (`GetMagicTotals`) |
| functions.tsv 0x612290 / 0x612500 / 0x612600 / 0x6129C0 | classes CCampSpellConstr / CCampItemConstr | `this` is the `CCampScreen` (+0x1C0 / +0x1D4 / +0x214 / +0x218); renamed `CCampScreen::RefreshSpellComponents / DeconstructSpell / RefreshItemComponents / DeconstructItem` |
| batch-screens.tsv (CEscMenu::OnLButtonDown row) | "COptionsScreen (0x418, +0x3F4 = 1 in-game)" | the in-game flag is +0x3F5 (slide-in); +0x3F4 is the waiting-for-key flag and is zeroed by Build |
| findings.md (0x645000 section) | "CRichEditSurface (Riched20, EM_FORMATRANGE)" | EM_STREAMIN here; the EM_FORMATRANGE / EM_SETTARGETDEVICE calls are `RichEditScroller::FormatRange` 0x65F020 |
| items.md (camp calls) | "CCampItemConstr::Deconstruct / TryConstruct" | same functions, now `CCampScreen::DeconstructItem` 0x6129C0 / `RefreshItemComponents` 0x612600 (behaviour as described there) |

## 16. Function index (one line each; the batch rows carry the detail)

| Address | Name | What |
|---|---|---|
| 0x6024B0 | DialogScreen::Build | zone talk mode: text / topic panels, camera, lobby bar |
| 0x6030F0 / 0x603360 | DialogScreen::OnLButtonDown / DblClk | click a unit -> topics, terrain -> Order_MoveTo (run on double click) |
| 0x6034A0 | DialogScreen::OnKeyDown | Esc / Space / Enter per mode, obj / quicksave / quickload keys |
| 0x604130 | DialogScreen::Update | talk markers (SendMsg19), zone exit, walking, phrase timing |
| 0x604C50 | DialogScreen::ParseDialogScript | #nolips #show #hide #animation #camera #phrase, rewards, camera shots |
| 0x6064E0 | DialogScreen::SelectAnswer | topic -> briefing / constr -> camp / goodbye |
| 0x607BB0 / 0x607EC0 | DialogCamera::FocusOn / SetShot | camera cuts = SendMsg0F_Move; shots 1..12 |
| 0x608210 / 0x608450 | DialogScreen::StartBriefing / StartDialogLine | begin / end of a briefing (script events on end) |
| 0x608D20..0x609900 | CLobbyStatusBar | multiplayer portrait strip over the dialog |
| 0x60C870 / 0x60E6B0 | CCampScreen::Build / OnClose | merchant, stocks, baskets, camp units; teardown |
| 0x60E9E0 | CCampScreen::OnButton | tab switch, leave |
| 0x60F470 / 0x60F600 | CCampScreen::OnLButtonDown / OnKeyDown | confirm / cancel / hero arrows / tabs; Esc |
| 0x60FAB0 | CCampScreen::GetItemPrice | price by coefficient kind 0..8 |
| 0x60FCA0 / 0x60FE40 | CCampScreen::PutOnItem / UnequipItem | hero equipment edits |
| 0x610020 | CCampScreen::GetMagicTotals | party max magic skill / mana |
| 0x6100E0 | CCampScreen::CanDropItem | drop rules per tab |
| 0x610920 / 0x610AD0 | CCampScreen::EquipItem / OnDropItem | item moves between party, stock, baskets, components |
| 0x611220 / 0x611760 / 0x611930 | CCampScreen::OnBuyOrRepair / OnCancel / EnterMode | confirm, cancel, totals |
| 0x612290 / 0x612500 | CCampScreen::RefreshSpellComponents / DeconstructSpell | spell constructor |
| 0x612600 / 0x6129C0 | CCampScreen::RefreshItemComponents / DeconstructItem | item constructor |
| 0x612CF0 | CCampScreen::FillConstructorStock | cheat / default constructor stock |
| 0x613EA0..0x619150 | CCampScreen2 | multiplayer party / hero creation |
| 0x61B2C0..0x61D780 | CLobbyScreen | server list, addrbook.ini, host / join |
| 0x620380..0x621F30 | CCampBar2 | item strip with category tabs |
| 0x622000..0x622D40 | CCampBar / CCampQuickBar | hero slots |
| 0x622DB0..0x624CB0 | CUnitInfoWin | hero stats pane |
| 0x624F10..0x626FA0 | CCampSkillsWin | skills / perks editor |
| 0x6270A0..0x6298B0 | CCampTradeWin / RepairWin / SpellConstrWin / ItemConstrWin | tab panes |
| 0x62A830 | CFreeCameraScreen::Build | observer camera screen |
| 0x62AB20..0x62B300 | CEscMenu | in-game menu |
| 0x62B7D0..0x62C010 | CCreditsScreen | Bink + rich-edit scroller |
| 0x62C250..0x62D210 | CExchangeScreen | lobby item exchange |
| 0x62D350..0x6312C0 | CWorldMapScreen | global map, travel, map.txt readers |
| 0x6326B0..0x6335E0 | CMainMenu | main menu |
| 0x6337B0..0x633EC0 | CDifficultyBox | new-game difficulty |
| 0x634650..0x634E60 | CMessageBox | generic box |
| 0x635250..0x63B920 | CBattleHud | HUD, selection, orders, keys |
| 0x63BAB0..0x64AB20 | CHud* widgets | weapon / quick / spell / ability / portraits / minimap / unit info / clock / stance / messages |
| 0x63B5D0 / 0x64BE40 | CQuestLogWin ctor / SetCaption | |
| 0x64BE80 / 0x64CC10 | CQuestLogWin::Build / ShowMap | window + one zone |
| 0x64D9B0 / 0x64DC60 / 0x64EA80 | CQuestLogWin::DrawQuestList / FillObjectives / LoadQuestMarkers | quests |
| 0x64E330 / 0x64E5A0 / 0x64E760 | CQuestLogWin::OnLButtonDown / OnKeyDown / Update | travel OK, selection, blink |
| 0x6503C0 | COptionsScreen::Build | |
| 0x650A30 / 0x650C20 / 0x650E40 | COptionsScreen::Update / OnLButtonDown / OnKeyDown | live preview, OK / cancel, key capture |
| 0x651600 / 0x651940 / 0x651D70 | COptionsScreen::BuildKeyList / BuildOptionsList / SetTab | |
| 0x652000 / 0x652200 / 0x6524A0 | COptionsScreen::ApplyPendingKey / LoadFromGlobals / ApplyToGlobals | |
| 0x6539C0 / 0x653A50 | SaveSlotRecord::Serialize / MakeLabel | info.sav |
| 0x653DD0 | CSaveLoadWin::Build | |
| 0x654660 / 0x6548A0 / 0x655050 | CSaveLoadWin::OnLButtonDown / OnKeyDown / OnChar | |
| 0x655290 / 0x655550 / 0x655A10 / 0x655B20 | CSaveLoadWin::OnConfirm / DoSave / OnLoad / DeleteSelectedSave | |
| 0x655D40 / 0x6567A0 / 0x656E10 | CSaveLoadWin::ScanSaves / RefreshSaveList / UpdateSaveScreenshot | |
| 0x6570E0 / 0x6575B0 | CSaveLoadWin::QuickSave / SaveGame_BeginLoad | saveq / savea / saveb |
| 0x658E10 / 0x659420 | CTutorialScreen::Build / BuildPage | tutorial pages |
| 0x659190 / 0x6592C0 | CTutorialScreen::OnLButtonDown / OnKeyDown | |
| 0x65AEA0..0x65BC90 | CScrollBar / CVScrollBar / CHScrollBar | |
| 0x65BCD0..0x65C000 | CListBoxWin | |
| 0x65C010..0x65C930 | CEditWin | |
| 0x65CC00..0x65DAA0 | CTextListWin + ScrollText_ParseEscape | |
| 0x65E320..0x65EA80 | CTooltipControl | |
| 0x65EAD0..0x65ECB0 | DInputMouse | |
| 0x65ED30..0x65EF80 | CRichEditSurface | |
