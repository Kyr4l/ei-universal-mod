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
   directly and sends nothing until the player leaves (`Game_SendEnterWorld`); the dialog talks to the server with `Order_MoveTo`,
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
| art | figure "saveload"; text panel quad (200,430)-(600,570) (colour 0x80000000) + border strips (195,425)-(605,575); topic panel quad (155,416)-(645,580) (0xA0000000) + border (150,411)-(650,585) [V, deep read 2026-11-06; the old row had the topic rect garbled] |
| surfaces | [0] text (200,0x1AE)-(600,0x23A), [1] topic list (0,0)-(0xFA,0x1E) |
| controls | `+0x260` CScrollBar (0x248,0x1B8)-(0x252,0x230); `+0x25C` CTextListWin (0xA5,0x1AA)-(0x27B,0x23A) line 10 |
| state | `+0x16C` mode = 0, `+0x168` = 1, `+0x164` = 0 (**never written again**, see Update); camera distance g_0x79BD34 saved in `+0x268` and set to 4.0; `CMusicSystem::PlayBriefing(zone record +4, or "gipat" when the zone has no record)` (was named CMusicZone::PlayBattle); `+0x27D` = zone record +0x44 (single-speaker zone); `+0x27E` = 1 only on a fresh build (UI stream g_0x7AFDF8 empty) when the client mode (connection `+0x1C +0x104`) is not 5; on a load the stream is emptied and `+0x27E` stays 0; `+0x280` CLobbyStatusBar when Lobby_IsAlive (then window slot 36(1)); `+0x284` = the ids of the client player's units that pass `IsHeroId` (1e9 <= id < 2e9) |
| camera | when the zone record byte `+0x34` is set: `CCamera::SetLeash(point +0x38 / +0x3C, radius +0x40)`; camera flag 0x40 of g_0x79B2B0 cleared, 0x200 set, `FollowUnitId(-1)` |
| children | a 0xC-byte camera link (vt 0x741154) and a `CamCtlFree` camera controller |

Modes `+0x16C` [V]: 0 world, 1 topic list, 2 walking into place, 3 briefing text running, 4 line finished.

| Event | What happens (address) |
|---|---|
| L click, mode 0 | unit under the cursor whose server brain is idle (`+0x190` 0 / 0xB and `+0x1B4` == 9) -> click1.wav + `BuildAnswerList` 0x608B60 (topics = every `b.<unit>.<topic>` variable == 1, "goodbye" appended, 5 row buttons (0xD2,0x1B8+0x18 i)-(0x23A,..), mode 1); else terrain point -> `Order_MoveTo(player brain, pt, run=0)` unless `+0x27D` (0x6030F0) |
| double click, mode 0 | same, but a terrain point runs (`Order_MoveTo(.., 1)`) (0x603360) |
| L click, mode 1 | row i + page `+0x264` -> `+0x184` = topic, `SelectAnswer(0,1)` 0x6064E0 |
| L click, mode 3 / 4 | 3 = `ParseDialogScript(0)` next phrase; 4 = `StartDialogLine` (end the briefing) |
| keys (0x6034A0) [V, deep read 2026-11-06] | while the lobby modal flag `+0x2A0` is set only Esc does something (clears it). Esc: mode 0 pushes CEscMenu, 3 skip the rest (as Enter), 4 end (`EndBriefing`), 2 `SelectAnswer(1,0)` (stop walking, start at once), 1 -> mode 0; Space: 3 next phrase (`ParseDialogScript(0)`), 4 end, 2 `SelectAnswer(1,0)`; Enter (single player): 3 = click1.wav + `ParseDialogScript(1)` repeated while the mode stays 3 (the whole remaining text and the rewards appear at once), 4 end; lobby: Enter opens the chat, Backspace clears it. keyboard.ini actions in mode 0: 4 (obj) pushes CQuestLogWin `SetCaption(current map, 0, 0, allowTravel 1)` for the first variable `q.<zone>.<x>` (3 parts) == 1.0 whose zone record has the current zone's name (any zone in a lobby); 0xB camera follows the player unit; 0x35 QuickSave("saveq"); 0x36 SaveGame_BeginLoad("saveq") |
| mouse move (0x603D00) [V, deep read 2026-11-06] | lobby modal -> cursor 0xD and forwarded to the box; mode 1: hovered row (button 0..4 + page `+0x264`, below the topic count) -> `+0x188` + DrawTopicList; mode 3 cursor 0xE, mode 4 cursor 0; mode 0 with no mouse button held: a living unit (vt+0xDC == 0, not the own lobby unit) whose server brain is idle (`+0x190` 0 / 0xB and `+0x1B4` == 9) -> `LoadSpeakersFor(id)` (its topics), cursor 0xE only when it has topics, its name (cached map `+0x1D8`: id -> vt+0x1C name) always drawn in beige at (x-0x7D, y-0x14); else a zone exit under the terrain point -> cursor 0xF when `z.<target of exit +0x164>` != 1 (the hovered exit's own index is computed and **ignored**: the test always uses exit `+0x164` = 0) |
| OnCommand 0x606F40 | 0x10001 (leave-zone yes) = `Session_RequestMapChange(+0x164)`; 0x20001 = push CSaveLoadWin in save mode; 0x90001 / 0x100001 = leave the game (`CInterface +0x44` = 1, `LobbySession_ReleaseActive(2)`) |

`SelectAnswer(isBriefing, savePoses)` 0x6064E0 [V, deep read 2026-11-06]: `+0x27C` = isBriefing; no player unit `+0x1A4` -> mode 0.
"goodbye" -> mode 0. A topic whose first 6 characters are "constr" (case-insensitive `_stricoll` of `CString::Left(6)`; the
lower-casing `CString::MakeLower` 0x700EFE (was named MakeUpper) belongs to 0x6078C0, not here) -> merchant id = `atol(topic.Mid(7))`
(so "constr_5" -> 5; 0x6FD9B4, named `CString::Left`, is `CString::Mid(nFirst)`); when `g_0x7AA5D8` has that merchant,
`World_SetConstrCurrent(id)` + `Game_SendLeave(5)` (enter the camp); mode 0 either way. Otherwise the text `briefing <topic>` is
opened (missing -> mode 0) and **every** `#show` line of it is scanned: the first live unit named there (`Game_FindUnitById(hash of
the name)`) that is not the player's unit becomes the second speaker `+0x1A8`, any later different one the third `+0x1AC` (the last
one wins). **No second speaker -> mode 0 and nothing happens** (a briefing needs at least one live non-player unit in its `#show`
lines; NPC portraits do not count). Geometry, d = second speaker proto `+0x28` (radius) + 2.5 m: in a normal zone the player walks to
the point d along the second speaker's forward axis from it (`+0x1BC`) and faces it (`+0x1B0` = the second speaker's position); in a
single-speaker zone (`+0x27D`) the roles swap: the second speaker walks to d along the player's forward axis and faces the player.
The third speaker's spot `+0x1C8` is the midpoint of the pair moved sideways by half the pair distance (the midpoint plus the
half-vector rotated 90 deg), facing the midpoint. Unit vectors (z = 0): `+0x1D4` = `+0x1BC - +0x1B0`, `+0x1E0` the reverse,
`+0x1EC` = midpoint - `+0x1C8`. With savePoses the third (and in a single-speaker zone the second) speaker's position and reversed
forward axis are kept in `+0x210` / `+0x228`; a still pending walk node of those units is dropped and, when its byte `+0x28` is 0, its
target is taken as the pose to restore. `PlaceUnit` 0x6089D0 (unit, pos, dir, waitFlag 1, teleport = isBriefing, run 0): a 0x30-byte
node in `+0x240` (state 1) + `Order_MoveTo`, or an instant move. Mode = 2 (walk; Update starts the briefing) or, with isBriefing,
`StartBriefing` 0x608210 at once. The topic-row click calls `SelectAnswer(0, 1)`, Esc / Space while walking `SelectAnswer(1, 0)`.

`ParseDialogScript(skipToEnd)` 0x604C50 [V, deep read 2026-11-06 with the machine code] continues the open `briefing <topic>` text
**up to and including the next `#phrase`** (one phrase per call; the caller calls again when the phrase is over). The directives
are case-insensitive and apply to the phrase that follows them (their values are reset on every call):
- `#nolips`: this phrase without lip movement, and its text in grey.
- `#show <name> [index] [weapon]`: `AddSpeaker(name, index - 1)` 0x605DE0 (-1 without an index). The third argument is **never
  read** by AddSpeaker, so `weapon` changes nothing (the line only makes a second, identical call). A live unit (by the hash of the
  name) is appended to the speaker vector `+0x294` (an earlier entry of the same unit is removed first), `SendMsg19(1, unit id)` is
  sent, `CUnit::SetVisibleToAll(0)` when it is the player's unit, brain `+0x978` = callback 0x6023F0. Any other name is an **NPC
  portrait** and needs an index >= 0 (none = ignored): an older portrait of that name is removed, the name ('_' -> ' ',
  lower-cased) is looked up in the name -> number table `g_0x7C8764` (no other code references it; unknown names give nothing), the
  64x64 picture "quitem%04d" (number) is blitted into a 256x256 target on the quad "initqu%ditem" at slot (index mod 7) of the tables
  0x74111C (x) / 0x741138 (y), depth 12, turned by pi; index < 7 adds a dark backdrop (centre +-60) and a border (+-65), index > 7
  tints the quad 0x82FFFFFF.
- `#hide <name> [x]`: `RemoveSpeakerByName(name, skipToEnd, 1 when a second argument exists)` 0x6062C0.
- `#animation <n>`: the speaker plays model entry n (server unit `Stop`, done callback 0x6023F0, `QueueScriptOrder`); without it
  (n < 1) the talk animation `StartSpeakerAnimation(unit, 1, 0)`.
- `#camera <shot> [yaw-deg distance height]`: with all three numbers `FocusOn(unit, yaw * pi / 180, distance, height)` 0x607BB0 on
  the player (shot 2), the second (3) or the third speaker (4), any other shot = the pair's midpoint (unit 0); with fewer arguments
  `DialogCamera::SetShot(shot)` 0x607EC0 (shots 1..12: 1 both from 3 x d at 1.5 m, 2 player close-up -30 deg 4 m, 3 second +30,
  4 third, 5 both 2.7 d 15 deg h 3, 6 / 7 +-45 deg 3.5 m, 8 third 15 deg, 9 both 2.4 d -15 deg, 10..12 with the others sunk to -100
  by `World_SetUnitHeight`); no `#camera` (shot < 1): shot 1 for the first phrase of the briefing, else 2 when the speaker is the
  player, 4 the third speaker, 3 anybody else.
- `#phrase <name> [voice]`: the label is the name of the unit with that id (vt+0x1C), or, when that name is empty, the `pers` label
  of `+0x26C` keyed by the unit `+0xC`, or the raw name when no unit has it. The text = the following lines up to the next `#`
  (`ReadUntilHash`), appended to `+0x248` as `"\aC<FF B3 31>" <label> ":\n" "\aC<E4 D7 A7> " <text> "\n"` (golden label, beige
  text; `#nolips` uses grey `\aC<A8 A8 A8>`). Voice: only when not skipping and `voice` > 0: `Play2D("<r>\briefing\<topic>\<voice>.mp3",
  .., decompressMp3 1)` -> `+0x250`, where **r is "t" or "s" picked at random per phrase** (`(rand() * 2 / 32767) & 1` into the table
  0x793378); timer `+0x24C` = text length * 0.05 s (+ 1 s on the first phrase). Update moves on when the voice ends, or, without a
  voice, when the timer runs out.
After the phrase the speaker heights are reset, the camera is set (above) and every speaker record whose name equals the phrase name
gets the animation and the mouth flag (`+0x240 +mouthAnimActive` = not #nolips) and becomes `+0x254`; every other speaker goes idle
with a closed mouth. When no further `#phrase` follows (`SeekSection("#phrase")` from here) the briefing is over: mode 4 and the
**reward lines** of the briefing record `g_0x7C87D4[topic]` (the record `CWorldServer::OnNamedEvent` pays out at the end) in white
`"\aC<FF FF FF> %s %d\n"` / `" %s %s\n"`: exp `+0xC` > 0 "brief_get_exp" (trunc), money `+0x10` > 0 "brief_get_money", one
"brief_get_item" line per item name of `+0x14..+0x18` (`CreateItemByName` + item description), one "brief_lost_item" line per name of
`+0x20..+0x24` and, **in a lobby when the topic starts with "z"**, one per quest item (typeId 0x14) of player 0 (the host hands all
quest items in), "brief_get_quest" + the title (first line of "quest <c>") for every 3-part `a.b.c` name of `+0x2C..+0x30`,
"brief_complete_quest" the same for `+0x38..+0x3C`, and the text "brief_get_specials_<n>" when the count `+0x68` = n > 0.
`skipToEnd` (Enter / Esc in mode 3, called in a loop) skips the voice and the list refreshes, then at the end puts the whole text
in the list (`SetText`, scrolled to the bottom), closes every mouth, idles every speaker and removes the NPC portraits.

`EndBriefing` 0x608450 (was named StartDialogLine; it ends the briefing) [V, deep read 2026-11-06]: stops the voice, `Music_FadeIn`,
removes the NPC portraits; per speaker `SendMsg19(2, id)`, mouth closed, `SetVisibleToAll(1)`, height 0, done callback cleared,
server unit `Stop`; the speaker vector is emptied; the third speaker (and in a single-speaker zone the second) walks back to its
saved pose (`PlaceUnit`, no wait flag, no teleport; the player is not moved back). Then the `b.` variables of the host (or, in a
lobby, the client) table are scanned and **only the first** 3-part `b.<x>.<topic>` whose third part equals the topic (any value)
fires `CWorldServer::OnNamedEvent(player 0, name)` (in a lobby `Lobby_SendNamedEvent`; else the rewards of the briefing record, see
world-server.md): two variables naming the same topic still fire once. Mode 0, `SendMsg19(0, 0)`, camera back:
`SendMsg0F_CameraFromTo(+0x204, +0x1F8, +0x204)`, or after an automatic briefing (`+0x18C`) `SendMsg04_CameraAt(player position)` +
camera flag 0x40 cleared + `+0x18C` = 0.

Update 0x604130 [V, deep read 2026-11-06]: (1) every walk node of `+0x240` accumulates time `+0x2C`: state 1 (walking) becomes 2 when
the unit is within 0.1 m (xy) of the target and gets `Brain::OrderTurnTo(dir)`; state 2 becomes 0 when the forward axis matches the
direction (squared difference < 0.01); after 30 s the unit is moved there (`CollisionMap::MoveObjectAndRestamp`, yaw from the
direction, `ResetOrdersAndStance`) and the node ends; ended nodes are freed. (2) mode 1 shows the topic surfaces / scroll bar
(`GetChanged` -> `+0x264` + DrawTopicList) and then does mode 0's work; modes 0 / 1: the `b.` variables equal to 1.0 with 3 parts
whose third part does not start with "constr" and whose second part is the hash of one of the hero ids `+0x284` give the "has a
topic" set; every hero of `+0x284` that exists in the client world gets `SendMsg19(1, id)` when in the set, else `SendMsg19(2,
id)`; then, when `+0x168` is set and the client party has heroes: exit `+0x164`'s target (not "none") with `z.<target>` != 1.0 ->
when `Game_CollectHeroesInZoneExit(the party's first hero record, +0x164)` says every living hero stands in that exit: g_0x784750 = 0
and `Session_RequestMapChange(+0x164)`. Because `+0x164` is only ever set to 0 (Build), **talk mode leaves a zone only through
exit #0** [V: no other write in 0x6024B0..0x609D00; bug candidate or a design limit of the one-exit talk zones]. (3) mode 2 stays
until no walk node with the wait flag `+0x28` is still running, then mode 3 + `StartBriefing`. (4) mode 3 shows the text list; no
voice: the timer counts down and below 0 calls `ParseDialogScript(0)`; a voice: the next phrase starts when its channel is gone
(`FindChannelById` and `HasPendingSound` both 0). (5) mode 4 only closes the speaking unit's mouth the same way; the briefing waits
for a click / Space / Enter / Esc (`EndBriefing`). `OnPartyChanged` 0x602C90 auto-starts a briefing on zone entry when a `b.%s` key with 3 parts is 1.0
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
`+0x1A0`), `SelectNextHero` and the first enabled tab. A first visit (`CGSPlayer +0x38` set) computes per magic school 0..7 the
party's min and max `UnitStats_MagicSkill`, `CGSPlayer::SetShopLimits(min[8], max[8])`, `GenerateSpellShop`, `GenerateItemShop`
and clears `+0x38`. The flag is set on a new merchant and, [I], again for every merchant by `ObjPartyRecord::MarkChildrenChanged` 0x4A88E0, which the script `QuestComplete` calls: the shops would be re-rolled (`items.md` 6) on the first camp visit after each completed quest. `FillConstructorStock` 0x612CF0 (slot 39) has **no
caller** (no direct call; no virtual `+0x9C` call on a camp screen found) [V]: a leftover developer stock that clears both stocks and fills them
with everything: per weapon proto an "Instruction Weapon" (`+0x54` = row, count 10) plus one finished weapon per material whose
name equals the proto's M.Type `+0x10`; the same for armor protos whose name does not contain "prototype"; an "Instruction Quick
Item" (x10) per quick proto whose name contains "wand"; "Material.<name>" x20 per material; one "Rune" x10 per perk and one
"Prototype" x10 per spell. `OnClose` 0x60E6B0 unpauses, removes the camp
units, releases the previews and the 8 basket parties. Esc = leave (0x60F600). Lobby: Backspace clears / Enter opens the chat.

