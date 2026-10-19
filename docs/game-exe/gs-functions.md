# WorldScript (GS) function table

Source: game.exe g_0x78BCA0, 225 records of 16 bytes {name*, id, signature*, params*}; dumped by `_cpr/claude-re/full/tools/gsfuncs.py`. The id is the switch case in `SSCScript::Run` (0x4B6760, commands), `SSCCmd::EvalFloat`, EvalObject, EvalGroup. Params: f float, s string, o object, g group; a leading capital in the signature is the return type (F float, S string, O object, G group); commands have no return type.

| id | name | kind | params |
|---|---|---|---|
| 0x6d | ( | syntax |  |
| 0x6e | ) | syntax |  |
| 0x71 | , | syntax |  |
| 0x73 | : | syntax |  |
| 0x72 | = | syntax |  |
| 0x74 | WorldScript | command |  |
| 0x75 | Script | command |  |
| 0x76 | DeclareScript | command |  |
| 0x77 | GlobalVars | command |  |
| 0x6f | if | syntax |  |
| 0x70 | then | syntax |  |
| 0x95 | ActivateTrap | command | of |
| 0xe0 | AddLoot | command | fss |
| 0xc3 | AddRectToArea | command | fffff |
| 0xc4 | AddRoundToArea | command | ffff |
| 0xa0 | AddMob | command | s |
| 0x22 | AddObject | command | go |
| 0x86 | AddUnitToParty | command | fss |
| 0x88 | AddUnitToServer | command | sssfff |
| 0x83 | AddUnitUnderControl | command | fo |
| 0x12 | AttachParticleSource | command | ff |
| 0xe7 | AttachParticles | command | fo |
| 0x3c | Attack | command | oo |
| 0x32 | BlockUnit | command | of |
| 0x3d | Cast | command | off |
| 0xa8 | CastSpellPoint | command | sffff |
| 0xa9 | CastSpellUnit | command | sffo |
| 0x7d | ConsoleFloat | command | f |
| 0x7e | ConsoleString | command | s |
| 0xd4 | CopyItems | command | fss |
| 0xcb | CopyLoot | command | fss |
| 0x9d | CopyStats | command | fss |
| 0x3e | Crawl | command | o |
| 0x17 | CreateFX | command | fffffs |
| 0x16 | CreateFXSource | command | ffffffs |
| 0xf | CreateLightning | command | ffffffff |
| 0xe | CreateParticleSource | command | fffffs |
| 0x9a | CreateParty | command | fs |
| 0xb | CreatePointLight | command | ffffffff |
| 0x18 | CreateRandomizedFXSource | command | fffffffffs |
| 0xc6 | DeleteArea | command | f |
| 0x19 | DeleteFXSource | command | f |
| 0x1a | DeleteLightning | command | f |
| 0x11 | DeleteParticleSource | command | f |
| 0xd | DeletePointLight | command | f |
| 0x3b | Follow | command | oo |
| 0xcc | EnableLever | command | of |
| 0x93 | EraseQuestItem | command | ff |
| 0xdf | FixItems | command |  |
| 0xb7 | FixWorldTime | command | f |
| 0x7f | GSSetVar | command | fsf |
| 0x98 | GSSetVarMax | command | fsf |
| 0x82 | GSDelVar | command | fs |
| 0xcf | GiveDexterity | command | of |
| 0xd0 | GiveIntelligence | command | of |
| 0xa6 | GiveItem | command | fs |
| 0xa7 | GiveMoney | command | ff |
| 0x92 | GiveQuestItem | command | fs |
| 0xcd | GiveSkill | command | osf |
| 0xce | GiveStrength | command | of |
| 0xaf | GiveUnitQuestItem | command | os |
| 0xb3 | GiveUnitSpell | command | os |
| 0x97 | GodMode | command | ff |
| 0x33 | Guard | command | offf |
| 0xc2 | HideObject | command | of |
| 0x34 | Idle | command | o |
| 0x43 | InvokeAlarm | command | fff |
| 0x44 | InflictDamage | command | of |
| 0x79 | KillScript | command |  |
| 0x35 | KillUnit | command | o |
| 0x8a | LeaveToZone | command | fsf |
| 0xc9 | Lie | command | o |
| 0x10 | MoveParticleSource | command | ffffff |
| 0xc | MovePointLight | command | ffffff |
| 0x2 | MoveToPoint | command | off |
| 0x4 | MoveToObject | command | oo |
| 0x1b | PlayAnimation | command | os |
| 0x1c | PlayFX | command | ffffs |
| 0xa1 | PlayCamera | command | s |
| 0xc1 | PlayMovie | command | s |
| 0x1d | PlayMusic | command | fs |
| 0xa4 | SetPlayerAggression | command | ff |
| 0xd5 | QStart | command | s |
| 0xd6 | QFinish | command |  |
| 0xd7 | QObjArea | command | f |
| 0xd8 | QObjSeeObject | command | s |
| 0xd9 | QObjSeeUnit | command | s |
| 0xda | QObjKillUnit | command | s |
| 0xdb | QObjKillGroup | command | s |
| 0xdc | QObjUse | command | sf |
| 0xdd | QObjGetItem | command | f |
| 0x9f | QuestComplete | command | fs |
| 0xd2 | RecalcMercBriefings | command |  |
| 0xd3 | RedeployParty | command | f |
| 0x9b | RemoveParty | command | fs |
| 0x23 | RemoveObject | command | go |
| 0x96 | RemoveObjectFromServer | command | o |
| 0xae | RemoveQuestItem | command | of |
| 0x84 | RemoveUnitFromControl | command | fo |
| 0x87 | RemoveUnitFromParty | command | fs |
| 0x89 | RemoveUnitFromServer | command | o |
| 0xe8 | ResetTarget | command | o |
| 0xb0 | Rest | command | o |
| 0x3 | RotateTo | command | off |
| 0x41 | Run | command | o |
| 0xb6 | RunWorldTime | command | f |
| 0x39 | SendEvent | command | ff |
| 0x3a | SendStringEvent | command | fs |
| 0x94 | Sentry | command | off |
| 0x1f | SetBackGroundColor | command | fff |
| 0x14 | SetCameraOrientation | command | f |
| 0x13 | SetCameraPosition | command | fff |
| 0x8e | SetCP | command | offf |
| 0xad | SetCPFast | command | offf |
| 0x9c | SetCurrentParty | command | fs |
| 0x37 | SetDiplomacy | command | fff |
| 0x6 | SetDirectionToObject | command | f |
| 0x38 | SetEnemy | command | of |
| 0xba | SetParticleSourceSize | command | ff |
| 0x36 | SetPlayer | command | of |
| 0xac | SetScience | command | offffff |
| 0x42 | SetSpellAggression | command | of |
| 0x1 | SetSunLight | command | fffffffff |
| 0xb8 | SetWaterLevel | command | fff |
| 0x1e | SetWind | command | ffff |
| 0x7 | Sleep | command | f |
| 0xde | SleepUntil | command | f |
| 0xb9 | SleepUntilIdle | command | o |
| 0x3f | Stand | command | o |
| 0x5 | StartAnimation | command | os |
| 0xa3 | ShowCredits | command | s |
| 0x21 | ShowBitmap | command | fs |
| 0x91 | SwitchLeverState | command | of |
| 0x9e | SwitchLeverStateEx | command | off |
| 0xc8 | UMAg | command | of |
| 0x2d | UMAggression | command | o |
| 0x24 | UMClear | command | o |
| 0x30 | UMCorpseWatcher | command | o |
| 0xc7 | UMFear | command | of |
| 0xca | UMFollow | command | oo |
| 0x26 | UMGuard | command | offf |
| 0x27 | UMGuardEx | command | offff |
| 0x28 | UMPatrol | command | o |
| 0x2a | UMPatrolAddPoint | command | off |
| 0x2b | UMPatrolAddPointLook | command | offf |
| 0x29 | UMPatrolClear | command | o |
| 0x31 | UMPlayer | command | o |
| 0x2e | UMRevenge | command | o |
| 0x25 | UMSentry | command | off |
| 0x2c | UMStandard | command | o |
| 0x2f | UMSuspection | command | o |
| 0xa | WaitEndAnimation | command |  |
| 0x9 | WaitSegment | command | f |
| 0x40 | Walk | command | o |
| 0x6b | For | command | Og |
| 0x6c | ForIf | command | Ogf |
| 0x68 | Every | function -> F | Ogf |
| 0x69 | Any | function -> F | Ogf |
| 0x6a | Sum | function -> F | Ogf |
| 0x45 | IsEqual | function -> F | ff |
| 0x46 | IsLess | function -> F | ff |
| 0x47 | IsGreater | function -> F | ff |
| 0x48 | IsEqualString | function -> F | ss |
| 0x49 | Add | function -> F | ff |
| 0x4a | Sub | function -> F | ff |
| 0x4b | Mul | function -> F | ff |
| 0x4c | Div | function -> F | ff |
| 0x4d | Not | function -> F | f |
| 0x7c | HaveItem | function -> F | ff |
| 0x4e | GetObject | function -> O | f |
| 0x85 | GetObjectByName | function -> O | s |
| 0xab | GetObjectByID | function -> O | s |
| 0xbb | IsInSquare | function -> F | ffffff |
| 0xbc | UnitInSquare | function -> F | offff |
| 0xd1 | GetMercsNumber | function -> F | f |
| 0xbd | GetZValue | function -> F | ff |
| 0xbe | GetBSZValue | function -> F | ff |
| 0x4f | HP | function -> F | o |
| 0x50 | MaxHP | function -> F | o |
| 0x51 | Mana | function -> F | o |
| 0x52 | MaxMana | function -> F | o |
| 0x53 | DistanceUnitUnit | function -> F | oo |
| 0x54 | DistanceUnitPoint | function -> F | off |
| 0x5f | Random | function -> F | f |
| 0x90 | GetLeverState | function -> F | o |
| 0x55 | GetX | function -> F | o |
| 0x56 | GetY | function -> F | o |
| 0x57 | GetZ | function -> F | o |
| 0xbf | GetFutureX | function -> F | of |
| 0xc0 | GetFutureY | function -> F | of |
| 0x58 | GroupSize | function -> F | g |
| 0x59 | GroupAdd | function -> G | gg |
| 0x5a | GroupSub | function -> G | gg |
| 0x5b | GroupCross | function -> G | gg |
| 0x5c | GroupHas | function -> F | go |
| 0x80 | GSGetVar | function -> F | fs |
| 0x99 | IsUnitInWater | function -> F | o |
| 0x5d | UnitSee | function -> G | o |
| 0x5e | GroupSee | function -> G | g |
| 0xa5 | PlayerSee | function -> G | f |
| 0x60 | GetDiplomacy | function -> F | ff |
| 0x61 | IsEnemy | function -> F | of |
| 0x63 | IsAlive | function -> F | o |
| 0xa2 | IsCameraPlaying | function -> F |  |
| 0x64 | IsDead | function -> F | o |
| 0xb1 | IsPlayerInDanger | function -> F | f |
| 0xb2 | IsPlayerInSafety | function -> F | f |
| 0x62 | GetPlayerUnits | function -> G | f |
| 0xb4 | GetUnitOfPlayer | function -> O | ff |
| 0x65 | GetPlayer | function -> F | o |
| 0x66 | GetObjectID | function -> F | o |
| 0x8f | GetAIClass | function -> F | o |
| 0xe1 | GetLootItemsCount | function -> F | f |
| 0xe2 | GetMoney | function -> F | f |
| 0x67 | IsAlarm | function -> F | f |
| 0x8b | AlarmTime | function -> F | f |
| 0x8c | AlarmPosX | function -> F | f |
| 0x8d | AlarmPosY | function -> F | f |
| 0xaa | IsNight | function -> F |  |
| 0xb5 | GetWorldTime | function -> F |  |
| 0xc5 | IsInArea | function -> F | fff |
| 0xe3 | WasLooted | function -> F | o |
| 0xe5 | IsUnitVisible | function -> F | o |
| 0xe4 | GetLeader | function -> O |  |
| 0xe6 | IsUnitBlocked | function -> F | o |