`OnButton(tab)` 0x60E9E0 [V, deep read 2026-11-06]: disables every pane, resets the party bar filter to 9 (all), then enables per tab:
0 unit info `+0x1D8` + hero slots `+0x1E4`; 1 unit info + quick bar `+0x1E8` + skills `+0x1F4`; 2 help `+0x1DC` ("camphelp" text) +
spell trade + spell stock bar, party bar filter 1; 3 spell constructor + spell stock bar; 4 item trade + item stock bar, party bar
filter 5; 5 item constructor + item stock bar; 6 repair + item stock bar. Tabs 2 / 3 need the spell shop `+0x195`, 4 / 5 / 6 the
item shop `+0x196`, and 3 / 5 also `+0x254` (i.noconstr) == 0; a disabled tab re-applies the current one. So **i.noconstr removes
only the two constructor tabs; trade and repair stay**. On a change: slot.wav, `+0x190` = tab, `CUnitInfoWin::SetMode(tab)`, the
title `+0x240[tab]` on surface [1] (0,10)-(200,0x1E), tab icons scaled 0.5 except the active one (1.0); the hero arrows are hidden
in tab 0 when there is no item shop or i.noconstr. Tab 7 = **leave**: `CCampSkillsWin::SelectNextAvailable(0, 1)` (uncommitted skill
edits are discarded), the buy baskets go back to the stocks and the sell / repair baskets to the party (`Party_MoveAllItems`), the
component lists empty item by item (`Party_TransferItem`: an item carrying the stock link (`+0x1C` link with flag `+0x10`) returns to
the stock, others to the party), transit.wav, then `Game_SendEnterWorld` (the camp party is uploaded and the zone spawns again) or,
when the camp was opened from the multiplayer hero editor (`+0x194` set by the inline ctor in `CCampScreen2::OnLButtonDown`),
`RequestPopScreen(0)`.

| Buttons (Build) [V] | rect / meaning |
|---|---|
| 0 / 1 | prev / next hero arrows (0xC,0x1C2)-(0x34,0x1EA) / (0x94..0xBC): press sets `+0x25C` = 1 / 2 and repeats every 0.5 s while held (Update 0x60F6B0); hidden in tab 0 without the item shop or with i.noconstr (OnButton) |
| 2 / 3 | confirm (300,0x1D6)-(0x14A,500) / cancel (0x1D6,0x1D6)-(500,500) -> `OnBuyOrRepair` 0x611220 / `OnCancel` 0x611760 when `+0x210 / +0x211` allow |
| 4..11 | 8 tabs across x 0x26C..0x30C (20 px each), y 0x1AE..0x1EA: 0 weapons (tip 0x4E84) 1 skills 2 spell trade 3 spell constr 4 item trade 5 item constr 6 repair 7 leave; disabled tabs fall back (`OnButton` 0x60E9E0) |
| sub-windows (ItemAtPoint slot 25, kinds) | party bar `+0x1E0` kind 1, spell stock bar `+0x1EC` kind 2, item stock bar `+0x1F0` kind 3, hero bar `+0x1E4` (`CCampBar`: weapons kind 2, belt kind 3, doll armor kinds 4..10; "kind 0" = hero slot in OnDropItem), spell slots `+0x1E8` (`CCampQuickBar`, kind 1), trade baskets kinds 0x11 / 0x1C, repair 0x19, spell components 0x13, item components 0xD |

**Drawing** [V, checked 2026-11-06]: `CCampScreen::Draw` 0x60F050 recomputes the hovered item `+0x168`, calls **`EnterMode` every
frame** (so totals and the confirm state always follow the baskets), draws the windows and, in tab 0 only, the selected hero
(`UIUnitPreview::Render(hero unit, +0x258, x = 400, sixRects = 1)`: drawn straight into the frame after a depth clear, no render
target). Every item figure of the bars and panes goes through `UIUnitPreview::DrawItemFigure` 0x5ECA10 (was GetOrCreatePortrait),
which asks the screen's slot 43 `CanDropItem(item, kind)`: a refused item gets a translucent dark-red overlay (0x82804040). So **the
red-tinted icons of the camp are exactly the drop rules below** for the current tab.

**Item moves** [V]: a click on an item of a bar reaches `CCampScreen::OnDropItem` 0x610AD0 (slot 42) with the bar kind; `CanDropItem`
0x6100E0 (slot 43) decides per tab (rules below); the move is `Party_TransferItem(item, from, to, flags)` or, in tab 0,
`PutOnItem` 0x60FCA0 / `UnequipItem` 0x60FE40 (`HeroRecord::AddWeapon / AddArmor + CUnit::EquipArmor / AddQuickItem / AddSpellItem`, 4
weapons, 4 quick, 8 spells, one armor per type; the first weapon becomes the slot in hand `+0x774`; `CUnitInfoWin::Redraw`). Shift
(`CInterface +0x7D`) moves a whole material stack. Every move calls `CCampBar2::RebuildList` on the bars concerned.

`OnDropItem(item, kind)` 0x610AD0 [V, deep read 2026-11-06]: a refusal of `CanDropItem` plays messbox\cancel.wav. Kind 0 (hero slot)
= `UnequipItem`; 1 (party bar) = `EquipItem` 0x610920 (per tab: 0 / 1 put on, 2 / 4 -> the sell basket, 3 / 5 -> the components, 6 ->
the repair basket); 2 (spell stock): tab 2 -> the buy basket `+0x1CC`, tab 3 -> the components with the stock link; 3 (item stock):
tab 4 -> the buy basket `+0x1B8`, tab 5 -> the components (with shift a material stack moves unit by unit while `CanDropItem(item, 3)`
allows); these play put_on.wav. The other kinds take an item back (put_off.wav): 4 / 5 a spell / item basket item returns to the stock
(buy basket) or the party (sell basket); 6 (spell components): a rune returns alone (stock if it carries the stock link, else party),
anything else (blueprint or container) **empties the whole list**; 7 (item components): a material returns (shift: every material),
then any spell container in the list goes back too; a container returns alone; a blueprint or usable item empties the list; 8
repair basket -> party. **Basket capacity** (`Party_TransferItem` 0x5EE1B0 (item, dst, src, count, shiftFlag, keepLink)): with
count n > 0 and the destination already holding >= n entries, the item only goes in when it stacks with one of the first n entries
(item vt+0x38), else nothing moves: the trade baskets take 8 different entries (count 8), the repair basket 16 (0x10); components and
stocks are unlimited (count 0). keepLink marks the item with a 0x18-byte `ItemLink` (`+0x1C`, flag `+0x10` = 1) = "came from the
stock".

**The merchants' price factors** [V, deep read 2026-11-03]: the 9 coefficients live in each merchant record (`CGSPlayer +0x10..+0x30`,
saved with the campaign by `CGSPlayer::Serialize` tag 4; no script command or other code writes them). `GSPlayers_CreateDefaults`
0x4A7630 sets them on a new game: buy 1.0, sell 0.5, construct 0.2 (charged per component), deconstruct 0.1 (charged on the item) (the same four for spells [0..3] and items
[4..7]), repair 0.2 [8]. Single player (`Session_ResetPlayers_Single`) creates merchants 1..5 and gives **merchant 5 all nine
factors 0** (everything at that merchant costs 0 and sells for 0; index 5 is the `b.golem.constr_5` building of the campaign start,
world-server.md 2.6 [I]); multiplayer (`Session_ResetPlayers_Multi`) creates merchants 1, 3, 4, 5, all with the standard factors.
Both shops (+0xC items, +0xD spells) start enabled for every merchant; the campaign toggles them later (the `i.on/offitemconstr`,
`i.on/offspellconstr` variables). Confirmed in a real save (`_cpr/claude-re/sav/save38_scenario.sav.raw`, the five records after
tag 4): merchants 1..4 hold 1.0 0.5 0.2 0.1 1.0 0.5 0.2 0.1 0.2, merchant 5 holds nine zeros; there merchant 1 has only the item
shop and merchant 2 only the spell shop. A mod can change the factors only by editing these records in the campaign save
(`scenario.sav`) or by patching the defaults.

**Prices** (`GetItemPrice` 0x60FAB0, slot 54, kind 0..8 = coefficient index) [V, machine code re-read 2026-11-06]: base = item
vt+0x40 `GetPrice` (one unit); wear = 1 - durability `+0x20` / max `+0x24`. Kind 8 (repair, any item kind) = 0 when wear == 0 or
coef[8] == 0, else max(1, trunc(base * coef[8] * wear)). Other kinds: weapon / armor (0x3004 / 0x3005) = trunc(base * coef[k] -
base * coef[8] * wear) (worn gear is worth less by exactly its repair cost), anything else trunc(base * coef[k]). All conversions are
`_ftol` = truncation toward zero (not rounding).

`EnterMode` 0x611930 [V, deep read 2026-11-06] recomputes `+0x20C` total, `+0x210` confirm / `+0x211` cancel allowed, `+0x212`
deconstruct, and draws on surface [0]: "<camp_current_money> %d" (party money; tab 1: "camp_current_exp" + the free experience) at
(0,10)-(200,0x1E), the title key at (0,0x1E)-(200,0x32) and the total "%+d" (or "0") at (0,0x32)-(200,0x46); the confirm / cancel
quads 0 / 1 are scaled 0.5 when not allowed. Per tab:
- 0 (item shop, no i.noconstr): "camp_repair_all" = sum of kind 8 over the selected hero's weapons `+0x730` and worn armor `+0x750`;
  confirm when > 0.
- 1: confirm = cancel = `CCampSkillsWin::GetPerkFlag` (something edited), total = `GetPointsLeft`, title "camp_level_up"; the money
  test is skipped.
- 2 / 4 (a basket not empty, confirm and cancel allowed): "camp_spell_trade" / "camp_item_trade", total = sum(kind 0 / 4 price *
  count `+0x48`) over the buy basket - sum(kind 1 / 5 price * count) over the sell basket (negative = the party earns money).
- 3 / 5 (components not empty, cancel allowed): every component costs kind 2 / 6 (the **construction** fee), plus the full buy
  price kind 0 / 4 when it carries the stock link (taken from the merchant's stock = bought now); counts are not multiplied. A
  spell container / usable item in the list switches to deconstruction: total = kind 3 / 7 of that item (the **deconstruction**
  fee) (+ kind 0 / 4 when from the stock), `+0x212` = 1, confirm allowed, title "camp_spell_deconstr" / "camp_item_deconstr";
  a blueprint calls `RefreshSpellComponents` 0x612290 / `RefreshItemComponents` 0x612600, whose result is the confirm flag and
  which build the preview `+0x218` / `+0x214` (title "camp_spell_constr" / "camp_item_constr" when it succeeds); total 0 when
  confirm is not allowed; with neither, the "camphelp" text is shown.
- 6 (repair basket not empty): "camp_item_repair" = sum(kind 8 * count).
Then (all tabs but 1) confirm is refused when the total exceeds the party money `+0x50`.

**Confirm** (`OnBuyOrRepair` 0x611220, buy.wav) [V, deep read 2026-11-06]: tab 0 (repair all, only with the item shop and no
i.noconstr) sets durability `+0x20` = max `+0x24` on the selected hero's weapons `+0x730` and worn armor `+0x750` (not the belt, not
the backpack); 1 commits the level-up (`CCampSkillsWin::SelectNextAvailable(1,0)`) and **returns without touching the money**; 2
spell trade: every sold item is restored to full durability and its prototype is **added to the merchant's allowed lists** (spell
`ShopStock` `CGSPlayer +0x1D4`: a container -> `AddSpellProto` of its spell, a blueprint -> `AddSpellProto(row +0x54)`, a rune ->
`AddPerkProto`), then the sell basket -> spell stock and the buy basket -> party (`Party_MoveAllItems`); 4 item trade: the same with
the item `ShopStock` `+0x11C` (usable weapon / armor / quick item -> `AddWeapon / AddArmor / AddQuickProto` by proto row `+0x18 >> 16`;
an instruction by its `+0x50` table and `+0x54` row), plain loot (kind 0x3007 category 0, e.g. trophies) is **deleted** instead of
going into the stock; so a sold prototype can appear in that merchant's next generated stock (`ShopStock::Generate*`, items.md; regeneration: see Build above).
3 = `DeconstructSpell` 0x612500 when `+0x212` (container -> a "Prototype" + one "Rune" per perk) else the preview `+0x218` replaces
the components; 5 = `DeconstructItem` 0x6129C0 when `+0x212` (item -> "Instruction Weapon/Armor/Quick Item" with `+0x54` = proto row,
one "material.<name>" per proto Components, a "spell container" with its spell) else the preview `+0x214` replaces the components;
6 restores the repair basket to full durability and returns it to the party; finally (all tabs but 1) party money `+0x50` -=
`+0x20C` (negative totals = income). **Cancel** returns the baskets / components to where they came from.

**Drop rules per tab** (`CanDropItem` 0x6100E0, slot 43 (item, source kind)) [V, deep read 2026-11-06, x87 parts from the machine
code]. Source kinds: 1 party bar, 2 spell stock bar, 3 item stock bar, 6 / 7 the spell / item component lists (taking back is always
allowed there).
- tab 0 (equipment): only weapons 0x3004, armor 0x3005, quick items 0x3006; a weapon / armor with round(durability `+0x20`) < 1 is
  refused when it comes from the party bar (broken gear cannot be put on).
- tab 1 (skills): from the party bar only spell containers, and only when the selected hero (slot 51 -> record, stats `+0x20`) can
  cast the spell: round(`UnitStats_MagicSkill(stats, SpellProto +0x14 school)`) >= spell `+0x50` reqSkill **and** trunc(spell mana
  `+0x40`) <= trunc(`UnitStats_MaxMana` / `UnitStats_ManaCostFactor`). Other sources: always.
- tab 2 (spell trade): spell containers, spell blueprints (`Item_IsLootCat2Sub3`), runes (`Sub4`); tab 4 (item trade): anything but
  kind 0x3009, spell containers, blueprints and runes; tab 6 (repair): weapons / armor with durability `+0x20` < max `+0x24`.

**Constructor rules** (tabs 3 / 5, sources 1 and 2 resp. 1 and 3) [V]: `GetMagicTotals` 0x610020 (slot 45: school -> the party
**maximum** over all heroes of round(MagicSkill(school)) = maxSkill and of trunc(MaxMana / ManaCostFactor) = maxMana).
- Spell: a container only into an empty component list `+0x1D4` (= deconstruct); a blueprint only into an empty list, when
  trunc(SpellProto `+0x18` mana) <= maxMana and SpellProto `+0x44` complex <= maxSkill (school of the blueprint); a rune (PerkProto
  row `+0x54`) only when a blueprint is in, no container, fewer than 8 runes, the blueprint allows the rune type (`+0x78[PerkProto
  +0xC]`, checked only for types 0..4 r / t / a / e / d) and, counting the new rune, blueprint complex + sum(PerkProto `+0x18`
  complex) <= maxSkill **and** trunc(blueprint mana + sum(PerkProto `+0x10` mana)) <= maxMana. So the limits are the best hero's
  skill and mana budget, not the selected hero's.
- Item: a blueprint (`Sub0to2`) or a usable item only into an empty list `+0x1C0`; a material only when no usable item is in and a
  blueprint is: the first material's name (MaterialProto `+4`) must equal the blueprint proto's material type (`+0x10`, `_stricoll`),
  later ones must be the same material row and the number of material entries already in must be < proto Components `+0x4C`
  (blueprint proto row `+0x54` in the weapon / armor / quick table by blueprint `+0x50` = 0 / 1 / 2); a spell container only when the
  preview `+0x214` exists, the container holds a spell, the preview is a quick item (0x3006) or the spell has perk ic / it
  (`CSpell::HasItemPerk`), spell reqSkill `+0x50` <= preview slots `+0x40` (an int field), spell mana `+0x40` <= preview max mana
  `+0x2C`, and no container is in yet. `RefreshItemComponents` then drives `CItemConstructor` (`BeginCreateItem(kind, -1) / SetProto /
  SetMaterial / SetSpell / FinishItem`, `items.md`).

**The bars** [V]: `CCampBar2` 0x620380 (vt 0x74197C; `Build(top)`: a 100-px strip at y 500 (arg 0, depth 4) or y 0 (arg 1,
depth 8): scroll arrows buttons 0 (0x1E..0x32) / 1 (0x2EE..0x302) with auto-repeat, 6 category icons 2..4 (left column x 0..30) and
5..7 (right column x 0x302..800, three 30-px rows each) (`SetTab` 0x620960, scroll memory per tab `+0x15C..+0x170`, the bar starts
on tab 5), row area 8 (0x32..0x2EE) with drag scrolling, 8 cell frames of 100 px from x 50 (the last one is covered by the right
end piece); classifier by mode `+0x194`:
0 `Item_GetCampCategory` 0x6218F0, 1 `Item_GetSpellContainerGroup` 0x6219A0 (spell school 0..2, rune 3 / 4, other 5), 2
`Item_GetSlotGroup` 0x621A80; filter `+0x1B0` 9 = all; `switch_filters` g_0x79BE84 jumps to the category of the moved item).
`CCampBar` 0x622000 [V, deep read 2026-11-06] = the hero row of 8 100-px buttons (y 0..100; window rect (0,0)-(800,100), or (800,500) with the doll): buttons 0..3
= the weapon slots `+0x730` (hit kind 2), buttons 4..7 = the **belt** (quick items `+0x740`, drawn right to left: belt slot i =
button 7 - i, hit kind 3); in the camp (`Build(1)`) also the "camp1..4" paper-doll picture (200..600 x 100..500) and 7 more
buttons 8..14 = the worn armor by armor type (100x100 around table 0x741EC4 / 0x741EE0: 0 helmet (250,150), 1 plate (250,300),
2 legs (250,450), 3 shield (550,150), 4 pants (550,350), 5 boots (550,450), 6 gloves (550,250); hit kind 4..10). `Draw` 0x622420:
marker quad 0 on the weapon in hand (`+0x774`, x = 100 slot + 50), quad 1 on the selected belt slot `+0x154` (x = 750 - 100 i),
the 3D item figures (`UIUnitPreview::DrawItemFigure`) of the weapons, the belt and the armor at its doll position (after a depth clear (Clear flags 2, or 6 with stencil) of the seven 70-px doll boxes; whole depth cleared again after); right
click on a weapon slot = the slot in hand `+0x774`, change.wav (0x622960). `CCampQuickBar` 0x622A80 = the 8 **spell** slots
(`+0x760`, hit kind 1, marker `+0x154`).

`CUnitInfoWin` 0x622DB0 [V, deep read 2026-11-06] = the hero stats pane (0,100)-(200,400): buttons 0 / 1 (0..0x1E / 0xAA..200 x
0x7D..0x91), one blit surface, arrow / frame quads, texts "infounit_3 / _4 / _9 / _14 / _17", "lmp_points_left", "lmp_pers_tall",
"magic_skills" (`+0x15C` array). `SetMode(mode)` 0x6233C0 (`+0x158` = the camp tab, or the CCampScreen2 mode) recreates the
tooltip row buttons from index 2 (15-px rows from (20,0xA5)): mode 0 = 4 + 3 + 1 + 1 rows with gaps; mode 10 = 4 label rows
(0x14..0x8C) + 4 "-" buttons (0x8C..0xA0) + 4 "+" buttons (0xA0..0xB4) + one row (0x14,0xD2)-(0xB4,0xE1); others 8 rows. `Redraw`
0x623880 draws the name at (0,0x1B)-(200,0x2B) and per mode:
- mode 10 (new multiplayer hero): Strength / Dexterity / Intelligence = trunc of the base stats (`+0xC / +0x14 / +0x1C` floats),
  "lmp_points_left" = trunc(75 - (str + dex + int)), "lmp_pers_tall" = `+0x170` (0..100); a "-" is grey (0x828282) when the stat
  <= 15, a "+" when the stat >= 35 or no point is left; the height "-" / "+" grey at < 1 / > 99. So the creation rule is
  **each stat 15..35, sum 75** (display gating; `OnConfirm` checks the sum).
- mode 0 (equipment tab): "infoitem_25", the armor of 4 body parts (`DrawBodyPartArmor`), the weapon in hand (`+0x774`, none ->
  blank): its name, two value lines, "damage_%d" (type < 7) and "%d-%d" damage; then Encumbrance = trunc(`HeroRecord::TotalWeight`)
  / trunc(carry capacity), with the red (0xFF) overload text when weight > capacity, and one more text line.
- other modes: Strength, Dexterity, Intelligence, Health (`UnitStats_MaxHP`), Mana (`MaxMana`), Actions, Experience, Encumbrance
  (CCampScreen2 mode 11 takes the weight from slot 53 `GetCarryWeight`) with the same overload warning; in tab 1 (skills edit) the edited rows blink (0.25 s, earlier read).

`CCampSkillsWin` 0x624F10 [V, deep read 2026-11-06] (the skills / perks pane of tab 1; one blit surface with origin (200,100);
titles "skills" (0,10), "perks" (0,0xCD), "perks_av" (200,10) in surface coordinates). Buttons: 0 / 1 learned-list scroll (0x168..0x17C
x 0x140 / 0x1E0), 2 / 3 available-list scroll (0x230..0x244 x 0x7D / 0x1E0), 4 / 5 Melee / Archery rows (0xD7..0x181 from y 0x87,
15-px rows), **6 = an empty rect (0,0,0,0): the Backstab row**, 7..9 Elemental / Sense / Astral rows, 10 Science (y 0xA5, the third
line), 11..33 the 23 available-perk rows (0x19F..0x249 x 0x87..0x1E0), 34..43 the 10 learned-perk rows (0xD7..0x181 x 0x14A..0x1E0).
So the camp shows Melee, Archery, Science, a blank line, Elemental, Sense, Astral: **Backstab (stats `+0x6CD`) is never shown and
cannot be raised in the camp**. Per party hero a 0x3C-byte edit record (`+0x190` vector, `SaveStatsBytes`) keeps the stat bytes and
spent experience for Cancel.
`RefreshPerks` 0x625880: `UnitStats_Recompute(stats, 0)`, owner slot 47; skill rows = the names Melee, Archery, Backstab, Elemental,
Sense, Astral (table 0x793BD4) with level = byte `stats +0x6CB + i`, then Science = byte `+0x6C8`; title = first line of
"perk <name>" (or the name); raise cost = `Skill_RaiseCost` 0x527FF0 = trunc(50 * 1.61^((v+1)/5) - 50 * 1.61^(v/5)) (constants
g_0x7C547C = 50, g_0x7C5480 = 5 set at start, base 1.61 at 0x73E140). Learned perks: for each of the 36 bytes `stats +0x6D3..+0x6F6`
with level L >= 1, record (3 g + L - 1) of the perk table g_0x7AF274 (0x44 bytes, code `+4` stripped of trailing "123" -> "perk
<code>" text); label "%s (%s%d)" with "perk_level" when L >= 2; next cost = `Perk_ComputeCost(stats, record 3 g + L)` when L < 3,
else 0 (maxed). Available perks: every first-level record (step 3) that `UnitStats_CanLearnPerk(stats, g_0x7AF268, record)`
accepts, cost `Perk_ComputeCost`. Free experience `+0x18C` = trunc(stats `+0x700` + experience `+4` - spent `+8`).
`RebuildRows` 0x6261A0 draws the three lists: a cost (`FormatNumberKM` 0x626090) is beige when <= the free experience, grey
(0x828282) when not; skills at level >= 100 show no cost; each affordable-looking row gets a "+" quad (350..390 or 550..590);
10 learned rows from scroll `+0x158`, 23 available rows from scroll `+0x154`.
`CCampTradeWin` 0x6270A0 (trade1..4) [V, deep read 2026-11-06]: `Draw` 0x627390 clears the depth in (200,100)-(600,500) and draws
the buy basket as a 4-column grid of 100-px cells from (250,150) and the sell basket from (250,350) (two rows each = the 8-entry
basket limit), a stack count "%d" when `+0x48` > 1 and, unless `+0x160`, the unit price from the owner's slot 54 `GetItemPrice`.
`CCampRepairWin` 0x6279E0 (repair1..4, 16 slots). `CCampSpellConstrWin` 0x6280C0 (constrspell1..4): `Draw` 0x6284A0 shows a
container to deconstruct at (250,300), the blueprint at (350,300) and the runes in 2 columns x 4 rows from (450,150) (8 cells =
the rune limit); with a blueprint two lines "%s %d (%d)" = complexity (blueprint `+0x44` + runes `+0x18`) with the party best skill
from `GetMagicTotals`, and mana = trunc(max(blueprint mana, blueprint mana + runes `+0x10`)) with the best mana budget (the same
max as `CSpell +0x40`; note `CanDropItem` tests the plain sum); a pulsing marker (cos 5 t) shows the free rune cells, or the
container / blueprint cells when empty; without a blueprint the two help lines `+0x15C`. `CCampItemConstrWin` 0x628CC0
(constritem1..4): `Draw` 0x628F80 shows a usable item to deconstruct at (250,300), the blueprint's proto figure at (350,250), the
spell container at (350,350) and the materials in 2 x 4 cells from (450,150); the pulsing marker marks the material cells still
needed (count < i <= proto Components `+0x4C`), then the container cell (350,350) once the materials are complete, or the item /
blueprint cells (250,300) / (350,250) when empty; nothing while a container is in.

## 5. Party screen, lobby, exchange (multiplayer)

**CCampScreen2** 0x613EA0 (pushed by the main menu button 5, modes `Layout` 0x615D30: 9 slot list, 10 new-hero edit, 11 equip and start):
`Databases_Reload`; `PartyStore::LoadAll(g_0x7C82A0, "mp\")` fills the slot list `g_0x7C82A4` of 0x74-byte `PartySlot` (CParty +
name); the face lists = the hero prototypes of the 0x60-byte table `g_0x7AF378` split by the race model mask (`+0xE8`) "unhuma"
(`+0x1C8`) / "unhufe" (`+0x1D4`); a prototype whose record byte `+0x5C` is set also gets a voice row "<lmp_male_voice / lmp_female_voice>
<n>" (`+0x1F8 / +0x204`); both sorted. Two 10-character `CEditWin`: slot name (0x22E,0x6E)-(0x2B7,0x7E) and hero name
(0,0x7F)-(200,0x8F). `Layout` re-rects the first buttons per mode (tooltips 0x9CA4.. mode 9, 0x9D6C.. mode 10, 0x9E34.. mode 11) and
recreates the row buttons from 9 (mode 9: 10 slot rows (0x6E..0x172 x 0xFA + 0x18 r)).
Buttons [V, deep read 2026-11-06] mode 9: 0 back ("reserved_04" message bar, `LobbySession_ReleaseActiveAndReset`, pop), 1 start
(`OnStart` 0x6183B0 -> "lmp_no_pers" box or `PartyStore::SaveAll` + push CLobbyScreen), 2 / 3 hero arrows (held: `+0x1E4` = 1 / 2,
camp\move.wav), 5 new (slot 52 with the current face, empty name, mode 10), 6 rename (`ShowSlotDetails`: the slot name edit; Enter
stores "<label before '|'> | <new text>" (or the label alone when the text is empty) into the party name and `PartySlot::SaveToFile`;
Esc cancels), 7 delete ("lmp_deletepers" box -> 0x10001 `DeleteSlot`), 8 redress (pushes a CCampScreen with `+0x194` = 1, so its leave
button only pops), 9..0x12 slot rows (save\select.wav, `SelectSlot`). Mode 10: 0 cancel (mode 9), 1 `OnConfirm` 0x6186F0 (needs str
+ dex + int = 75 and a name), 2 / 3 face arrows, 4 reset the hero (slot 52 again, the six stat floats `+0x2C..+0x40` (str / dex / int,
base and current) = 25.0, complection `+0x72C` = 0.7, empty name), 9..0x13 voice rows (record `+0x18` = the voice prototype,
`LoadSlotSounds` plays a sample). Mode 11: 0 back to mode 10 (skill edits discarded, the name put back into the edit), 1 `StartGame`
0x617A50 (strips spells the hero cannot cast, copies the voice set, `PartySlot::SaveToFile`), 4 undo (skill edits discarded, first
weapon / quick item / spell re-selected in the bars). Keys (`OnKeyDown` 0x618A10): Enter = start / confirm / StartGame per mode,
Esc = button 0 per mode, Up / Down / PgUp / PgDn (10 rows; 11 for voices) move the slot or voice selection. `Draw` 0x6159E0 renders the preview hero `+0x1A0` with `UIUnitPreview::Render` at x = 555 (400 in mode 10); the face bars draw their faces with `UIUnitPreview::DrawFaceFigure` 0x5ED7B0 (exact compare of the race mask `+0xE8` with "unhuma" / "unhufe" -> "infa%sme%dface", else "face%s%02d"). Slot 42 `SetEquipped`,
43 `GetSpellCost`, 47 `CommitUnit`, 51 `GetHeroRecords`, 52 `RebuildHeroSlots`, 53 `HeroRecord::GetCarryWeight` [V].

**CLobbyScreen** 0x61B2C0 [V]: reads `config\addrbook.ini` (`Archive_Open`, `NetServerRecords_Serialize`) into `NetClient::SetServerList`,
`LobbyServer_Create`; buttons 0 join (`JoinSelected` 0x61CCE0: row 0 = host with `Lobby_StartServerOrClient(name, players, allod,
password, showFull)`, a password row -> box 0x50000 -> `Lobby_BeginConnectFromAddress`, wrong protocol -> "lmp_wrong_protocol"), 1 back
(`AddrBook_Save` 0x61D1C0 writes the ini, close 0x1000001), 2 direct connect (box 0x30000 / 0x40000: `NetAddress::FromString` +
`NetClient::OnServerFound`), 3 delete server, 4 show-full toggle, 5 password box, 6..9 column sort, 10..13 allod tabs, 14.. rows
(0x34-byte server records). `Update` 0x61C430 polls `NetClient::TickDiscovery` every 0.5 s and shows lmp_server_full /
lmp_you_are_banned / lmp_wrong_password / lmp_cannot_connect on a failed connect. `CConnectProgressWin` 0x61E2F0 (a CMessageBox with a
TcpStream and a progress quad) closes when `LobbyClient::OnServerInfoPacket` is done.
Build details [V, deep read 2026-11-06]: the player name edit `+0x194` (letters / digits, 10 chars, disabled) shows the client
party's first hero name cut before '|' (the slot tag of CCampScreen2), at most 10 characters, trimmed; the host's player count is
the `CHScrollBar` `+0x190` (0x1C2,0xC4)-(0x28A,0xCE) with range 5 (= 1..6 players, `+0x198` = 6); an id field of the loaded address book [I: client id], when 0, is
replaced by `GetTickCount() ^ ISAAC` before `NetClient::SetServerList`; 22 texts "lmp_connection_%d" and 4 "lmp_allod_name_%d".
`LayoutLobby` 0x61D780 draws the table from `+0x188` (rows 0x18 px): row 0 = "host a game", other rows name / "%d/%d" players /
allod (or "?" when record `+0x24` == 4) / ping right-aligned, coloured by record `+0x2C` (1 gold 0x31B3FF, 2 0x31E3EE, else beige);
the selected row (marker quad at x 239) fills the detail panel: row 0 = the host settings (players "%s %d", the 4 allods, two
option texts), a server = name, "%s %d / %d" players, allod, the wrapped description, the address (`NetAddress::ToStringDefault`)
and the ping; sort arrows by `+0x258`; separators with `CDC::LineTo`.

**CExchangeScreen** 0x62C250 (screen id 9) [V, deep read 2026-11-06; protocol in net-transport.md 4.7]: figure "campinfo"; buttons
0 accept (0xC,0x1C2)-(0x34,0x1EA), 1 retract (0x94..0xBC), 2 leave (0x2E4,0x1CC)-(0x30C,0x1EA) (tips 0xA028 / 0xA029 / 0xA02B); quads
0 / 1 the accept / retract art, 2 a hidden frame, 3 the leave art (scale 0.5); `+0x190` a fresh scratch `CParty`, `+0x198` own offer
= `LobbyClient +0x2620` (cleared), `+0x194` the remote offer = `LobbyClient +0x2690`; children: `UIItemInfoWin` `+0x164` (600,100),
the party inventory `CCampBar2` `+0x19C` (mode 0, kind 1), a `CCampFaceBar` `+0x1A0`, a `CCampTradeWin` `+0x1A4` (kind 5) whose top
grid (250,150) shows the **remote** offer and the bottom grid (250,350) **ours**, the money `CEditWin` `+0x1A8` (0,0x15E)-(200,0x16E)
(mask 1 = **digits only**, 9 characters, text 0xFFFFFF, caret 0x31B3FF) and 4 text surfaces. `Draw` 0x62C9C0: "xch_current_money"
"%s %d" (party money) + "xch_accept"; "xch_money_offer"; when the partner record exists and is live (bit 31 of `+4` clear)
"xch_money_remote" + its money "%d" and "xch_remote_header" + "xch_remote_ok" (partner `+0x1C` confirmed) or "xch_remote_cancel";
once we confirmed, the accept art shrinks to 0.5 and the retract art grows to 1.0 (the reverse before); the hovered item of the party
bar or offer grid goes to the info window. `Update` 0x62C930: while the pair record `LobbyClient +0x2618` lives, a dirty money edit
sets own offer `+0x50` = atol(text) and `FillParty` re-uploads it; when the pair vanishes, the own offer goes back to the party and
`Game_SendEnterWorld`. Click 0x62CF70: **accept** only when our record is live and not yet confirmed and **our offered money <= our
party money** (`+0x198 +0x50` <= party `+0x50`; the partner's money is not checked here) -> buy.wav + `ConfirmPair` (cancel.wav is
played after it in every case); retract only when confirmed (`RetractExchange`); leave = transit.wav + `ReleaseExchangeRecord`. Item
click 0x62D100 (slot 42, every drop allowed by slot 43 0x62D200): kind 1 moves the item party -> own offer (basket limit 8 entries,
`Party_TransferItem` count 8), kind 5 takes one of our offer back; both re-upload (`FillParty`). `Exchange` 0x62D210 (slot 37,
reached through `CInterface::OnLobbyRef` 0x5F8310 from the confirmed-pair update handler, code at 0x41F270 that has no function entry
in the database) (a, b, c): `CParty::ReleaseItems` on the client party, moves every item of c and then of a into it, party money
= a.money - b.money + c.money, then `Game_SendEnterWorld` [V]; which party is a / b / c is in the caller [I: a = a copy of our party,
b = our offer, c = the remote offer].

## 6. Main menu, esc menu, boxes, credits, intro

| Screen | Build / buttons [V] |
|---|---|
| CMainMenu 0x6326B0 (id 2) | `Databases_Reload`, version text (0x6335E0 "version_name" + build), 6 buttons from the rect table g_0x7C80A8: 0 new game -> CDifficultyBox (base 0x20000; 0x20001 = `Databases_Reload`, Movies\Intro.bik, new-game start 0x5816C0), 1 load -> CSaveLoadWin(load, no slide), 2 quit -> Movies\Exit.bik + `App_PostQuit`, 3 options (`+0x3F5` = 0, no slide), 4 credits (`Game_StopAllSounds`, the menu's text surfaces cleared, CCreditsScreen "Titles" pushed without the fade), 5 multiplayer -> "reserved_04" message bar, `Game_SetServerPaused(1)`, CCampScreen2 (0x220) [V, `OnLButtonDown` 0x632D70 deep read 2026-11-06; every button plays menu\ok.wav except quit]; camera "MainMenu.cam"; hovering animates the menu figure (stone.wav 0x633400); Esc = quit box (0x10001 = quit) |
| CEscMenu 0x62AB20 | figure "escMenu00" / "escMenu00labels" at (400,700) depth 4.8 tilted by g_0x7C7FB0; 5 buttons (g_0x7C80A8): 0 save -> CSaveLoadWin(save, slide) (not in a lobby), 1 load, 2 continue (close), 3 quit -> box "quit" (0x10001 = `LobbySession_ReleaseActive2`), 4 options -> COptionsScreen(slide); hover plays stone.wav and switches the figure animation 2..6 (0x62AD10); 0x100001 / 0x110001 from a child closes the menu too (0x62ACC0) |
| CDifficultyBox 0x6337B0 | [V, deep read 2026-11-06] a CMessageBox (0x184; ctor args title "option_difficulty", empty text, base 0x20000, flags 3) whose own Build lays out: the title, the first paragraph of the text "tip option_difficulty" (`OpenText` + `ReadUntilHash`, right-trimmed), the two choices "difficulty_easy" / "difficulty_hard" centred one under the other (rows of 0x18 px, buttons 2 / 3 over them, marker quads 0 / 1), the panel 200..600 vertically centred on the 0x226-px text block, OK (0x107..0x151) / Cancel (0x1CF..0x219) buttons 0 / 1 (tips 0xC3B4 / 0xC3B5). The choice `+0x180` starts at **0 = hard** (the hard row is marked); button 2 = easy (1), 3 = hard (0); OK (0x633DD0) writes **g_0x79BEC0 = choice (1 easy, 0 hard**, the same order as the options list "difficulty_hard" / "difficulty_easy"), ok.wav, closes 0x20001; Cancel closes 0x20002; Enter / Esc the same (0x633EC0) |
| CMessageBox 0x634650 | (ui, titleKey, textKey, base, flags, align): flags 1 OK, 2 Cancel, 4 edit, 0x10 align. `Build` 0x634790 [V, deep read 2026-11-06]: blit surface (200,0x32)-(600,500); the text font is 2, or 1 for the fatal box (base -0x10000); the block (title + text or edit + 0x3C) is centred vertically in 0x226 px; title golden 0x31B3FF centred, text beige 0xA7D7E4 with `align | 0x10`; flag 4 replaces the text line by a `CEditWin` (0xD7,y)-(0x249,y + line height) with **mask 0xF (any character), 50 characters**, the text font, seeded from the caller's char buffer `+0x17C`; dark panel (200..600) + border; buttons: 1 and 2 = OK (0x107,y+0x1A)-(0x151,y+0x4A) + Cancel (0x1CF..0x219), 1 alone = OK (0x16B..0x1B5), 2 alone = an empty button 0 + Cancel at (0x16B..0x1B5) so Cancel stays index 1. OK (0x634D20 / Enter): base -0x10000 plays Exit.bik and quits; flag 4 strcpy's the edit text into the caller's buffer (the two lobby callers pass `CLobbyScreen +0x1A0`, a 0x9C-byte char buffer, so 50 characters fit); closes base \| (button + 1); Esc = Cancel |
| CCreditsScreen 0x62B7D0 (id 8) | [V, deep read 2026-11-06] name `+0x1D4` = "Crdt" (screen id 8, the end of the campaign, `+0x1F4` = 0) or "Titles" (main menu button 4, `+0x1F4` = 1). Build: end of game only: `CSoundManager::StopAll` + music stop; `Movies_PlayIniList("<name>fin")` (movie.ini section played before), **font smoothing saved in `+0x16C` and switched off** (SPI 0x4A / 0x4B = SPI_GET / SETFONTSMOOTHING, restored by OnClose 0x62BD00), the archive `<root>outro.res` (`CMappedResource` `+0x170`) entry "credits.scr" parsed line by line (space separated): "[#scroll] <seconds or speed> <rtf entry> [title]" -> 0x18-byte nodes of the list `+0x1C8` (`+8` scroll flag, `+0xC` float, `+0x10` title, `+0x14` rtf entry); vanilla holds one line "#scroll 25 outro.rtf"; a `CRichEditSurface` (10,0x96)-(0x316,0x1C2) `+0x164` and a title surface (10,0x6E)-(0x316,0x8C) `+0x168`, both hidden; from the main menu `Game_SetServerPaused(0)`; render globals `+0x14` saved in `+0x1F8` and zeroed; background video `Movies\<name>.bik` (`CBinkVideo` `+0x1D8`, no sound). `Draw` 0x62BD80: when the timer `+0x1CC` has passed and the scroller is idle, the next node is streamed in (`StreamInFromResource`): a plain node is a still page shown for `+0xC` seconds, a `#scroll` node scrolls up from the bottom at speed `+0xC` (`RichEditScroller::Start(speed, 1)`) with its title drawn above (font 2, 0xFEFEFE); after the last node and the end of the Bink, `Movies_PlayIniList("<name>fout")` and either pop (main menu) or `LobbySession_ReleaseActive2` (back to the main menu after the campaign). A click / Esc / Space skips the same way |
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
| list | `ScanSaves` 0x655D40: every `saves\save*` directory -> a 0x44-byte `SaveSlotRecord` (label, folder, CTime, gtime, zone title, map; `+0x40` error = no info.sav "Error Save"), sorted newest first; save mode inserts a new slot 0 "save<n+1>" with the live screenshot (`CInterface +0x5C` thumbnail) [V, deep read 2026-11-06 with the machine code: n = the largest `atol(folder.Mid(4))` of the folders with names longer than 4 characters (a separate variable, not the info.sav version); the version gates read the stream's own version field (time when > 1, label when > 2); an empty label gets `MakeLabel`; the new slot holds "gtime", the client map, that map's zone record `+4` as the title and a `MakeLabel` label; scroll range = slots - 16 when more than 16] |
| layout [V, deep read 2026-11-06] | quads 0 / 1 / 2 button art (163..237 / 563..637 / 363..437 x 526..574), 3 the screenshot (410,100)-(700,320) with UVs (0,256)-(256,64) (192 rows, flipped), 4 the row cursor (moved to (239, 0x78 + 0x18 r) by `RefreshSaveList`), frames list (95,95)-(395,505) / shot (405,95)-(705,325) / details (405,335)-(705,505); figure "escMenu00" parts "load" + "loadlabel" or "save" + "savelabel" at (400,700) depth 4.8; load mode selects slot 0 (or -1 when there are no saves), save mode slot 0 with its label copied into the edit string; the tutorial only when not sliding in |

`info.sav` [V] = `Stream_WriteHeader4(0x111, version 3)`, float gtime, string zone title, string map name, u32 time_t (v >= 2), string
label (v >= 3) (`SaveSlotRecord::Serialize` 0x6539C0; `ScanSaves` reads it with the version gates). `shot.sav` = 0x18000 bytes =
256 x 192 x 16-bit pixels (`CUIBlitSurface::StretchTo(256,192)` in `QuickSave` 0x6570E0; the live figure `+0x184` in `DoSave` 0x655550;
read back into the scratch figure by `UpdateSaveScreenshot` 0x656E10, "Can't lock surface." fatal).

| Action | Flow [V] |
|---|---|
| select | row click / Up / Down / PgUp / PgDn (16) -> `+0x170`, the label copied into the edit string `+0x188`, list + shot refreshed (`ClampSelection` 0x655430) |
| type a name (save mode) | `OnChar` 0x655050: a normal slot (not saveq / savea / saveb, `IsSpecialSlot` 0x657820) starts editing (`+0x18C`, text cleared), < 60 chars and < 0xB2 px wide (font 1); Backspace / Delete / Home / End / Left / Right in `OnKeyDown` 0x6548A0 |
| confirm | button 0 / Enter / double click -> `OnConfirm` 0x655290: save: slot 0 -> `DoSave`, other -> box "overwrite" (0x20001 -> `DoSave`); load -> `OnLoad` 0x655A10: `ShowMessageBar("notify_loading")`, `Game_StopAllSounds`, `SessionRequest::SetLoadSave(g_0x7AFE20, folder, 0) 0x5816D0 (kind 2)` (the session loop loads it) |
| DoSave 0x655550 | free space (`Disk_GetFreeBytes` vs `SavesCurrent_ScanSize`, else box "no_disc_space" 0x30001 -> close 0x100002), `ShowMessageBar("notify_saving")`, `Game_SaveToSlot_Wrap(folder)`, cursor frame 3, record label = `+0x188` (slot 0 unedited = empty -> MakeLabel), time = now, info.sav, shot.sav, close with 0x100002. [V, deep read 2026-11-06] The free-space test is skipped when `Disk_GetFreeBytes` returns 0, and a refusal only pushes the box (base 0x30000, OK) and returns. The game state goes to the **selected** slot's folder, but the info.sav written is always **record 0** (the new-slot record that ScanSaves filled with the current gtime / zone title / map), whose label becomes the edited name, else the overwritten slot's old label, else (slot 0 unedited) empty; so overwriting keeps the old name and stores the current game's details. shot.sav = the live figure `+0x184` locked under g_0x7AEE78 (0x18000 bytes); a lock failure closes 0x100002 without the shot |
| delete | button 2 -> delete.wav + box "delete" (0x10001 -> `DeleteSelectedSave` 0x655B20: `saves\<folder>\*.*` DeleteFileA + RemoveDirectoryA, rescan); refused for the new slot |
| cancel | Esc / button 1 -> close 0x100001 |

Quick / auto saves [V]: `CSaveLoadWin::QuickSave(folder, renderFrame)` 0x6570E0 is static (ecx = the folder name): "saveq" from the
quicksave key (action 0x35, HUD / dialog / world map), "savea" by `CBattleHud::OnZoneEnter` 0x6360F0 when the option autosave
g_0x79BE7C is on, "saveb" by `CWorldMapScreen::Build`. Labels: "save_quick" / "save_auto_enter" / "save_auto_exit" / zone title
(`SaveSlotRecord::MakeLabel` 0x653A50, zone title = first line of "zone <map>" or "save_camp"). `SaveGame_BeginLoad(folder)` 0x6575B0 =
the quickload key: `_access` the directory, "notify_loading", `SessionRequest::SetLoadSave`. Not in lobby games.
`QuickSave` [V, deep read 2026-11-06]: does nothing in a lobby, and **silently skips the save** when the disk has less free space
than the saves need (no box); otherwise renders a frame first when asked, copies the 800x600 back buffer, busy cursor,
g_0x7C7DB0 (`CInterface +0x60`) = `Game_IsServerPaused()`, "notify_saving", `Game_SaveToSlot_Wrap(folder)`, then info.sav (header
0x111 v3: "gtime", zone record `+4` of the client map as the title, the map name, now, an **empty label**, so the list shows the
`MakeLabel` text) and shot.sav = the frame `StretchTo(256, 192)` (0x18000 bytes).
`OnKeyDown` 0x6548A0 [V, deep read 2026-11-06]: Enter = `OnConfirm`, Esc = cancel.wav + close 0x100001, PgUp / PgDn -+16, Up / Down
-+1 (save\select.wav, `ClampSelection`); other keys only in save mode: Backspace / Delete on a normal slot not yet edited start
editing with an **empty** name, Home / End / Left / Right start editing with the slot's label; Backspace removes the character
before the caret, Delete the one at it, Home / End / Left / Right move the caret; `RefreshSaveList`. `RefreshSaveList` 0x6567A0
draws 16 rows from the scroll position: label (the edit text for the selected row while editing, with a golden caret line unless
the slot is saveq / savea / saveb) at (0xC,0xB)-(0xBE,0x20) + 0x18 r and the time "%d/%m, %H:%M" (not for the new slot), and for a
valid selected slot the allod name, the zone title, "save_day" "%s  %d,  %d:%02d" and the folder name.

## 8. The world map (CWorldMapScreen 0x740B3C, screen id 4)

`Build` 0x62D350 [V, deep read 2026-11-06]: current zone (`MapZoneRecords_FindByName(g_0x79BF00, client map)`) -> its allod record
(`MapAllodRecords_FindByName`) -> textures "Map<allod>", "Sys<allod>", "MapG<allod>", "saveload" and a 256x256 render target; the
allod title (first line of "allod <id>", `+0x17C`) and the current zone's `map.txt` position `+0x180` (`MapTxt_LoadZonePosition`
0x630E00: section `#zone <id>` then `#position` "x y z" ints; `MapTxt_LoadQuestPosition` 0x630CC0: section `#quest <id>`, first line
"x y z"). The map lies in 3D: quaternion `+0x1EC` = 144 deg (2.513 rad) about x, centre (400,280), scale 10, every point projected as
rot(p) + ((cx * 0.0025 - 1) * 0.4816 * s, (cy * 0.0025 - 0.75) * 0.4816 * s, s); quads 0 / 1 = the current-zone marker (0.8 scale
copy at z + 2) and quad 2 the map.
Three entry tables:
- **zone entries** (0x10 bytes `+0x190`, count `+0x164`): **every** zone of the allod's list whose record `+0x28` kind is 0 ("game")
  and whose record `+0x10` string is not empty; `+0` = trunc(`z.<zone>`) (0 = unknown: not hoverable / clickable), `+4` linked row or
  -1, `+8` name, `+0xC` title (first line of "zone <id>"), one quad each (the old note "visited zones `z.` == 1" was wrong: the value
  is stored, not filtered).
- **quest markers** (0x18 bytes `+0x1B8`, count `+0x168`): per zone entry every variable of `q.<zone>` (`Game_GetNamedFloat2`) equal
  to 1.0 with 3 dot parts: `+0` zone entry index, `+4` `#quest <id>` position, `+0x10` quest id, `+0x14` quest title; a 0.3-scale quad.
- **zone rows** (0x18 bytes `+0x1A4`, count `+0x16C`): per zone entry, every exit (record `+0x4C` data, `+0x50` count, 0x3C each)
  whose target zone is of kind 2 ("brief"), has a non-empty `+0x10` and `z.<target>` == 2.0: `+0` entry index, `+4` `#zone`
  position, `+0x10` name, `+0x14` title, a 0.3-scale quad and a button (index 3 + row) at its projected rectangle; when the source
  entry's state is 2 its `+4` links to the row.
Then three cloud-layer quads (700..100 x 0..600, colours 0xA08200C8 / 0xF0B400FF / 0x4BFFFFFF, heights -0.5 / 0.7 / 2), render
globals `+0x14` = 0xFF3C005A, the picking surface `+0x210` (built 400x300, rect 800x600), a hidden hover panel (400,0x14)-(0x30C,300),
the time panel (20,20)-(170,70): line 1 the allod title, line 2 "save_day" "%s  %d,  %d:%02d" from "gtime" (game hours: m = trunc(gtime * 60)
(fmul 60.0 at 0x62E750), day m / 1440, hour (m mod 1440) / 60, minute m mod 60) (font 2, 0xA7D7E4); the camp button art (hidden when `i.nocamp` == 1, `+0x27C`), the two rotate arrows; ambient of the
selected entry `+0x280` (ingos (0.371, 0.508, 0.625), suslanger (0.391, 0.195, 0.086), else (0, 1, 0)), `+0x278` = 1 on gipat;
`Game_ForwardToWorldServer(0, 0)`, music stopped, `FlameNoise` 256 `+0x214`, the 18 colour stages (`ApplyFadeColor`), wind.wav loop
`+0x290`; **autosave "saveb"** (`QuickSave`) only when the screen is built live (not restored from the UI stream g_0x7AFDF8 on a
load), the client mode (connection `+0x1C +0x104`) is not 5 (= not coming back from the camp [I]) and the option autosave g_0x79BE7C
is on; tutorial.
`Redraw` 0x630670 renders the picking surface: zone entry i is drawn flat in grey 0x18 + 0x10 i, so a pixel's bits 4..7 - 1 give
the entry; **at most 15 zone entries per allod can be picked** (the 16th colour wraps to an invalid id) [V].

Input [V]: buttons 0 / 1 rotate while held (`+0x174` = -1 / +1, move.wav; `Update` 0x62F050 turns the map by pi/4 rad per second,
fmod 2 pi); right drag rotates by dx / 300 rad (`OnMouseMove` 0x630010, which also prints "x:y" to a debug stream); 2 camp =
transit.wav + `World_SetConstrCurrent(0)` + `Game_SendLeave(5)` unless `i.nocamp` == 1. `OnMouseMove`: the hovered zone row button,
or the picked zone entry (on gipat redirected to its linked row) -> `SelectEntry` 0x630860 + zone.wav when the name changes; the
panel shows the title and, for a zone entry, the titles of its quest markers (one 20-px line each, frame grown to fit); an entry
with state 0 shows nothing. `OnLButtonDown` 0x62F950 (globalmap ok.wav):
- a picked zone entry with state != 0 that is not linked (any allod but gipat, or no row): `LoadZonePoints(current zone)` for that
  zone name (`+0x1D8`) and push **CQuestLogWin** `SetCaption(name, +0x1E0 travel-time map, globalMapMode 1, allowTravel 0)`: the player
  picks the entrance there and its OK travels (section 9);
- a linked entry (gipat) or a zone row button (3..): `LoadZonePoints`; only when a path exists (`+0x1DC`) -> `World_AddGameTime(time
  of entrance 0 in the map)` + `Game_SendString(zone name, 0)` (the travel order, entrance 0). `IsZoneAllowed_Ret1` 0x6316A0 always
  allows.
`LoadZonePoints` 0x6312C0 (zone record) is a depth-first search over the zone exits from the current zone: an exit whose target is the
destination sets `+0x1DC` = 1 and stores, keyed by the exit's entrance index (`+0x2C`), the travel time (-1 when the destination is a
direct neighbour, else the sum of the exit `+0` times of the hops before the last; the smaller value is kept); a target already on
the path stack `+0x1C8` is skipped; the search passes **through** a zone of kind 0 only when `z.<zone>` == 2, kind 1 ("edge") when
`z.` != 1, kind 2 ("brief") when `z.` != 0 (the destination itself may be passed once, flag `+0x1DD`). `World_AddGameTime` ignores
negative deltas [I: its purpose says delta >= 0], so a direct neighbour costs no time.
Esc = CEscMenu; 0x35 / 0x36 quick save / load. Tutorial key "global_map" / "global_map_nocamp" (0x62EE70).

## 9. The quest log / travel window (CQuestLogWin 0x7411BC, 0x1F0 bytes)

One window for three uses (`SetCaption` 0x64BE40 (name, travelTimes, globalMapMode, allowTravel)) [V]: the in-zone quest log (keyboard
action 4 "obj" from the HUD / dialog: mission mode, travel allowed only when `q.<zone>.<x>` names the zone), the "pass time / travel"
dialog of the clock strip (`CHudClockStrip::OpenTimeDialog` 0x648F10) and the world-map quest view (global-map mode with the travel-time
map `+0x178`). Build 0x64BE80: scrollbar `+0x1BC` (quests, step 3), a CTextListWin (0x3C,0xBE)-(0x180,0x1FE) for the objectives, art
"saveload" + "cross", quads 0 OK art (163..237 global / 363..437 mission), 1 cancel art (global only), 2 cursor, 3 map picture, 14 / 15
prev / next map arrows, 16..20 quest state markers; buttons 0 OK (0xA3..0xED global / 0x16B..0x1B5), 1 cancel (0x233..0x27D), 2 / 3
prev / next map (0x19E..0x1BB / 0x2CD..0x2EA x 0x180..0x194), 4..8 quest rows (0x3C..0x168 x 0x1E + 0x18 i), 9.. exit arrows.
[V, deep read 2026-11-06] Build also saves render state 0x18 in `+0x1EC` and sets it to 1, loads "obj_0..2" into `+0x1D8`, and
lays out the panels: quest list (50,20)-(390,160), objectives (50,180)-(390,520), map (410,20)-(750,360), zone text
(410,380)-(750,520), button strip (285,541)-(515,579); OK tip `tip 80102` (mission) / `tip 80100` (global map), cancel
`tip 80101` only in global-map mode (its art is hidden in mission mode), quest rows `tip 80111`. Without the 4th SetCaption flag
(`+0x17C`, called allowTravel) the window shows the caption map only (`ShowMap(caption)`). With it the **map list** `+0x164` is
every zone that has an **open quest**: each 3-part variable `q.<zone>.<quest>` == 1.0 whose zone record exists and lies in the
caption zone's allod (record `+4` equal; any allod in a lobby), each zone once (the old note said the `z.` variables: wrong, the
prefix read is "q."); when none matched, the zone of the quest named in `CInterface +0xAC` (the last quest message) is used; still
none -> the window closes. The map shown first is that last-message zone (else entry 0); the prev / next arrows (tips 80200 /
80201) appear with more than one map. So the dialog screen's obj key browses every zone of the allod with an open quest, the HUD's
obj key (flag 0) shows only the current zone.

`ShowMap` 0x64CC10 [V, deep read 2026-11-06]: drops the per-map textures / quads / buttons beyond the fixed 2 / 0x15 / 9; zone
record missing -> close. Map picture = figure of zone record `+0x14` on quad 3 centred at (580,190), **300 px for the larger side**
(half-sizes 150 * w / max and 150 * h / max of the sector counts `+0x20 / +0x24`, UVs cropped to match). Every zone exit of the
record (`+0x4C`, 0x3C bytes: rect `+4..+0x10`, target `+0x28`; "none" skipped, but the exit index keeps counting them) becomes a
0x1C-byte entry `+0x188`: `+4` exit index, `+0xC / +0x10` rect centre, enabled `+0` = (with travel times `+0x178`) a time stored for
that exit index, else `z.<target>` != 1.0; `+8` time; screen position `+0x14 / +0x18` = 580 + (cx - W/2) * 300 / max(W, H), 190 -
(cy - H/2) * 300 / max (W, H = sectors * 32 m); an arrow quad (UV 240..254) pointing to the nearer map edge (|dx| <= |dy|: up = no
turn, down = turned pi; else +-pi/2 by g_0x7C8170) and a button 10 px outwards (tip `tip 80110`). **Only with travel times** the exit
with the smallest time is pre-selected (`SelectExit` 0x64E990, 1.2x scale); otherwise nothing is selected (the old note said "the
nearest exit"). In global-map mode with no reachable exit the OK art and tooltip are removed. Quests = every 3-part `q.<map>.<id>`
!= 0, sorted by id; selected = the id equal to `g_0x7C8178` (last viewed, global) without the map-list flag, or to `CInterface +0xAC`
with it; `g_0x7C8178` = the selection (cleared when the map has no quest); scroll range = count - 5. The "zone <map>" title
(golden, (0,5)-(0x154,0x19)) and description (beige, (10,0x19)-(0x14A,0x8C)) go on surface [1]. `DrawQuestList` 0x64D9B0: 5 rows,
title = first line of "quest <id>", marker quad coloured by `q.<map>.<id>` (2 done, 3 failed).
`FillObjectives` 0x64DC60 [V, deep read 2026-11-06] (only when the selection changed): `g_0x7C8178` = id; text "quest <id>": the
description after the title (`SkipToSectionEnd` + `ReadUntilHash`) as `"\aC<E4 D7 A7>" <description> "\n "`, then from the quest
record `g_0x7C87B4[id]` in white `"\aC<FF FF FF> "` the lines "obj_cost_exp %d" (exp `+0xC` > 0, trunc) and "obj_cost_money %d"
(money **`+0x14`** > 0); then the sections of the same text up to the next `#quest`: every `#subobj <n>` whose `q.<map>.<id>.<n>`
(trunc) != 0 adds its first line in golden + " " + (value 2: green `\aC<74 DC 01>` "obj_completed"; 3: red `\aC<FF 60 48>`
"obj_failed"; other values: nothing) + "\n \n" + its text in beige + "\n \n". `CTextListWin::SetText`; then the quest markers of
`map.txt` section `#quest <id>` (first line = "x y x y ..." ints, `LoadQuestMarkers` 0x64EA80; lobby: the quest record
`+0x20..+0x24`) as 20 x 20 quads (texture 1) + buttons (tip `tip 80112`) at the same map projection.

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
Details [V, deep read 2026-11-06, the key switch checked in the jump tables 0x651308 / 0x65132C]: `Build` 0x6503C0 lays the tab
buttons at (100, 0x9C + 0x18 i)-(0xFA, ..) with icon quads (110..240), the tab tooltip is the text "tip option_group_<name>" (read
with `ReadUntilHash`, not a tip id), OK / Cancel tips `tip 70100` / `tip 70101`, frame (95,95)-(705,505), figure "escMenu00" at
(400,700) depth 4.8; the tutorial is shown only when the screen does not slide in. `OnKeyDown` 0x650E40: while waiting for a key, Esc
stops waiting; a scancode (& 0x1FF) missing from the key-name table plays cancel.wav and **keeps waiting**; a known one ends the wait,
plays ok.wav and, when some other key-action row's action is the one the working table `+0x164` maps this scancode to, pushes
CMessageBox "key_already_mapped" (base 0x10000, OK + Cancel, the text "key_already_mapped_msg" formatted), else `ApplyPendingKey` at
once. Outside the wait only Enter (a key row starts the wait, a list row cycles its `CListBoxWin` to the next value; ok.wav), Esc
(restore the previewed volumes and gamma from the globals, cancel.wav, close 0x110001), PgUp / PgDn, Left / Right and Up / Down
(save\select.wav) do something; every other key is ignored.

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

`Build` 0x635250 [V, deep read 2026-11-06]: the controlled-unit id list `+0x1DC` starts with **only the hero** (the id `+0x1C` of
the first unit of the client party, `+0x200` "first zone enter" = 1; `UpdateUnitList` adds the others later) or, on a load, comes from the UI stream `g_0x7AFDF8`: count +
ids, 4 more bytes, the pause byte (`Game_SetServerPaused`), when the stream version g_0x7AFE00 > 0x46 the **combat-log text** (put
back into the message panel `+0x164`; the old note said "zone name"), when > 0x61 the pending zone exit `+0x188`; then the stream is
emptied. Two 256x256 render targets, figure "battle00"; widgets `+0x194` CHudWeaponBar, `+0x198` CHudQuickBar (4 belt items + up to 4 quest items), `+0x19C`
CHudSpellList, `+0x1A0` CHudAbilityList, `+0x1A4` CHudMainPanel (portraits + minimap frame), `+0x1A8` CHudUnitInfoPanel, `+0x1AC`
CHudMinimap (built by 0x641F20, which was named `CHudMainPanel::Build` but runs on this vt 0x742660 object), `+0x1B0`
CHudClockStrip, `+0x1B4` CHudStanceStrip, `+0x1B8` CTextListWin combat log (0xBE,0x19)-(0x262,0x59), `+0x1BC` CHudMessagePanel
(`FlushCombatMessages`), a `CamCtlFree` camera handler; personality labels from every text "pers *" (first line, keyed by a hash of
5 characters, std::map `+0x1F0`); command mode `+0x17C` = 0, slot `+0x180` = -1, selection `+0x168` emptied, called-shot flags
cleared; `Game_ForwardToWorldServer(0, 0)`, `Game_SetFlag94_8`, bit 0x40 of g_0x79B2B0 cleared; in a lobby slot 36 is called. `Update` 0x638280 [V, deep read 2026-11-06]: (1) when exactly one unit is selected and its vt+0xDC answers non-zero [? a unit
the player does not command], the weapon / quick / spell / ability bars are emptied and the unit info panel shows that unit (`+0x14C`
unit, proto `+0x20` / `+0x24`, `UnitLabel`); (2) **only in a lobby game** the unit list is rebuilt from the lobby player records
(`LobbyClient +0x25D8`): records with `+0x24` == 0 whose unit `+0x2C` exists give list A, the others their `+0x30` in list B (both
sorted); when either differs from `+0x204 / +0x214` (`UnitIdLists_Equals` 0x6381E0) `+0x16C` is rebuilt with the own player's unit
(record == `LobbyClient +0x25F0 +0xC`) first and selected (`HideObject`) and the others after it, `BuildUnitPortraits`, and the
camera follows the own unit (`CCamera::FollowUnitId`, `+0x210`) + `RefreshPanels` (single player maintains `+0x16C` in
`OnUnitAdded` / `OnUnitRemoved` / `UpdateUnitList`; the old note had it the other way round); (3) children tick,
`ShowQueuedTutorial(0)`; (4) the pending exit `+0x18C`: the client party's hero records (`+0x30`, 0x784 bytes each, count `+0x34`)
-> `Game_CollectHeroesInZoneExit(heroes, +0x188)` -> box "leave_zone" / "leave_zone_msg" (0x10000, OK + Cancel; 0x10001 ->
`Session_RequestMapChange(+0x188)`), flag cleared; (5) during a rubber band drag the controlled, unselected units inside the
rectangle get the hover flag 0x100 (`CollectPickedObjects`), the others lose it; (6) the quest sound `+0x1FC`: 1 quest.wav, 2
subcomplete.wav, 3 complete.wav, then 0. Slots 28..35: `OnLootMessage` 0x636150 (items / money into the message panel),
`OnUnitAdded` 0x6361B0, `OnUnitRemoved` 0x6364E0, `ShowGameOver` 0x6365E0 ("game_over" / "end_of_game", base 0x90000 -> leave),
`OnUnitEvent` 0x6366F0 (unit voices, "say" lines into the log), `ShowQuestMessage` 0x6369D0 (name, value) [V, deep read 2026-11-06]: a quest variable `q.<zone>.<quest>` (3 dot parts) prints
"\aC<EE E3 31>" (yellow) + "combat_complete_quest" (value 2) / "combat_failed_quest" (3) / "combat_get_quest" (other) + " " + the
quest title (first line of "quest <id>"); a sub-objective `q.<zone>.<quest>.<n>` (4 parts) is shown only when its quest
`q.<zone>.<quest>` != 0 (always in a lobby) in golden "\aC<FF B3 31>" with "combat_complete_subobj" / "combat_failed_subobj" /
"combat_get_subobj" + the text of the `#subobj <n>` section; each line ends with "\aC<E4 D7 A7>\n" (back to beige) and goes into the
message panel (`CTextListWin::Refresh` unless the panel is on its other tab); the clock strip `+0x158` = 1 (its quest-log button
blinks [I]); `+0x1FC` sound = max(itself, trunc(value)) with a failure counted as 1, a completed quest forced to >= 3: so **a failed
quest plays quest.wav (the "new quest" sound)**, a completed sub-objective subcomplete.wav, a completed quest complete.wav (Update), `OnZoneEnter` 0x6360F0 (autosave "savea"), 40 `UpdateUnitList`.

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
`+0x188 / +0x18C`) + console "Terrain Coords x y z", a **dead** unit (object vt+0xDC = `CUnit +0x274` dead flag; the decompile of
0x638EF0 lost most branches, this split is from the deep read 2026-11-06 [I]) = `SendInteractOrder` 0x6373A0 (`SendMsg36_Interact(ids,
0, id, target)`, i.e. loot the corpse), a living unit = when `CUnit::IsPlayerInMask(first unit, target)` says hostile
`SendAttackOrder` 0x637190 (`SendMsg31_Attack(ids, shift, target, hit location 0..5 or 6 any)`, warning event 10 when the target is
> 5 levels above) else a move to its position. The cast modes take the target only while it is alive (else the terrain point; a
(0,0) point does nothing); mode 3 refuses a belt item of proto subtype 8 (scroll) on anyone but the user [I]; modes 5 / 6 / 8 flash
the first selected unit's portrait (random phase from the ISAAC generator). Every order resets `+0x17C / +0x180 /
+0x184` and the slot highlights. `SetCursorTarget` 0x639EF0 (object, x, y) [deep read 2026-11-06; the switch is garbled in the decompile, frame numbers V,
conditions I]: a living object with flag 0x400000 under the cursor becomes the unit info panel's hover unit (`+0x150`, proto `+0x20 /
+0x24`, label), cleared otherwise; nothing selected -> no change; then the frame per mode: 0 -> 1 attack on a hostile living unit
(`IsPlayerInMask`), 4 on a corpse or a usable object (one unit selected), 0xF over a zone exit whose `z.<target>` != 1 (alt held:
the same exit test); 1 / 2 (spell) -> 2 when `CUnit::SpellUsability(slot, point)` allows, else 0x17; 3 / 4 (belt item) -> 2 when
`QuickItemUsability` allows (a scroll only on the user), else 0x17; 5 / 6 (use / loot) -> 4 when the object is usable (vt+0x98, loot
also vt+0xA8), else 0xD; 7 (follow) -> 0xF on a living unit, else 0xD; 8 -> 0x18 unless the object is one of the controlled units
(0xD).

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
Details from the deep read 2026-11-06 [V]: while `+0x22C` (modal portrait exchange) is set every key is swallowed and the flag
cleared; auto-repeated keys (flag bit 0x4000) are ignored; Esc first cancels a pending command mode (mode / slot / interaction reset,
highlights cleared) and only without one pushes CEscMenu; pause sends `Game_SendPause(!paused)`; **decel and accel both work only
while not paused and not in a lobby** (clock strip `+0x154` = 0 / 1, mirrored to `CInterface +0x94`); obj stops the clock strip
blink (`+0x158` = 0) and opens `CQuestLogWin::SetCaption(current map, 0, 0, 0)` (mission mode, **no travel** from the HUD key);
camera_norm = `CCamera::SetYaw(0)` + minimap `+0x1B0` = 0.5; **spell1-8 and item1-4 without ctrl / alt call `SelectSpellSlot` /
`SelectBeltSlot` (targeting mode), with ctrl or alt `UseSpellSlot` / `UseBeltSlot`**; weapon1-4 only when the slot exists and is not
the current one (weapon.wav, `SendMsg34_SelectWeapon(unit, i)` when the bar has a unit); w_info1-4 open the unit info panel on that tab
or close it when the tab is already shown (sling.wav), w_text1-2 the same for the message panel; w_minimap slides the minimap
(`+0x1A8` direction negated, `+0x198` = 1); select1-3 = `SelectUnitIndex(0..2)` (on_off.wav) and with ctrl or alt the camera follows
that unit; select_all (single player only) selects every controlled unit (`HideObject`), tilts every portrait, fires slot 32 for the
hero and `RefreshPanels`; tutorial_script = `ShowQueuedTutorial(1)`.

**The strips** [V]:

| Widget | Buttons and effects |
|---|---|
| CHudWeaponBar 0x63BAB0 | 4 slots at ((i*5+10)*8, 0x1EF)-((i*5+15)*8, 600); release = `SetSlotSelected` + weapon.wav + `SendMsg34_SelectWeapon(unit, slot)`; follows `CUnit +0x270` in Update |
| CHudQuickBar 0x63C800 | 8 slot buttons, but `SetUnit` 0x63D2B0 [V, deep read 2026-11-06] fills them as: the unit's **4 belt items** (`CUnit::GetQuickSlotItem(0..3)`, figures at (695 - 50 i, 575), scale 0.55, tooltip `SlotTooltipText` 0x63CEE0 "<name> %d/%d (%d)" charges with the item1-4 key 0x2A + i appended) and then, **only for a unit the player owns** (`CPlayer::OwnsUnit`), the first up to 4 **quest items** (kind 0x3009) of the player's item list (player `+0xE8`) at (695 - 50 (i - 4), 525), scale 0.85, tooltip from `GetItemTextKeys`; slot records 0xC bytes `+0x15C` (+0 1 belt / 0 quest item, +4 item id, +8 item), quad index per slot `+0x16C..`; release = `UseBeltSlot`, right / shift = `SelectBeltSlot`; highlight = alpha 0.65 / 0.55 (0x63CD20) |
| CHudSpellList 0x63E2A0 | spell rows x 0x2F8..800 from y 0x1D6 (0x28 each); `SetUnit` 0x63E820 [V, deep read 2026-11-06]: one row per filled spell slot `CUnit::GetSpellSlotItem(0..7)` (`CHudSpellList::Build` with the item's figure names); the row tooltip = the item text (`GetItemTextKeys`) + " \n" + lines built from the 13 labels "infoitem_7 / 18 / 8..13 / 21 / 22 / 26..28" (loaded by the ctor) with the container's `CSpell` (`+0x44`): a number "%s %d", the school "school_%d" text, a number or a text by proto byte `+0x7B`, speed shown as a number when >= 1 else a text, "%s %d%s", a "%.1f%s" value unless flag 0x10000000, the duration "%.1f%s" when `buff.duration` >= 2, a last "%s %d" (which label goes with which value is hidden by the x87 code), plus the spell1-8 key (action 0x1E + i); release = `UseSpellSlot`, right = `SelectSpellSlot` |
| CHudAbilityList 0x63F300 | rows: 0 follow ("skill0009"), 1 Science ("skill0011") -> `SelectAbilityRow` |
| CHudMainPanel 0x63FE70 | the portrait strip (0x78-byte records `+0x158`); click = portrait unit as the click target (lobby: `SendNamedEvent("%d")`). [V, deep read 2026-11-06] `BuildUnitPortraits` 0x6407E0 (selected flag): one portrait per controlled unit (`+0x16C` of the HUD): face picture "infa<m/f><me/fa/th><n>face" (m = model "unhuma", f = "unhufe"; **th when the unit's build `+0xC8` < 0.2, fa when > 0.8, else me**; n = proto `+0xC`) or "face%s%02d"; selected units get a tilted portrait (rotation about -x by g_0x7C8144 / 7); record `+0` = HP (`+0x17E`), `+0x40` face name, `+0x50` "infa<sex>", `+0x54` "<n>face", `+0x58` build, `+0x74` -1; tooltip = `UnitLabel` with the select1-3 key (actions 0x37..0x39) for the first three (none in a lobby); in a lobby every player record without a live unit in the zone gets a **grey portrait without bars** (face of its hero prototype, tooltip = player name, `+0x74` = record `+0x30`). `BuildPortraitSlot` 0x640060 (picture, alt name, selected, absent): faces blitted 4 per 256x256 render target (128-px quadrants); single player slots 56 px wide centred on x 400 ((x, 510)-(x + 56, 600)), lobby 40 px (+16 for the selected one); face quad 510..580, HP bar 587..592 and mana bar 592..597 (hidden for absent players), frame, selection overlay, button (x, 0x1FE)-(x + w, 600); at most 6 slots (index mod 6). `Update` 0x641080 (slot 24) per portrait: HP bar = `+0x17E / +0x180`, mana bar = `+0x182 / +0x184` (UV fill 9 + 64 * ratio of 256); **an HP drop** (below record `+0`) starts a red flash (alpha fades over 1 s) and face variant 2 for 2 s (variant 1 lasts 4 s); otherwise variant 3 while `+0x184` == 0 or `+0x182 / +0x184` < 0.25 [? which pair this is: live row], else 0 (`BuildFaceArt` 0x640670 on a change); a build change swaps the me / fa / th face; random idle moves (chance dt / 3 per frame while not paused: a random tilt of up to pi/12 about y and x, eased with a cosine at speed 1..2), nod (1) / shake (2) animations of 0.63 s; the tilt eases to the selected / unselected pose at 2 dt; quad 0 is a marker 60 px above the portrait of the unit whose id == g_0x79B3C4. |
| CHudMinimap 0x645060.. | 0 sling toggle (scroll direction), 1 / 2 map click -> `CCamera::SetCarrierId(-1)` + `SetTarget(world point)`, 5 reset yaw; wheel = zoom `+0xD8`. [V, deep read 2026-11-06] `Build` 0x641F20 (was named `CHudMainPanel::Build`; details in its batch row): window (0x267,0)-(800,0xA5), map size = zone record `+0x20 / +0x24` sectors x 32 m, texture = zone record `+0x18` ("battle00" without a record), `+0x168` = 2 / size, zoom `+0x16C` = min(1, size * 0.05), 7 buttons (0 (0x30C,0)-(800,0x1E) with the w_minimap key, 1 the 16-point disc, 2 (0x26C,0)-(0x30C,0xA0), 3 / 4 / 5 the three right-edge buttons (0x30C,100..0x82 / 0x82..0xA0 / 0x46..100), 6 the whole window), disc (700,80) r 64, ring r 66, frame. `Init` 0x642E80: centre `+0x14C / +0x150` = the camera point (g_0x79B388 / 38C), the map turns with the camera (angle atan2 of the camera's up axis), the 16-point disc UVs = centre + offset / zoom / (2 / size), clipped against the map edges (x, y >= 0 and <= size: Sutherland-Hodgman). `DrawMinimapUnits` 0x643DC0: markers in disc units (offset * 2 / size * zoom): **units** of the client's visible list (connection `+0x50D8`, count `+0x50DC`), not dead, within r^2 <= 0.56: an arrow along the unit's facing (tip 0.07, wings 0.04), colour 0 for the player's controlled units, 2 when the player's bit (player `+0x24`) is set in both unit masks `+0x244` and `+0x248` (hostile), else 1; the unit with id 0xA2C2A is forced to 2 on zone gz20g; **lights** of the light manager g_0x7AF748 (renderer.md) as colour-3 squares of 0.06; **zone exits** whose target is not "none" as colour-1 squares at `GetZoneExitCenter`; both squares shown inside r^2 <= 0.6, or at their unzoomed position on the rim (0.68 <= r^2, abs(x), abs(y) <= 0.97); in a lobby also the other players (connection `+0x50EC`). |
| CHudUnitInfoPanel 0x6456E0 | sliding panel (0.3 s): 0 slide toggle (sling.wav), 1..4 tabs (w_info keys): figure with damaged parts, bars, body parts "skull/torso/arm/leg" (0x647B70), stats "move_%d", "dmg_%d", "consider_more%d / less%d" (0x647040: d = level of the shown unit - level of the first selected unit, clamped to +-25; d >= 0 -> `consider_more<d>`, d < 0 -> `consider_less<|d|>`: 51 texts, e.g. more0 "Equal", more25 "Invincible", less25 "Feeble" [V machine code + texts 2026-11-03]), spell info (0x646B90). [V, deep read 2026-11-06] `Build(tab, shown)` 0x6456E0: panel (0,0)-(0xB9,0xE1), slide distance `+0x174` = 0xB4, offset `+0x170` = 0xB4 when shown else 0, state `+0x178` = shown, direction `+0x17C` = +1 / -1; buttons 0 slide (0,0)-(20,30) tip `tip 10100`, 1..4 the tabs (0,100 + 30 i)-(20,130 + 30 i) tips 10101..10104 with the w_info1-4 keys (0x2E..0x31), 5 the whole panel; quads 0 the figure holder, 1 the tab strip, 2 the arrow, 3..6 the tab icons (1.0 for the current tab `+0x180`, 0.5 others), 7 border (-5,-5)-(185,225), 8 a ruler grid (20,0)-(25,220), 9 a "more" marker (164,204)-(180,220), 10..13 two bars + frames (40..140 x 190..195 / 205..210), 14..25 six bar + frame pairs at the table 0x742434 (x 50 50 95 5 95 5) / 0x74244C (y 35 105 70 70 140 140) + (25..75 x 15..20) (the body-part bars), 26..31 six 24-px icon cells (28, 38 + 30 k); blit surface (20,0)-(180,220); texts "infounit_0..17" `+0x19C`, hl_skull / hl_torso / hl_leg / hl_arm `+0x1B0`; icon cache `+0x184..+0x198` = -1. `Draw` 0x6462A0 (slot 20): the shown unit is the hover unit `+0x150` (label `+0x168`) or else the selected one `+0x14C` (`+0x164`), only while the panel is out (`+0x178` < 1); except on tab 2 the unit's own model is drawn into the panel viewport (after a depth clear, unit moved to x = ((100 - offset) * 0.0025 - 1) * 0.4816 * 9, z 9, turned by the HUD tilt, scaled by 1 / the panel height factor, parts white on tab 0, grey 0xFF808080 on the others, flag 0x100 on the parts and 0x800 on the unit while drawing; pose and scale restored afterwards); the label wrapped in (5,5)-(155,20), font 2, beige; tab 0 = `DrawBar` + the ruler scaled 15 / (proto size `+0x15C` or `+0x160` x unit scale vt 0x518D30), tab 1 = `DrawParts` + `DrawBar`, tab 2 = `DrawStats`, tab 3 = `DrawSpellInfo`. `DrawSpellInfo` 0x646B90: the unit's **active effects** (`CUnit +0x198` records of 0x14 bytes, count `+0x19C`, at most 6 shown; > 6 shows quad 9): per effect two 15-px lines from (40,35)-(155,50): the title (first line of "spell <name>", name = spell proto `g_0x7C8510[record byte +0]` `+4`) and "%d" = record `+4` / 15 (the remaining time in seconds [I: 15 ticks per second]); icon "spell%04d" (proto `+0x60`) cached in one of six 64x64 cells of texture [1] ((0,128) (0,192) (64,0) (64,64) (64,128) (64,192)) and shown on icon quad 26 + i |
| CHudClockStrip 0x648520 | (0x2D0,0x208)-(800,600): 0 pause (clock.wav, `Game_SendLeaveFlag(!paused)`), 1 normal (g_0x78FF34 = 0x37), 2 fast (0x1B, single player), 3 the time dialog (CQuestLogWin); lobby: no speed buttons. `Update` 0x6488E0 [V, deep read 2026-11-06 with the machine code]: copies the speed mode `+0x154` to `CInterface +0x94`; game hour h = fmod(client tick g_0x7AF61C x hours per tick (CWorld `+0x20`) + CWorld `+0x1C`, 24) (not the "gtime" variable); the dial (quad 0) turns about z by (h + 15) x pi / 12 (one turn per day); pulse f = 0.7 + 0.3 cos(g_0x7C8140 x phase), phase `+0x14C` cycling 0..1 per second; the speed pointer (quad 1) sits at (800 + 73 cos a, 600 - 73 sin a) with a = 165 deg paused (blinking: shown in the second half of each second, dial scaled 1.4 - f), 135 deg normal, 105 deg fast; the four gear teeth (quads 2..5) at radius 30 turn with `+0x150` (+0.5 rad / s normal, +1 rad / s fast, still when paused; wraps at pi/6); the quest-log button (quad 6) pulses with f while `+0x158` (new quest message) is set, else scale 1 |
| CHudStanceStrip 0x649190 | (0,0x208)-(0x50,600): 0 = the battle button (`OnSelect` 0x649810: if every selected unit has `CUnit +0x438` == 1 -> 0 else 1, `SendMsg3B_SetField438(unit, v)`; the "battle mode" / Regroup of `unit-ai.md`), 1..4 crawl / sneak / walk / run = `SendMsg35_SetStance`; marker from `CUnit +0x268` |
| CHudMessagePanel 0x649AF0 | (0xB4,0)-(0x26C,100): 0 slide, 1 / 2 tabs (w_text keys); the combat log text `+0x164` (`AddCombatMessage` "combat_picked_up" / "combat_crippled", `AddMoneyMessage` "format_money", `FlushCombatMessages` 0x64A7A0) via `CTextListWin::Refresh` |

## 13. The generic controls (0x65AEA0-0x65EF80)

| Control | Construction and behaviour [V] |
|---|---|
| CScrollBar 0x65AEA0 (vt 0x742C18 / 0x742BE4, 0x18C) | (ui, l, t, r, b, depth, max, step): art "Scrollbar", quads track / thumb / up / down, buttons 0 up, 1 down, 2 track. `SetValue(max)` 0x65AFE0 sets the range (0 hides the bar), `SetPosition(v)` slot 25 (0x65B0C0) clamps and moves the thumb, `GetChanged(&v)` 0x65B090 returns 1 once; thumb drag (0x65B240 / 0x65B1A0), held arrows repeat step * 16 / s and the wheel adds step per notch into the accumulator `+0x188` (0x65B360, 0x65B430) |
| CHScrollBar 0x65BC30 (0x742CBC / 0x742C88) | the horizontal twin (quads rotated 90 deg, 0x65B8B0); the options sliders |
| CTextListWin 0x65CC00 (0x742E9C / 0x742E68, 0x1C8, a CScrollBar) | (ui, text, l, t, r, b, depth): the text is tokenised by `ScrollText_ParseEscape` 0x65D970 (`\n` paragraph, `\aF<d>` font, `\aC<rgb bytes>` colour, other `\a` = fatal "ScrollText::Incorrect special symbol") into word nodes (width by MeasureText) and format nodes (`FormatLines` 0x65D4B0), wrapped to width - 14 (`Relayout` 0x65DAA0, scroll range = total height - view), drawn line by line with full justification of inner lines (`Redraw` 0x65D150). `SetText` 0x65CFE0 restarts at 0, `Refresh` 0x65CF30 appends keeping the view anchored |
| CListBoxWin 0x65BCD0 (0x742D5C / 0x742D28, 0x174) | (ui, l, t, r, b, first string): a one-line switch: `AddString` 0x65BE70, a click cycles `SetSelection(sel + 1)` 0x65BEE0 (wraps), `GetChanged` 0x65BEB0 |
| CEditWin 0x65C010 (0x742DF4 / 0x742DC0) | (ui, l, t, r, b): `Init` 0x65C1A0 (char mask: 1 digits (`CRT_isdigit` 0x6E9379, was named _isalpha_like), 2 other letters / digits (isalnum) / space, 4 other, 8 . :, centred, font, max chars, text colour, caret colour; defaults 0xF / 0 / 1 / 50 / 0xFFFFFF / 0xFF); `OnChar` 0x65C930 inserts at the caret when the class is allowed, ctrl is not held, length < max and the text fits the rect (MeasureText); `OnKeyDown` 0x65C610 Backspace / Delete / Home / End / Left / Right; the first key on a fresh box clears it (`BeginEdit` 0x65C5C0); `SetText` 0x65C290, `SetActive` 0x65C2D0; the owner polls the dirty flag `+0x17A` |
| CTooltipControl 0x65E320 (0x742F64 / 0x742F30) | (ui, owner, text, normalLines, actionCode): lines split at `\n`; `OnMouseMove` 0x65E580 is the state machine: armed -> after tooltip_time g_0x79BE80 seconds a surface is created at the cursor (+ hot spot g_0x7C8258), kept inside 800x600, with " (<key>)" of the bound action appended to line 0; fading out over `+0x17C`; `Redraw` 0x65E880: fill 0x1C2B, frame 0x31B3FF, lines in 0xA7D7E4 then 0x31B3FF from `+0x164` |
| CRichEditSurface 0x65ED30 (0x742FD8, 0x58, a CUIBlitSurface2) | a hidden Win32 "RichEdit20A" (Riched20.dll) used as a text formatter: `StreamInFile` 0x65EE70 / `StreamInBuffer` 0x65EF80 / `StreamInFromResource` 0x65EF30 send EM_STREAMIN (0x449, SF_RTF); the credits scroller (`RichEditScroller::FormatRange / Draw / Start` 0x65F020.., outside this range) renders it |
| DInputMouse 0x65EAD0 | DirectInput 7 mouse: `Create` (GUID_SysMouse, c_dfDIMouse, NONEXCLUSIVE | FOREGROUND, fatal messages), `Acquire` 0x65EC70, `GetState` 0x65ECB0 (dx, dy, wheel, buttons at `+0x10`), `Release` 0x65EC30; polled by CSoftCursor (framework) |

## 14. What is not known

- Shop re-roll [I]: `ObjPartyRecord::MarkChildrenChanged` 0x4A88E0 sets byte `+0x38` on every object of a vector of the party
  record after `QuestComplete`; that these objects are the `CGSPlayer` merchants (so every completed quest re-rolls the shops on the
  next camp visit) is inferred from the offset only (live row in live-t5-scr-a.tsv).
- New multiplayer hero limits: `CUnitInfoWin::Redraw` greys "-" at 15 and "+" at 35 / no points left; whether the click handler
  (not read in the 2026-11-06 round) refuses the same values, or only the grey colour stops the player, is not checked.
- The addrbook.ini dword that `CLobbyScreen::Build` replaces by `GetTickCount() ^ ISAAC` when 0 is probably the client id.
- `CExchangeScreen::Exchange` 0x62D210 (a, b, c): which party is which comes from the confirmed-pair update code at 0x41F270, which
  has no function entry in the database (live row in live-t5-scr-b.tsv).
- The decompiles of `CBattleHud::IssueCommandAt` 0x638EF0, `SetCursorTarget` 0x639EF0 and `CHudMinimap::DrawMinimapUnits` 0x643DC0
  lost most branch targets (empty `if` bodies): the order / cursor / marker conditions in section 12 are [I] where marked; a
  machine-code read would settle them.
- Portrait face variant 3 tests `CUnit +0x182 / +0x184` < 0.25 (the second bar's pair, mana if `+0x17E / +0x180` is HP): live row.
- The minimap colour-3 squares come from the light manager g_0x7AF748 as read; what they show in a zone is a live check.
- The labels of the HUD spell tooltip ("infoitem_7 / 18 / 8..13 / 21 / 22 / 26..28") are paired with their values by x87 code the
  decompile hides; the text resources defining them were not found in the repository.
- ~~Tooltip id mapping~~ settled [V]: `UIButton::SetTooltipById` 0x5FC730 formats the id as the text key `tip %d` (decimal),
  reads it with `CTextManager::OpenText` + `ReadUntilHash`, and stores a new `CTooltipControl` in button +0x30 (id 0 = empty
  tooltip). So 0x4E84.. = `tip 20100`.., 0x7594.. = `tip 30100`.., 0x9CA4.. = `tip 40100`.., 0xEAC4.. = `tip 60100`..,
  0x111D4.. = `tip 70100`.., 0x138E4.. = `tip 80100`...
- ~~GetMagicTotals second output~~ settled [V] from the machine code (0x610020: `call UnitStats_MaxMana`, `fstps`, `call
  UnitStats_ManaCostFactor`, `fdivrs`, `_ftol`): `trunc(MaxMana / ManaCostFactor)` = max mana x intelligence / 25, a QUOTIENT (the
  earlier "product" inference was wrong).
- `CTooltipControl` size 0x184 confirmed [V] (`operator new(0x184)` in `UIButton::SetTooltipById`); `CEditWin` size 0x17C
  confirmed [V] (`operator new(0x17c)` in `CCampScreen2::Build`, `CLobbyScreen::Build`, `CExchangeScreen::Build`).
- ~~Where the option globals are persisted~~ settled [V]: not in an ini. They are the `AppSettings` fields `g_0x79BC70`, read by
  `AppSettings::ReadFromRegistry` 0x45B210 at start and written back by `AppSettings::WriteToRegistry` 0x45C560 (127 `CWinRegistry`
  set calls) once, at exit from `main`, under `HKCU\Software\Nival Interactive\EvilIslands` (ui-framework.md 2). keyboard.ini is
  written by `KeyBindings_Save` at exit.
- ~~`CSaveLoadWin::ScanSaves` version gates~~ settled [V] from the machine code (0x656063 / 0x656089: `cmp [stream + 8], 1 / 2`): the
  version is the stream's own field, the folder number is a separate local (0x656128); time at v >= 2, label at v >= 3.
- `COptionsScreen::GetTutorialKey` 0x650900 returns "options" only while waiting for a key — as coded, probably a bug of the original.
- ~~CCampScreen2 +0x1A0..+0x220~~ settled [V]: the full CCampScreen2 layout (+0x164..+0x21C, 33 fields) is in structs.tsv (struct
  completion 2026-11-02). ~~CCampScreen +0x168..+0x18F~~ settled 2026-11-03: `+0x168` is the hovered item (recomputed by `Draw`
  0x60F050 from the hit tests of the hero slots, quick bar and party bar) [V]; `+0x170` / `+0x180` are two STL containers of 0x28-byte
  nodes (limit 50 at `+0x17C` / `+0x18C`, the ctor's variant byte at `+0x178` / `+0x188`) that only the shared base ctor 0x5F6240 /
  0x5F6F30 builds and the dtor frees [I] (structs.tsv).

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
| ui-screens.md 4 (constructor rules) | a rune needs "sum(complexity +0x18) within the mana maximum"; a blueprint only when "its school fits GetMagicTotals" | complexity (blueprint +0x44 + runes +0x18) is checked against the party's best magic skill of the school, and mana (blueprint +0x18 + runes +0x10, the float sum the decompile drops) against the best mana budget; the blueprint itself must pass both (`CanDropItem` 0x6100E0, 2026-11-06) |
| ui-screens.md 4 (item moves) | tab 1 accepts a spell container when the hero has the magic skill | it also needs trunc(spell mana) <= trunc(MaxMana / ManaCostFactor) of the selected hero (machine code 0x6101B4..0x6101DD) |
| structs.tsv CItem +0x40 | `float slots` | an **int**: ApplyProto adds proto `+0x44` + material `+0x1C` as integers (0x6A604C) and `InitFromProto` copies the dword raw; `CanDropItem` compares it as unsigned int with the spell's reqSkill |
| ui-screens.md 4 (leave) | `OnButton(7)` ends with `Session_LeaveGame` | it calls `Game_SendEnterWorld` 0x578B20 (renamed from Session_LeaveGame), or only pops the screen when `+0x194` (opened from CCampScreen2); it also discards uncommitted skill edits and empties the component lists |
| ui-screens.md 4 (tabs) | "disabled tabs fall back" (implied all need the constructor) | spell trade needs only the spell shop, item trade / repair only the item shop; i.noconstr disables tabs 3 / 5 only |
| ui-screens.md 4 (confirm) | tab 0 "repairs the hero's weapons / inventory"; sold items registered in "`CItemRegistry` +0x1D4 ... AddUnique_*"; "empty 0x3007 containers deleted" | tab 0 repairs the weapons `+0x730` and the worn armor `+0x750` only; the registries are `ShopStock` +0x1D4 (spells) / +0x11C (items), `AddSpellProto / AddPerkProto / AddWeaponProto / AddArmorProto / AddQuickProto` (the merchant's allowed prototype lists); the deleted items are plain loot (0x3007, category 0); sold items are restored to full durability; tab 1 does not change the money |
| ui-screens.md 4 (price factors) | "deconstruct 0.2, construct 0.1" for coefficients [2] / [3] (and [6] / [7]); batch row of GetItemPrice and structs.tsv `priceCoef` "2 spell deconstruct, 3 spell construct" | `EnterMode` charges kind 2 / 6 on every component when constructing and kind 3 / 7 on the item when deconstructing: [2] / [6] = construction fee 0.2, [3] / [7] = deconstruction fee 0.1 |
| ui-screens.md 4 (prices) | `GetItemPrice` uses round(..) | every conversion is `_ftol` (0x6E853C) = truncation toward zero (machine code 0x60FAB0..0x60FB9B) |
| ui-screens.md 4 (EnterMode) | trade totals = kind 0 - kind 1; constructor = kind 2 per component | trade prices are multiplied by the stack count `+0x48`; constructor components taken from the merchant's stock also cost their buy price (kind 0 / 4); constructor counts are not multiplied |
| ui-screens.md 4 (camp Build) | "a merchant without stock gets `FillConstructorStock` 0x612CF0" | nothing calls it (slot 39 of vt 0x74091C, no `vt+0x9C` call anywhere): an unused developer "everything" stock; real stocks come from `GenerateSpellShop / GenerateItemShop` on the first visit |
| ui-screens.md 5 (CCampScreen2) | mode 10 "2..4 random hero"; mode 11 "4 = reset equipment" (batch row: "then StartGame") | mode 10: 2 / 3 are the face arrows, 4 rebuilds the hero from the current face with str / dex / int 25 (not random); mode 11: 4 discards the skill edits and re-selects the first weapon / quick item / spell, no StartGame |
| structs.tsv UnitStats +0xC..+0x20 | `int` strengthBase .. intelligence | **floats**: `UnitStats_ManaCostFactor` 0x523CD0 does `fdiv dword [ecx+0x20]`, CCampScreen2 writes 25.0f (0x41C80000) into all six |
| functions.tsv 0x6FDAE9 | `CString::Mid_first` "substring from an index to the end" | MFC `CString::Left(n)`: AllocCopy(dest, n, 0, 0), whole string when n >= length |
| ui-screens.md 4 (bars) / functions.tsv 0x622000 / 0x622860 / 0x622A30 / 0x622BC0 | `CCampBar` "8 slot buttons: 0..3 weapons, 4..7 armor"; `CCampQuickBar` "8 quick / spell slots"; `SelectArmorSlotOf` = index in "the armor array +0x740"; `CCampBar2` "0x40-px items" | `CCampBar` buttons 4..7 are the **belt** (quick items `+0x740`, button 7 - i); the armor is on 7 extra paper-doll buttons 8..14 (camp only) by armor type; `CCampQuickBar` holds the 8 **spell** containers `+0x760`; 0x622A30 selects a belt slot, 0x622BC0 a spell slot; `CCampBar2` cells are 100 px |
| ui-screens.md 4 (CCampSkillsWin) | "10 skill rows, 11 available-perk rows, learned perks" | 6 skill rows (Backstab's button has an empty rect and its line is replaced by Science), 23 available-perk rows, 10 learned-perk rows (`Build` 0x624F10, `RebuildRows` 0x6261A0) |
| functions.tsv 0x527FF0 | `Rand_Float` "returns an int from two random calls" | `Skill_RaiseCost` (fastcall ecx = from level, edx = to level) = trunc(50 * 1.61^(to/5) - 50 * 1.61^(from/5)); 0x6E81A0 is `_CIpow`, not random |
| ui-screens.md 4 (panes) | `CCampTradeWin` "two 4x5 grids" | two 4-column grids of two rows (8 cells each, the basket limit of `Party_TransferItem`) at (250,150) and (250,350) (`Draw` 0x627390) |
| ui-screens.md 4 / 5 (cross-notes from the ui-fw round, checked in the decompile) | nothing said how the hero preview and the item icons are drawn | `CCampScreen::Draw` 0x60F050 calls `EnterMode` every frame and `UIUnitPreview::Render(hero, +0x258, 400, 1)` in tab 0; `CCampScreen2::Draw` 0x6159E0 renders at x 555 / 400; `UIUnitPreview::DrawItemFigure` 0x5ECA10 tints an icon dark red (0x82804040) when the screen's slot 43 (`CanDropItem`) refuses it; `DrawFaceFigure` 0x5ED7B0 compares the race mask exactly with unhuma / unhufe |
| items.md (camp calls) | "CCampItemConstr::Deconstruct / TryConstruct" | same functions, now `CCampScreen::DeconstructItem` 0x6129C0 / `RefreshItemComponents` 0x612600 (behaviour as described there) |
| ui-screens.md 6 / functions.tsv 0x62B7D0 / 0x62BD00 (credits) | "screensaver disabled (SPI 0x4A / 0x4B)" | SPI 0x4A / 0x4B are SPI_GETFONTSMOOTHING / SPI_SETFONTSMOOTHING: font smoothing is saved in `+0x16C`, switched off and restored on close (deep read 2026-11-06) |
| ui-screens.md 5 / structs.tsv CExchangeScreen +0x1A4 | accept "when the remote money <= ours"; `Exchange` ends with `Session_LeaveGame`; +0x1A4 "second CCampBar2" | accept needs **our** offered money (`+0x198 +0x50`) <= our party money; `Update` / `Exchange` end with `Game_SendEnterWorld`; +0x1A4 is a `CCampTradeWin` (remote offer on top, ours below) |
| ui-screens.md 13 / structs.tsv CEditWin +0x158 | char mask "1 letters, 2 digits / space" | the first test of `OnChar` 0x65C930 is `CRT_isdigit` 0x6E9379 (was named _isalpha_like): **1 = digits**, 2 = other letters / digits (isalnum) / space; the exchange money edit (mask 1) is digits only |
| ui-screens.md 6 / structs.tsv CMessageBox +0x178 | the flag-4 edit "15 chars" | `Init(mask 0xF, ..., 50 chars)`: 0xF is the character mask (any character), the limit is 50 characters (`Build` 0x634790) |
| ui-screens.md 8 / structs.tsv CWorldMapScreen +0x164 / +0x190 | "visited zones (`z.<zone>` == 1)", entry "+0 figure" | every kind-0 zone of the allod with a non-empty record `+0x10` gets an entry whose `+0` is trunc(`z.<zone>`) (0 = not hoverable / clickable); outside gipat a click opens the quest window (no direct travel); travel needs a path found by `LoadZonePoints` |
| ui-screens.md 12 (Build) | the UI stream holds the "zone name" (v > 0x46); the HUD starts with the party's hero ids | it holds the combat-log text (restored into the message panel); a live start puts only the hero in the id list |
| ui-screens.md 12 (Update) / functions.tsv 0x638280 | "outside a lobby game the controlled units come from the client player's unit list" | the list rebuild from the lobby player records runs **only in a lobby game**; single player keeps the list through `OnUnitAdded` / `OnUnitRemoved` / `UpdateUnitList` |
| ui-screens.md 12 (strips) / structs.tsv CBattleHud +0x198 | `CHudQuickBar` "8 belt slots" | 4 belt quick items + up to 4 quest items (kind 0x3009) of the player for an owned unit (`SetUnit` 0x63D2B0) |
| functions.tsv 0x641F20 | `CHudMainPanel::Build` | runs on the `CHudMinimap` object (+0x1AC, vt 0x742660) from `CBattleHud::Build`: renamed `CHudMinimap::Build` |
| ui-screens.md 12 (orders) | "another unit = SendInteractOrder and, when hostile, SendAttackOrder" | object vt+0xDC is `CUnit +0x274` dead (slot 55): a corpse gets `SendInteractOrder` (loot), a living hostile unit `SendAttackOrder`, other living units a move [I: branches lost in the decompile] |

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
| 0x612CF0 | CCampScreen::FillConstructorStock | unused developer stock (no caller) |
| 0x613EA0..0x619150 | CCampScreen2 | multiplayer party / hero creation |
| 0x61B2C0..0x61D780 | CLobbyScreen | server list, addrbook.ini, host / join |
| 0x620380..0x621F30 | CCampBar2 | item strip with category tabs |
| 0x622000..0x622D40 | CCampBar / CCampQuickBar | weapons + belt row and armor doll / the 8 spell slots |
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
| 0x641F20 / 0x642E80 / 0x643DC0 | CHudMinimap::Build / Init / DrawMinimapUnits | minimap window, disc geometry, unit / light / exit markers (0x641F20 was CHudMainPanel::Build) |
| 0x6312C0 | CWorldMapScreen::LoadZonePoints | travel path search over the zone exits (z. passability per zone kind) |
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
