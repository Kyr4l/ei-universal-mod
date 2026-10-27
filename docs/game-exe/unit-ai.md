# The unit AI in game.exe (perception, AiMemory, Brain, the AiLogic motivations)

Written 2026-10-27 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe`: the perception module
0x5C8B00-0x5CDF00 (`StimulusMap`, `AiMemory`), the spell scoring 0x5CDFB0-0x5D0A50 (`AiInfo`, `AiSpellInfo`, `Brain::ChooseTarget`),
the motivations 0x5D0A50-0x5D3760 (`AiLogic*`), the party logic and the `Brain` 0x5D3760-0x5DA2B0, plus the callers that matter
(`CUnitServer::Update` 0x552300, `AiAcknowledge` 0x5AEA40, `PlayerRelations` 0x4554B0, the `.mob` logic reader 0x46CBC0, the script
commands in `gs-functions.md`). It supersedes `docs/monster-ai.md` (2026-10-20) and the AI paragraphs of `docs/game-exe/hierarchy.md`;
every disagreement is listed in section 11. Movement itself (`CAIMap::FindPath`, `CMover`) is `docs/game-exe/pathfinding.md`; the combat
orders the AI issues (`CUnitServer::QueueOrder*`, `ApproachTarget`, `TakeHit`) are read by the combat page, here only their parameters.

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions (strong), **[?]** guess / not resolved.
Addresses are VAs in game.exe; offsets are bytes; "tick" = server world tick (`CWorldServer +0x64`, 0x7AFFC4, ~18/s); distances in metres.
The range 0x470000-0x474000 named in the brief holds no AI code (it is the 3DS scene converter); the motivation name strings live in
.rdata at 0x78D354 / 0x791E4C.. and only `Brain::Tick` reads them.

## How to re-implement it (short version)

1. **Every living unit, every tick** (`CUnitServer::Update` 0x552300): `AiMemory::ComputeDetectionFactors` (player units only, the eye /
   ear icons), `AiAcknowledge::Tick` (voice lines), **`Brain::Tick`** 0x5D53B0, `PropagateAlarm`, then the order machine (`Think`, `TickState`).
   There is no AI thread, no per-N-tick scheduling of the brain; only the neighbour scan (every 8..11 ticks) and the follow logic (every 8) are throttled.
2. **Perception = AiMemory::Tick inside Brain::Tick**: the neighbour list (units within `2 x max(sight range, sense-life range)`), then for every
   neighbour a *vision* test (range product, view cone, `CAIMap::TraceVisibility`), a *peripheral* test and a *sense-life* test; a seen unit stays
   seen until it is farther than 1.05 x both ranges, **except that an AI unit never drops a seen enemy by distance** (only by death / forget). **Hearing** is event driven: footsteps, attacks, casts, loot and spell hits call
   `StimulusMap::HearSound`, which gives every enemy listener within `hearing x loudness` a *heard sound record* that fires 26 ticks later and sets the
   **noticed position** with an *interest* of 900 that decays by 3 per tick. Smell and the footstep `StimulusMap` grid are **dead code**.
3. **One brain per unit** (`CUnitServer +0x980`): a list of *motivations* (`AiLogic*` objects, kinds 0..10). Each tick every motivation's `Evaluate`
   returns a priority (stationary logics 50, Suspection = the interest, Aggression 10000 when it has a target, Fear 15000 / 3000 / interest / 6, Player 10,
   Tamed 1e8); the current one gets +100; the best becomes current (`End` on the old, `Begin` on the new) and its `Update` runs. The motivation set comes
   from the unit's `.mob` logic slot (`Brain::BuildFromLogicSlot` 0x5D6630: Model + AggrMode) or from script commands.
4. **Targets**: `Brain::ChooseTarget` 0x5D0030 scores every (spell or weapon) x (candidate) pair with `AiSpellInfo_Score` 0x5CF9A0 (damage minus the
   target's absorption, minus 25 per metre out of range, minus 2 x the hits needed to kill) and keeps the best that is reachable
   (`Brain::TryAttackTarget` = a path + `CanReachOrderTargetFrom`). Candidates: the seen enemies (`AiMemory::CollectHostiles`) for monsters, the
   units attacking the party for heroes (`CollectAttackers` + `CollectCallers`).
5. **Outputs** are queued unit orders: `QueueOrderMoveTo(pos, 0, 0, 1e6)`, `QueueOrderTurnTo`, `QueueOrderAttack(target, 6, 0, run)`,
   `QueueCastOn(slot, target, 0)`, `QueueOrderKindB` (idle animation), plus `CallForHelp` (a heard-sound record in every friend within the Help radius)
   and the **alarm** (`CWorldServer::ActivateLogicSlot`: every AI unit switches to another `.mob` logic slot).
6. **Relations** are two 32-bit masks per unit: `CUnit +0x244 alarmMask` (sides that are enemies: initial = the `.mob` diplomacy's forced row, grows
   when attacked / alarmed / `SetEnemy`) and `+0x248 attackableMask` (default all; taming clears bits); `IsPlayerInMask(me, other) = bit(other.side) & both`.
   "Friend" everywhere else means `PlayerRelations` row bit (`g_0x7C2BC4[my side] & bit(other side)`).

## 1. Conventions

| Thing | Value |
|---|---|
| unit record | `CUnit` (+0x1c/+0x20/+0x24 position, +0x34 quaternion, +0x250 side = .mob player number, +0x24c owning CPlayer or 0 = AI, +0x240 the `CUnitServer`) |
| server part | `CUnitServer` (`CUnit +0x240`): +0x1e0 `UnitStats`, +0x164 state (0 walking .. 5 strike 6 idle), +0x190 current order / +0x1b4 queued order, **+0x980 Brain (0x190 bytes, embedded)**, +0xa50 AiMemory (= brain +0xd0), +0xb10 AiAcknowledge |
| brain pointer arithmetic | the decompiles use `brain[1].field_x` = brain +0xc4 + x and `brain[2].field_x` = brain +0x188 + x (Ghidra sized `Brain` at 0xc4) |
| "enemy" | `CUnit::IsPlayerInMask(me, other)` 0x530150 = `(1 << other.side) & me.alarmMask & me.attackableMask` [V] |
| "friend" | `g_0x7C2BC4.friends[me.side] & (1 << other.side)` (`PlayerRelations`, loaded from the .mob diplomacy; `CWorldServer +0x12c64` is a second instance of the same class used by the server-side combat code) [V] |
| interest | `AiMemory +0x18`, 0..1000; decays every tick by `+0x1c` (negative); the Suspection priority |
| priority | int returned by a motivation's `Evaluate`; the current motivation gets +100; Guard/Sentry/Follow 50, Patrol 50 / -1000, Suspection = stay (interest), Aggression 10000 / -1000, CorpseWatcher -1000, Fear 15000 / 3000 / 350 / interest/6 / -1000, Player 10, Tamed 100000000 [V] |
| random | `Isaac_NextWord(0x7A9D40)` / the table `g_0x7A9D48[DAT_0x7A9D44]`: `x % 1001 == 1` is the 1/1001 "idle sound" roll, `x % 11 == 0` the 1/11 "random animation" roll |
| AiInfo | the global block `g_0x7C53F8` (section 3.1); the values `DAT_0x7C53FC..` in older notes are its fields |

## 2. The tick

### 2.1 `CUnitServer::Update` 0x552300 (vt slot 6, every tick for every unit the world updates) [V]

Alive (`CUnit +0x274 == 0`):
1. stats regenerate / recompute (`Placement_Regenerate`, `UnitStats_Recompute2`);
2. **footstep**: when `(worldTick & +0x914 footstepTickMask) == 0`: `StimulusMap::EmitFootstep` 0x5C9770 (section 4.4);
3. `TickPassiveSpells`;
4. player-owned units (`+0x24c != 0`): `AiMemory::ComputeDetectionFactors` -> `CUnit +0x4c8` (replicated, section 4.6); the idle-voice counter `+0x938`;
5. `AiAcknowledge::Tick` 0x5AEA80 (section 5.5);
6. **`Brain::Tick`** 0x5D53B0;
7. `CUnitServer::PropagateAlarm` 0x552E00 (section 5.3);
8. states 0..2 / 6 without the flag 0x1000000 and `+0x144 == 0`: `Think` 0x54B390 (dispatches the queued order), `TickState`, `TickAnimation` (combat page).

Dead: sinks, once `OnUnitDied`, `Brain::Reset` (AI units) / the player's vt[20], the orders set to kind 9.

**Which units are updated**: `CWorldServer::TickActiveObjects` 0x58D670 calls vt[6] on every object of the *active* hash `+0x104` (mode 1) or of all
objects; a unit's countdown `CMapObject +0x174` (set by `CWorldServer::ActivateObject` 0x58D5D0 to `(ptr >> 5 & 0xf) + 0x80` ticks) deactivates it unless
`CUnitServer +0x174 fighting`, `brain +0x37 alwaysActive` or `brain +0x18c enemySeen` is set [V]. Units are (re)activated by damage (`TakeHit`),
by their `.mob` AlwaysActive bit (`BuildFromLogicSlot`) and by `ActivateLogicSlot` (alarm). [?] what else activates sleeping units (the player's
vicinity is the obvious candidate: not read).

### 2.2 `Brain::Tick` 0x5D53B0, order of operations [V]

```
1  alarm countdown: +0x34 alarmFlag && +0x40 wait > 0 -> wait--; at 0 and +0x35 alarmSlot != 0:
     CWorldServer::ActivateLogicSlot(alarmSlot, unit pos, force 0) ; AiAcknowledge::Request(8)        (section 5.4)
2  +0x4c idleSpeedFactor = (30 f + 1) / 31                                                             (towards 1.0)
3  AiMemory::Tick 0x5CB700  -> ignore pairs ttl--, heard sounds, StimulusMap::UpdateVision, interest += decay   (section 4)
4  +0x18c enemySeen = some unit of the SEEN list is IsPlayerInMask-enemy and alive
5  return while the unit is in state 5 (strike / dying)
6  enemySeen && !alarmFlag && alarmCondition +0x3c in {0, 2} -> alarmFlag = 1
7  MOTIVATION: best = current ? current->Evaluate(brain) + 100 : none (must be > 1);
     for every other logic: p = logic->Evaluate(brain); p > best -> best
     change -> log "Unit %i new motivation %s with power of %i"; kinds 3/4/5/7 with alarmCondition == 2 -> alarmFlag = 1;
               old->End(brain) (slot 7); new->Begin(brain) (slot 6); current = new
8  current ? current->Update(brain) (slot 5) : (target +0x2c released, order kind 0)
9  attacker +0x30 released; the friendly-corpse list (AiMemory +0x78) emptied
10 ORDER bookkeeping by brain +4:
     0 -> CUnitServer::ClearQueuedOrder
     1 (move) -> kind 0 once the unit's current / queued order is neither 1 nor 2
     2 (move + idle actions) -> Brain::TickMoveOrder 0x5D7520                                           (section 8.2)
     3 (attack) -> Brain::CheckOrderTarget 0x5D76A0
     4 (cast) -> kind 0 once no cast order (4 / 5) remains; the target goes to the ignore pairs for 15 ticks when enemySeen
     5 (interact) -> kind 0 once no interact order (6) remains
     6 (follow) -> Brain::TickFollow 0x5D6F90                                                           (section 8.3)
```
Note that `Evaluate` of *every* motivation runs every tick, and that several `Evaluate`s have side effects (Suspection adopts the noticed position,
CorpseWatcher pops a corpse, Aggression picks its target, Fear computes its kiting spot).

## 3. Data

### 3.1 `AiInfo` (global 0x7C53F8, 0xcc bytes; static defaults `AiInfo_StaticInit` 0x5CDFB0, overridden by `aiinfo.res` in `AiInfo_Load` 0x5CE0F0 from `main`) [V]

| off | key | default | used by |
|---|---|---|---|
| +4 | [Logic] GuardStayTime | 10 | guard idle time (x 15 ticks per turn), sentry with 1/11 chance |
| +8 | SentryStayTime | 10 | sentry idle time |
| +0xc | FollowMinDist | 2 | `TickFollow`: stop inside |
| +0x10 | FollowMaxDist | 4 | `TickFollow`: walk beyond; party cohesion `2 x 4 + 1 = 9 m` run / `1.4 x 4 + 1 = 6.6 m` walk |
| +0x14 | GuardRadius | 10 | default guard radius (`GuardPoint_Init` when < 0) |
| +0x18 | CallForHelpRadius | 7 | `brain +0x38` when the .mob Help equals CallForHelpRadiusDefault |
| +0x1c | CallForHelpRadiusDefault | 7 | the "unset" marker |
| +0x20 | FollowLimit | 15 | **no reader** |
| +0x24 | PlayerCallForHelp | 16 | `Brain::CollectCallers` radius (party help) |
| +0x28 | RangedRunDist | 4 | Fear kiting distance while Aggression is current |
| +0x2c | RangedAttackDist | 7 | Fear kiting distance otherwise |
| +0x30 | SwarmRadius | 10 | `Regroup`: attackers around the move target |
| +0x34.. | [RPG] HumanCrossAnimationSpeed 1, BackstabAdd 3, BackstabMult 0, RotateSpeedMult 100, DamageLevel1 0.5, DamageLevel1Value 0.5, DamageLevel2 0, DamageLevel2Value 0.33, DamageLevelRunLimit 0.5, Int1ExpBonus 0.2, HP_Val_1..5 25 30 50 1.1 1.61, MP_Val_1..5 same, Skill_Val_1 50, Skill_Val_2 5, Perk_Power_Base 2, Consider_Modifier 200, Consider_Log_Base 1.61, Item_Durability_Critical 0.3 (+0xa4) | | combat / stats (not AI) |
| +0x98 / +0x9c | [LMP] Lost_Money 0.1, Lost_XP 0.1; +0xa0 [Scenario] QuestsToRespawn 3 (int); +0xa8 / +0xb4 / +0xc0 [DifficultyLevels] Absorption / Attack / Defence [3] = 1 | | |

`normal.dat` of `aiinfo.res` (0x400 dwords -> `g_0x7C43C4`, loaded by `StimulusMap::Load`) has no reader in the AI code read here [?].

### 3.2 The senses (`UnitStats`, `CUnitServer +0x1e0 + off`; units.db columns "Senses *" / "Detections *" / "Skills Peripheral") [V]

| CUnitServer | stats | column | meaning |
|---|---|---|---|
| +0x224 | +0x44 | (race table) | `viewConeCos`: cos of the half view angle; heroes -2 (see all round) |
| +0x228 | +0x48 | Skills Peripheral | peripheral vision range |
| +0x290 / +0x294 | +0xb0 / +0xb4 | Senses Sight | base / current sight range |
| +0x298 / +0x29c | +0xb8 / +0xbc | Senses Infra | night vision in % (perk 0xf adds `level x infra / 100`) |
| +0x2a0 / +0x2a4 | +0xc0 / +0xc4 | Senses SenseLife | life sense range |
| +0x2a8 / +0x2ac | +0xc8 / +0xcc | Senses Hearing | hearing range (x loudness) |
| +0x2b0 / +0x2b4 | +0xd0 / +0xd4 | Senses Smell | smell (dead feature) |
| +0x2b8 / +0x2bc | +0xd8 / +0xdc | Senses Tracking | footstep sight (dead feature) |
| +0x2c0 / +0x2c4 | +0xe0 / +0xe4 | Detections Sight | **visibility**: how far others see this unit (buffs subtract %) |
| +0x2c8 / +0x2cc | +0xe8 / +0xec | Detections Infra | no reader found |
| +0x2d0 / +0x2d4 | +0xf0 / +0xf4 | Detections SenseLife | life signature |
| +0x2d8 / +0x2dc | +0xf8 / +0xfc | Detections Hearing | **noise**: footstep / attack loudness |
| +0x2e0 / +0x2e4 | +0x100 / +0x104 | Detections Smell | scent (only the detection meter reads it) |
| +0x2e8 / +0x2ec | +0x108 / +0x10c | Detections Tracking | no reader |

`UnitStats_Recompute2` 0x524560 copies base -> current for the 12 pairs, then adds the buffs (kinds 0xc..0x13 of its table: sight, infra, sense life
as + values; visibility, noise, life signature as - values; scent +).

**Per-tick multipliers** `CUnitServer::UpdatePerceptionFactors` 0x5536B0 (vt slot 45) [V]:

| field | value |
|---|---|
| +0x904 sightMult | `world+0x88 weatherSight (1.0; 0.9 in rain) x ((1 - night) + night x infra% / 100)`, `night = CWorld::NightFactor` (world +0x84) |
| +0x900 visibilityMult | 1; crawl (stance 0) 0.5; sneak (1) 0.75; x 1.5 while casting a spell whose proto +0x10 != 1 |
| +0x8f8 footstepMult | walking (state 0): crawl 0.4, sneak 0.2, walk 1, run (stance 3) 2; not walking 0 |
| +0x8fc hearingMult | 1; 0.5 while running |
| +0x914 footstepTickMask | from the speed vt[59]: > 1.2 -> 0 (footstep every tick), > 0.6 -> 1, > 0.3 -> 3, > 0.15 -> 7, else 0xf (every 16 ticks, also when not walking) |

### 3.3 `AiMemory` (0xb8 bytes at `CUnitServer +0xa50` = brain +0xd0) [V]

| off | field | meaning |
|---|---|---|
| +0x10 / +0x14 | noticedPos | xy of the last stimulus: lost unit, heard sound, friendly corpse |
| +0x18 | interest | 0..1000 (CUnitServer +0xa68): 1000 lost unit, 900 sound, 800 corpse; `HearSound` ignores listeners at >= 900 |
| +0x1c | decay | added to interest every tick: -1 footstep (dead), -2 lost unit, -3 sound / corpse (doubles as the stimulus kind) |
| +0x20 | refreshCountdown | neighbour scan every (rand & 3) + 8 ticks |
| +0x24 | neighbours (ObjRefList, head +0x28 = CUnitServer +0xa78) | every unit within `2 x max(sightMult x sight, senseLife)` (sector granularity 32 m) |
| +0x40 | seen (head +0x44 = +0xa94) | units currently perceived; the only source of enemies for the AI |
| +0x5c | corpses | seen dead units |
| +0x78 | friendCorpses (count +0x84) | seen corpses of friend sides; emptied every tick; CorpseWatcher pops them |
| +0x94 | ignorePairs {ref, ttl} (data +0x98, count +0x9c) | targets ChooseTarget skips: 20 ticks when unreachable as the only option, 15 after a finished cast |
| +0xa8 | heardSounds (CArray of 0x18 `{pos, delay, alarmMask, text}`, data +0xac, count +0xb0; = CUnitServer +0xaf8) | pending reactions (section 4.4) |

### 3.4 `Brain` (0x190 bytes at `CUnitServer +0x980`) [V]

| off | field | meaning |
|---|---|---|
| +0 | owner | CUnitServer |
| +4 | orderKind | 0 none, 1 move, 2 move + idle actions, 3 attack, 4 cast, 5 interact, 6 follow |
| +8 | orderPos | the move goal |
| +0x14 | actions (CArray, data +0x18, count +0x1c) | `PatrolAction {angle, turnSpeed, duration, playAnim}` list run after a kind-2 move |
| +0x28 | actionTimer | -10000 idle, -9999 turning, else the tick the current wait ends; a cast order stores the spell slot here |
| +0x2c | target | order target (ref) |
| +0x30 | attacker | the last attacker (ref), set by `Brain::OnAttackedBy`, released every tick after the logic update |
| +0x34 / +0x35 | alarmFlag / alarmSlot | section 5.4 (.mob NumAlarm) |
| +0x36 / +0x37 | currentSlot / alwaysActive | .mob logic slot in use; AlwaysActive bit |
| +0x38 | callForHelpRadius | .mob Help (7 default); also `PropagateAlarm`'s radius (+0x9b8) |
| +0x3c | alarmCondition | .mob AlarmCondition 0 / 1 / 2 |
| +0x40 | alarmWait | .mob Wait ticks (min 1) |
| +0x44 / +0x48 | nearRange / farRange | radius + 35 / + 80 (guard) or 35 / 80; 100000 after a reset; no reader found [?] |
| +0x4c | idleSpeedFactor | random 0.8..1.0 on a stationary `Begin`, decays to 1; no reader found outside the brain [?] |
| +0x50 / +0x5c | prevWaypoint / curWaypoint | post / patrol points (-1313 none); `IsNearPath` |
| +0x68 | spotRetryCountdown | `FindRandomReachableSpot` backoff |
| +0x6c | followTick | `TickFollow` every 8 |
| +0x70 | logics (ObjRefList, head +0x74 = CUnitServer +0x9f4) | the motivations |
| +0x8c | currentLogic | |
| +0x90 | mobLogicSlots (CArray of 0x4c `MobLogicSlot`, data +0x94, count +0x98) | the 5 logic records of the `.mob` unit |
| +0xa4 | spells (`AiSpellInfos`: attack table data +0xa8 / count +0xac, support table +0xbc / +0xc0, max level +0xcc) | section 7.1 |
| +0xd0 | memory | AiMemory |
| +0x18c | enemySeen (byte, = CUnitServer +0xb0c) | a seen unit is an enemy and alive (step 4 of the tick) |

### 3.5 The motivation classes (13-slot vtables; `docs/game-exe/hierarchy.md` has the slot tables) [V]

Slots: 0 dtor, 1 copy, 2 Reset, 3 Clone, **4 Evaluate(brain) -> priority**, **5 Update(brain)** (only the current one), **6 Begin(brain)** (becomes
current), **7 End(brain)** (stops being current), 8 GetKind, 9 IsStationary, 10 OnObjectRemoved, 11 Read, 12 Write. (Earlier notes called 6 "Leave"
and 7 unknown: `Brain::Tick` calls slot 7 on the old current and slot 6 on the new one.)

| kind | class (vtable, size) | ctor | fields | mob / script source |
|---|---|---|---|---|
| 0 | AiLogicGuard (0x73D128, 0x20) | 0x4BA1C0 | +8 point, +0x14 radius, +0x18 stayTime, +0x1c failedSpots | Model 1 / 5; Guard, UMGuard, UMGuardEx |
| 1 | AiLogicPatrol (0x73FF40, 0x20) | 0x5D0F60 | +8 points CArray (data +0xc, count +0x10), +0x1c index | Model 2; UMPatrol* |
| 2 | AiLogicSentry (0x73D190, 0x14) | 0x4BA360 | +8 point | Model 3; Sentry, UMSentry |
| 10 | AiLogicFollow (0x73D1C4, 0xc) | 0x4BA450 | +8 target | UMFollow |
| 3 | AiLogicSuspection (0x73D1F8, 0x18) | 0x4BA610 | +8 point, +0x14 stay | AggrMode 0 / 1; UMSuspection, UMStandard, UMAg |
| 4 | AiLogicAggression (0x73D260, 0x18) | 0x4BA710 / 0x4BA830(flag) | +8 target, +0xc revenge, +0x10 spell, +0x14 chosen | AggrMode 0 (normal) / 1 (revenge); UMAggression, UMRevenge |
| 5 | AiLogicCorpseWatcher (0x73D294, 8) | 0x4BA920 | none | AggrMode 0..3; UMCorpseWatcher |
| 7 | AiLogicFear (0x73FF8C, 0x2c) | 0x5D2850(flag) | +8 radius, +0xc jitter, +0x10 rememberedSpot, +0x1c kiteSpot, +0x24 kiteValid, +0x28 playerFear | AggrMode 0..3 (3 = flag 1); UMFear(flag) |
| 6 | AiLogicPlayer (0x73F0F8, 0x24) | 0x5D3760 | +8 keptTarget, +0xc leader, +0x10 moveTarget, +0x18 moveIssued, +0x1c state, +0x20 run | Model 4; UMPlayer; party heroes |
| 8 | AiLogicTamed (0x73F0C4, 0x24) | 0x5D3760 | same layout | `OnProvoked` stage 3 (taming) |

Base classes: 0x73D15C "stationary" (kinds 0, 1, 2, 10: Evaluate 50, Update = `CastSupportSpell`, Begin 0x5D0AF0, IsStationary true) and 0x73D22C
"reactive" (3..8: Begin 0x5D0A50 = target released + order kind 0, IsStationary false). `Brain::AddLogic` 0x5D5F60 replaces a logic of the same kind,
and any stationary one by a new stationary one: a unit has at most one post.

### 3.6 The `.mob` logic slot (`MobLogicSlot`, 0x4c bytes, 5 per unit; chunk 0xBBBC read by `UnitLogic_OnChunk` 0x46CBC0, copied by `Brain::SetMobLogics` 0x5D6D20) [V]

| off | chunk | field |
|---|---|---|
| +0 | 0xBBBC0003 dword | Model: 0 point (no post), 1 guard, 2 patrol, 3 sentry, 4 player, 5 guard at the alarm slot position |
| +4 | 0xBBBD0000 records | points (0x20: pos + a CArray of 0x14-byte look-at records `{pos, +0xc wait, +0x10 short, +0x12 byte}`) |
| +0x18 | 0005 vec3 | guard / sentry point |
| +0x24 | 0002 byte | cyclic (0 = a patrol walks the points back in reverse) |
| +0x28 | 000D byte | bit 3 AlwaysActive |
| +0x2c | 0004 float | guard radius |
| +0x30 | 000A float | unused by the brain |
| +0x34 | 0006 byte | NumAlarm (alarm slot 0..4 raised by this unit; 0 = none) |
| +0x38 | 0007 byte | Use (this slot can be switched to by an alarm) |
| +0x3c | 000C float | Help (CallForHelp radius) |
| +0x40 | 000B byte | AlarmCondition: 0 enemy seen, 1 attacked, 2 enemy seen or any reactive motivation begins |
| +0x44 | | Wait (ticks before the alarm fires, min 1) [?] chunk id not in the switch read |
| +0x48 | 000E byte (0001 / 0008 / 0009 legacy bytes clamp it to 0 / 1 / 2) | AgressionMode: 0 standard, 1 revenge, 2 fearful, 3 player-fear |

`Brain::BuildFromLogicSlot` (slot, index): Model 1 -> `AiLogicGuard(point, radius, -1)`; 2 -> `AiLogicPatrol` with every point snapped to a tile centre and
its look-at records turned into actions `{atan2(record - point), record+0x10, record+0xc, record+0x12}` (non-cyclic: the points appended again in reverse);
3 -> `AiLogicSentry(point)`; 4 -> `AiLogicPlayer`; 5 -> `AiLogicGuard` at the alarm slot position `g_0x7AFFF4[index]` (`CWorldServer +0x94 + 0x10 index`).
Models 0 and 4 get **no reaction logics**; otherwise AggrMode 0 -> Aggression(0) + Suspection + CorpseWatcher + Fear(0); 1 -> Aggression(revenge) +
Suspection + CorpseWatcher + Fear(0); 2 -> CorpseWatcher + Fear(0); 3 -> CorpseWatcher + Fear(playerFear). Then alarmSlot / alwaysActive / wait /
callForHelpRadius / alarmCondition from the slot, `AiSpellInfos_Build`, and `CWorldServer::ActivateObject` when AlwaysActive.

## 4. Perception

### 4.1 Vision (`StimulusMap::SeeUnits` 0x5CA5A0, every tick from `UpdateVision` 0x5CAA00) [V]

For the observer O (CUnit, server S) and every neighbour T (server TS), unless T is already in the seen list (or a seen corpse):
* AI observers (`O.player == 0`, witness flag 0x2000000 off) only test **enemies** (`IsPlayerInMask(O, T)`) and units that belong to a player; a player's
  unit tests everyone.
* eye = O position + terrain height + `CUnit +0x14c x 1.8 + +0x158 + +0x130` (model height), direction = -forward axis of the quaternion.
* `d` = **2D** distance eye -> T, `R = TS.visibilityMult(+0x900) x S.sightMult(+0x904) x TS.visibility(+0x2c4) x S.sight(+0x294)`.
* **seen** when `d <= R` and `dot(T - eye, eyeDir) >= viewConeCos x d` (3D dot; heroes have -2 = always) and `CAIMap::TraceVisibility(eye, T) x R > d`
  (the trace returns the fraction of the ray not blocked by terrain / object volumes, `docs/game-exe/pathfinding.md`; so partial cover shortens the range);
* else **peripheral** when `dot >= 0` (T in the front half plane) and `d < S.peripheralRange(+0x228)` (no occlusion test);
* else **sense life** when T is alive and `d < TS.lifeSignature(+0x2d4) x S.senseLife(+0x2a4)` (no cone, no occlusion);
* a dead T that passes goes to the corpse lists (`+0x5c`; `+0x78` too when T's side is a friend of O's side); a living T is appended to the seen list.
  Nothing else happens on sighting: the Brain reads the seen list.

**Losing sight** (`UpdateVision`, before `SeeUnits`): a seen living unit T is dropped only when **(T is not an enemy of O, or O belongs to a player)** and
not (T fights, `TS +0x174`, with its brain target == O) and `d >= 1.05 x R` (1.0 for player observers) and `d >= 1.05 x senseLife range` and O is not T's
current Aggression target (`Brain::GetCurrentTarget(TS brain)`): "lost from sight unit", and when T is an enemy the noticed position becomes T's position with
interest 1000, decay -2 (unless already >= 1000). Read literally: **an AI unit never forgets a seen enemy by distance**; the seen list of a monster keeps every
enemy it ever saw until the enemy dies (`Forget` / the corpse lists), the memory is cleared (`ResetTarget`, death) or the unit is removed. Only player units and
non-enemies are dropped by range [V]. (The old "lost from sight" notes assumed a symmetric rule; the distance test exists but is gated by the enemy test.)
A seen unit that dies moves to the corpse lists ("sees corpse of unit"); a friend's corpse sets the noticed position with interest 800 / decay -3 (unless >= 800).
Corpses in `SeeUnits`: an AI observer only *notices* corpses that are not enemies / not player-owned (the friends the CorpseWatcher cares about); a player's
unit notices every corpse.

The numbers of `docs/monster-ai.md` (sight 8.5 animals / 13 heroes, fov 50 / 75) are the units.db values seen live; the code adds nothing to them.

### 4.2 Neighbour scan (`StimulusMap::RefreshNearbyUnits` 0x5CA1D0) [V]

Every `(rand & 3) + 8` ticks (`AiMemory +0x20`): the neighbour list is rebuilt from the collision-map sectors (32 m) within
`max(S.sightMult x S.sight, S.senseLife) x 2 / 32` sectors of the eye cell, i.e. every unit within roughly twice the sight range, friend or foe, alive or dead,
except O itself. The scan also sets the unit flag 0x80000 (alarm to propagate), so `PropagateAlarm` re-runs at the same cadence. The neighbour list
feeds `SeeUnits`, `ComputeDetectionFactors`, `AiAcknowledge::Tick`, `PropagateAlarm`, `WitnessMurder`, the support-spell candidates and the
friendly-fire test of area spells.

### 4.3 Forced sight (`AiMemory::ForceSee` 0x5CBC80, from `Brain::OnAttackedBy`) [V]

An attacker that is not yet seen is appended to the seen list when its brain attacks / casts on this unit (order 3 / 4 with target == me) **or** when it is
beyond both the sight range and the sense-life range (an attacker that could not have been seen); a visible attacker is left to `SeeUnits`. A dead attacker goes
to the corpse lists. `CUnitServer::WitnessMurder` 0x5538D0 does the same for every friend of a murdered unit in the neighbour list of the victim / killer:
flag 0x2000000, `UpdateVision`, the killer appended to the seen list ("has seen murder and is forced to see unit"), `DeclareHostile(killer, witness)`.

### 4.4 Hearing: sounds are events (`StimulusMap::HearSound` 0x5C98C0) [V]

`HearSound(source S, loudness L, delay, pos)`: for every unit U within 50 m (`CollisionMap::FindUnitsNear`) for which the source is an enemy
(`IsPlayerInMask(U, S)`), whose interest < 900, and with `|pos - U| < U.hearingMult(+0x8fc) x U.hearing(+0x2ac) x L`: a heard-sound record
`{pos, delay, 0, text}` is appended to U's AiMemory (`HeardSounds_AppendLatest`: older unflagged records with delay >= the new one are dropped first, so a
unit keeps only the newest sound). **No wall or line-of-sight test.** Sources:

| event | loudness | delay | where |
|---|---|---|---|
| footstep | `S.footstepMult(+0x8f8) x S.noise(+0x2dc) x material step noise (+0x110 of the CAIMap material)` at the unit position | 0x1a = 26 ticks | `StimulusMap::EmitFootstep` 0x5C9770, every `footstepTickMask` ticks (section 3.2) |
| melee / ranged attack | `S.noise x 2` | 26 | `CUnitServer::FireAttack` 0x54F450 |
| spell cast (proto +0x10 != 1) | `S.noise x 2` | 26 | `CastOrderSpell` 0x54EE10 |
| loot / interact | `S.noise x 1.5` | 26 | `ExecuteInteraction` 0x54F000 |
| spell hit | 5.0 at the hit position, delay = ftol of a spell value | | `CSpellEffect::OnHit` 0x681BB0 |
| call for help | not a HearSound: a record `{pos, 26, 0}` written directly into every friend within the Help radius | 26 | `Brain::CallForHelp` 0x5D5990 |
| alarm | record `{?, 30, enemyMask}` | 30 | `CUnitServer::DeclareEnemies` 0x552D00 |

`AiMemory::TickHeardSounds` 0x5CC9D0 (every tick): each record's delay counts down; at expiry an unflagged record sets `noticedPos = record pos, interest =
900, decay = -3` (only when the interest is < 900; "Delayed sound: ..."), a flagged one ORs its mask into the unit's `alarmMask` (+0x244) ("hear delayed sound").
The reaction to a sound is therefore the **Suspection** motivation (priority = interest, up to 900) walking to the noticed position (section 6.5),
26 ticks (~1.4 s) after the sound.

### 4.5 Smell, tracking and the stimulus grid: dead code [V]

`StimulusMap` (`CAIMap +0x26c8` = `CWorldServer +0x2838` = `g_0x7B2798`, 0x1003c bytes) keeps 8 m cells of 16 per-side lists of 0x30-byte *stimuli*
with an expiry ring of 1024 ticks (`ExpireBucket` runs every world tick, `Read` / `Write` save them), and `StimulusMap::PerceiveStimuli` 0x5CA060 /
`PerceiveFootsteps` 0x5C9CD0 would let a unit "see" (Senses Tracking) or "smell" (Senses Smell) footsteps. **Nothing calls `StimulusMap::Add`,
`PerceiveStimuli` or `PerceiveFootsteps`** (no caller in the call graph, no data reference to their addresses): the shipped exe never creates a stimulus;
`Senses Smell` is only read by the detection meter. `normal.dat` is loaded into `g_0x7C43C4` and not read by this code.

### 4.6 The detection meter of the party panel (`AiMemory::ComputeDetectionFactors` 0x5CB850) [V]

For player units only, every tick, into `CUnit +0x4c8 float[5]` (replicated with `MarkNetDirty(0xc)`): over every *neighbour* N that is alive and an enemy,
with `d` the 3D distance and the minimum kept, each clamped to 3.0 (3 = undetectable, < 1 = detected):
`[0] sight = d / (TraceVisibility(my eye, N eye) x N.sightMult x me.visibilityMult x N.sight x me.visibility)`,
`[4] senseLife = d / (N.senseLife x me.lifeSignature)`, `[1] smell = d / (N.smell x me.scent)`, `[2] hearing = d / (N.hearingMult x me.footstepMult x N.hearing x me.noise)`;
`[3]` stays 3. The meter is a *prediction* with the same formulas as the monsters' tests (minus the view cone), not a readout of who actually sees the hero.

## 5. Relations, threat and alarms

### 5.1 Masks [V]

* `PlayerRelations` (0x100 bytes: `friends[32]` rows then `enemies[32]` forced rows): `Set(a, b, mode)` 0x4554E0 (mode 0 friend, 1 neutral, 2 enemy; script
  `SetDiplomacy`), `GetRelation` 0x455630 (0 friend / 1 neutral / 2 enemy), `GetForcedMask(a)` 0x455660 = the enemy row = the **initial `alarmMask`** of every
  unit of side a (`CWorldServer::CreateObjectFromDesc` 0x5927C0, script `SetPlayer`). Instances: `g_0x7C2BC4` (filled by `MobFile_Load` from the .mob diplomacy; the AI
  code's "friend" test) and `CWorldServer +0x12c64` (the combat code's; `Reset` sets `friends[i] = 1 << i`, saved with the world).
* `CUnit +0x244 alarmMask`: enemy sides. Grows by `PlayerRelations::DeclareHostile` 0x455680 (attacker, victim): unless the attacker's side is a friend
  of the victim's side: `victim.SetAlarmMask(bit)` and the same bit on every unit of the victim's player; called from `TakeHit`, `OnKilledBy`, `OnProvoked`,
  `OnAttackedBy`, `WitnessMurder`, `ExecuteInteraction`, `CWorldServer::CastSpell` (hostile spell classes). `CUnit::SetAlarmMask` 0x52E940 also sets the
  unit flag 0x80000 and replicates (0xb).
* `CUnit +0x248 attackableMask`: default 0xffffffff; `OnProvoked` stage 1 (taming) clears the tamer's side so the unit stops fighting back.
* `IsPlayerInMask(me, other)` = both masks have `other.side`: used by every perception and targeting filter.

### 5.2 Call for help (`Brain::CallForHelp` 0x5D5990) [V]

For every unit within `brain +0x38` (the .mob Help radius, 7 by default) whose side is a **friend** of the caller's side: a heard-sound record `{pos, 26, 0,
"Unit %i call for help unit %i from %fm distance"}`. 26 ticks later the friend's interest becomes 900 at `pos` and its Suspection walks there (and, if it then
sees the enemy, its Aggression takes over). Callers: `Brain::OnAttackedBy` (every hit / hostile spell, unless the attacker is a friendly unit),
`AiLogicAggression::Update` (**every tick** while it has a target, with the target's position), `AiLogicFear::Update` (with the nearest enemy's position).
For party members the equivalent is `Brain::CollectCallers` (section 6.9): units of the same player within `PlayerCallForHelp` 16 m that attack someone.

### 5.3 Alarm propagation (`CUnitServer::PropagateAlarm` 0x552E00, `DeclareEnemies` 0x552D00) [V]

When the unit's flag 0x80000 is set (by `SetAlarmMask`, the neighbour scan, a death, a hostile spell): for every neighbour whose side is a friend (world
`+0x12c64` row) within the Help radius: the enemy bits one side has in `alarmMask` and the other lacks in its `+0x97c propagatedMask` are sent both ways with
`DeclareEnemies(mask)`: `+0x97c |= mask`, "Unit %i claims that player %i are enemies", and a **flagged heard-sound record** with delay 30 that ORs the mask into
the receiver's `alarmMask` 30 ticks later. So hostility spreads through chains of friends at ~1.7 s per hop, limited by the Help radius.

### 5.4 The .mob alarm slots (`CWorldServer::ActivateLogicSlot` 0x593FC0; script `InvokeAlarm`, `IsAlarm`, `AlarmTime`, `AlarmPosX/Y`) [V]

`brain +0x3c alarmCondition` decides when a unit *raises* its alarm flag: 0 = an enemy is seen; 1 = it is attacked (`Brain::OnAttackedBy`); 2 = an enemy is seen or
any of Suspection / Aggression / CorpseWatcher / Fear becomes current. With the flag set, `brain +0x40 wait` counts down (the .mob Wait, min 1) and then
`ActivateLogicSlot(alarmSlot +0x35, unit position, force 0)` runs, with the "ALARM ALARM ALARM" voice level 8. The world marks slot n (`+0x8c + 0x10 n`:
active byte, tick, x, y) and **every AI unit** (`player == 0`) calls `Brain::SelectLogicSlot(n)` 0x5D5940: units whose slot n has the Use flag (or n == 0)
rebuild their motivations from that slot (`BuildFromLogicSlot`: new post, new AggrMode) and voice "to Alarm!" (level 7); units with AlwaysActive are activated.
A slot already active is not re-triggered unless forced (the script). Slot 0 forced clears all five.

### 5.5 Voice reactions (`AiAcknowledge`, `CUnitServer +0xb10`, 0x5AEA40-0x5AEA80) [V]

Not an alertness level: a 4-dword `{heard, pending, current, timer}` that turns motivation changes into voice lines. `Request(level)` (0x5AEA60) raises
`pending`; `Tick` (0x5AEA80, every tick before the brain): a pending level above `heard` becomes `current` for 20 ticks: log "Unit N acknowledges with ..." and
`CWorldServer::BroadcastMsg0B(unit, sound)` for 1 / 2 (idle sounds 0x31 / 0x32), 5 ("to Suspection", 0x2f), 6 ("to Aggression", 0x2e), 9 ("poluchi gadina",
0x30: `OnKilledBy`, a hero killed by an AI unit); 3 / 4 "back from Suspection / Aggression", 7 "to Alarm!", 8 "ALARM ALARM ALARM" only log. While current, the
level is written into `heard` of every friend in the neighbour list, so a group voices a reaction once. Levels: 1 / 2 random idle (1/1001 per tick in the
stationary / Aggression Updates), 3 Suspection End, 4 Aggression End, 5 Suspection Begin, 6 Aggression Begin, 7 alarm received, 8 alarm raised, 9 kill.

## 6. The motivations as state machines

Common: the priority is re-evaluated every tick; a change runs `End` (old) then `Begin` (new). Stationary logics share `Begin` 0x5D0AF0 (target released, order
kind 0, idle speed factor random 0.8..1.0, `fighting +0x174 = 0`) and `Update` = `Brain::CastSupportSpell` + the 1/1001 idle voice. "Idle" below = brain order
kind 0. All position orders go through `Brain::BuildIdleActions` 0x5D5B40 + `QueueOrderMoveTo(pos, 0, 0, 1e6)` with brain kind 2 (section 8.2).

### 6.1 Guard (kind 0, `AiLogicGuard::Update` 0x5D0BE0) [V]
Evaluate 50. Update when idle: waypoints = the guard point, `nearRange = r + 35`, `farRange = r + 80`; `Brain::FindRandomReachableSpot(point, r)` (after 100
failures: around the unit itself); look direction = `(spot - point) x |spot - point| / r / 2`; `BuildIdleActions(spot, dir, stayTime, random anim 1/11)`; walk there.
So a guard wanders inside its radius, stopping `stayTime x 15` ticks (randomised 25..100 %) per spot and turning 1..4 times.

### 6.2 Patrol (kind 1, 0x5D1700) [V]
Evaluate 50 (-1000 and "Unit %i has no patrol points, switching to guard" + the unit's own position added when empty; the message is a log, nothing switches).
Update when idle: next point (wraps); its actions (look-at turns with waits) copied to the brain, or `BuildIdleActions(point, 0, -1 -> 40 ticks)` when it has none;
walk there; waypoints = the point, near / far 35 / 80. Script `UMPatrolAddPointLook(x, y, delay)` appends an action `{x, y, delay, 0}` to the last point;
`PatrolActions_Write` does **not** save the angle (+0) of an action [V].

### 6.3 Sentry (kind 2, 0x5D1B70) [V]
Evaluate 50. Update when idle: `BuildIdleActions(point, 0, SentryStayTime 10; GuardStayTime with a 1/11 chance that also enables a random animation)`, walk to
the exact point, waypoints = point. A sentry therefore only turns in place and plays idle animations.

### 6.4 Follow (kind 10, `AiLogicFollow::Update` 0x5D1E10; script UMFollow) [V]
Evaluate 50. Update: whenever idle, brain kind 6 on the target; `Brain::TickFollow` does the walking (section 8.3). A permanent follower.

### 6.5 Suspection (kind 3) [V]
* `Evaluate` 0x5D1E90: when the attacker `+0x30` is a non-unit or a friendly unit: `SetNoticed(its pos, 1000, -2)` (friendly fire / traps make it look);
  if the noticed pos (`AiMemory +0x10`) still equals the logic's point: `stay = min(stay, interest)` else `point = noticedPos, stay = interest`; **returns
  stay** = the interest (so Suspection beats the 50 of a post only while interest > 50 (> 150 against the current post), and loses to Aggression / Fear).
* `Begin` 0x5D21A0: `fighting = 1`; walks to the point (face it, no wait); voice 5 "to Suspection".
* `Update` 0x5D1F80: while the current move goes to the point: once within 5 m, `stay -= 5` per tick; when idle: `stay -= 50` per tick while an enemy is seen;
  `FindRandomReachableSpot(point, 5 m)` (failure: `stay -= 5`); walk there (40-tick idle turns); `stay = interest`.
* `End` 0x5D2310: `fighting = 0`, voice 3 "back from Suspection".
So a unit that heard something walks to the spot, looks around in a 5 m circle and gives up as the interest decays (-3 per tick from 900 ~ 300 ticks ~ 17 s).

### 6.6 Aggression (kind 4) [V]
* `Evaluate` 0x5D2390: normal mode: needs `enemySeen`; candidates = `AiMemory::CollectHostiles` (seen, enemy, alive, not ignored); `Brain::ChooseTarget(attack +
  support tables, candidates, own pos, filter 0, direct 0)` -> `{spell, target}`; target kept in +8; **10000** when found else -1000.
  Revenge mode (+0xc, .mob AggrMode 1 / UMRevenge): keeps its target; without one takes the brain's attacker `+0x30` (not a friend, not itself, not ignored); the
  target must be alive, a unit and `AiMemory::IsFriendOrSeen`; spell = weapon (-1); 10000 / -1000. A revenge unit never attacks first.
* `Begin` 0x5D26D0: `fighting = 1`, target released, order kind 0, voice 6 "to Aggression".
* `Update` 0x5D2580: 1/1001 voice 2; `Brain::CallForHelp(target pos)` **every tick**; unless the brain already orders this `{spell, target}`:
  weapon -> `Brain::OrderAttack(target)` (kind 3, `QueueOrderAttack(target, 6, 0, run)`), spell -> kind 4, `+0x28 = slot`, `QueueCastOn(slot, target, 0)`.
* `End` 0x5D2710: a dead / gone target zeroes interest and decay; target released; `fighting = 0`; voice 4 "back from Aggression".
There is **no leash and no FollowLimit**: a monster keeps every seen enemy in its memory (section 4.1) and chases as long as the path finder reaches it
(`TryAttackTarget`: path length `3 d + 10` = `CUnit::FollowDistance`, so any straight path fits). It stops when the target dies or becomes unreachable (then the
target is ignored for 20 ticks, Aggression drops to -1000, the post logic resumes, and 20 ticks later it tries again). `docs/monster-ai.md`'s "chases at most
FollowLimit 15; leaving it behind works" is wrong (no reader of that value); what ends a chase in practice is an unreachable path (doors, water, cliffs) or the
scripted `ResetTarget`.

### 6.7 CorpseWatcher (kind 5, `Evaluate` 0x5D27D0) [V]
Never current (-1000, no Update). Each tick it pops the first corpse of a friend side from `AiMemory +0x78` (filled by `SeeUnits` / `UpdateVision` this tick) and
`SetNoticed(corpse pos, 800, -3)`: the Suspection logic then walks to the body. The list is emptied every tick (`Brain::Tick` step 9).

### 6.8 Fear (kind 7) [V]
* `Evaluate` 0x5D2960: jitter `+0xc = (2 j + 3) / 3`; an attacker `+0x30` -> `SetNoticed(its pos, 1000, -2)` and `radius = max(radius, 1.5 x distance)`.
  **Kiting** (ranged units: `CUnitServer::IsRangedAttacker` (no weapon: natural range > 1; weapon kind 5 / 6), animSpeedClass < 6, no spell in slot 0,
  `UnitStats_EffectLevel == 0`, `+0x238 > 0.6`): the nearest seen enemy within `RangedAttackDist` 7 (`RangedRunDist` 4 while Aggression is current):
  already moving -> **15000**; else a retreat point 5 m further away from the enemy (+-3 m jitter) with a path <= 2 x its distance and from which the enemy can be
  attacked (`CanReachOrderTargetFrom`) and within 1 m of the path end -> stored in `+0x1c` -> **15000**. Else `enemySeen` -> **3000**. Else standing in a spell
  area (`CAIMap::HasObjectType3AtPos`, moving or a non-idle prototype) -> remembers the spot -> **350**; the remembered spot still in an area -> 350. `playerFear`:
  the nearest *player-owned seen* unit within the radius -> `SetNoticed(its pos, 1000, -2)`. Then: interest > 50 and (Fear is current or the unit is within the
  radius of the noticed pos) -> **interest / 6**; else `radius = (30 r + 10) / 31` (decays to 10) and -1000.
* `Begin` 0x5D3650: `fighting = 1`, target released, kind 0. `End` 0x5D3680: `fighting = 0`.
* `Update` 0x5D30A0: origin = the nearest seen enemy when `enemySeen` (+ `CallForHelp` there), else the own position when inside a spell area, else the remembered
  spot when that is in an area, else the noticed position (interest zeroed unless playerFear with interest >= 501). Within `radius` of the origin and not moving:
  target = the kiting spot, or a point 5 m away from the origin with jitter +-`+0xc` (doubled every flee, capped at 2.5 r); `CAIMap::FindPath(3 d + 10)` ok -> kind 1
  `QueueOrderMoveTo`. When idle: `ChooseTarget(empty candidates, filter 1, direct 1)` + `ApplyCastOrder` (support spells, cast from where it stands), else turn to
  face the origin.
So Fear is three behaviours: the ranged kite (priority 15000 beats Aggression's 10000: an archer with an enemy inside 7 m steps back, then Aggression shoots
again), the flee from a seen enemy at 3000 (used by AggrMode 2 / 3 units that have no Aggression; with Aggression present 10000 wins), and leaving spell areas.

### 6.9 Player (kind 6) and Tamed (kind 8) [V]
Evaluate 10 (Tamed 100000000: a tamed unit keeps this logic over any reaction). The party-member logic; `AiLogicPlayer::Update` 0x5D4A90:
* state 0: with a leader `+0xc` and idle (or already following): brain kind 6 on the leader (section 8.3) unless the player's mode (`CPlayerServer +0x5120`) is 2
  (a party fight); nothing more while the leader's own state is 1 (it is executing a move); otherwise **`Regroup`** 0x5D4170 (below).
  `NotifyPlayer(0x20)` every tick while a leader exists and the battle button `CUnit +0x438 != 1`.
* state 1 (an own move order runs): back to 0 when the brain order is done.
* state 2 (a party move parked by `MoveTo` 0x5D3AE0 because a follower was farther than `FollowMaxDist x 2 + 1 = 9 m`): the move is queued once the followers are
  within `FollowMaxDist x 1.4 + 1 = 6.6 m` (`Brain::AreFollowersClose(0)`) or the mode is 2; while walking it is cancelled with `NotifyPlayer(0x21)` when they fall
  behind 9 m again; arrival -> state 0. State 2 also runs `Regroup`.
* **`Regroup`** (only when `+0x438 battleButton == 1`): keeps attacking `keptTarget +8` while `Brain::TryAttackTarget(direct)` allows (kind 3, `QueueOrderAttack(target,
  6, 0, run)`); else candidates = `AiMemory::CollectAttackers` (seen enemies that are attacking, within `SwarmRadius` 10 of the move target in state 2, unlimited
  otherwise) minus those farther than `1.1 x max(their sight range, their sense-life)`, plus `Brain::CollectCallers` (targets of same-player units attacking within
  `PlayerCallForHelp` 16 m); `ChooseTarget(candidates, own pos, 0, 0)`; a target: unless mode 2, every follower of the party chain that is not attacking / casting is
  stopped (its state 0, brain order cleared), then kind 3 + `QueueOrderAttack(target, 6, 0, run)`.
* `MoveTo`, `MoveToNow` 0x5D39E0, `AttackTarget` 0x5D3C30, `CastOn`, `CastAt`, `Interact`, `SetLeader` 0x5D3DE0 (no leader cycles) are the entry points of the
  player messages (`CPlayerServer::OnMsg_*`, `WorldServer::On*Order`); AI-owned units may only be ordered to attack units passing `IsFriendOrSeen`.
Taming: `CUnitServer::OnProvoked` 0x551AE0 escalates `+0x908` per big hit from another player's unit: 1 = the tamer's side removed from `attackableMask` (stops
fighting back), 2 = the unit's side changed to the tamer's, 3 = `SetPlayer(tamer)` + an `AiLogicTamed` logic; `StopFollowing` / `OnAttackedBy` undo it.

### 6.10 Script commands (`docs/game-exe/gs-functions.md`, all verified there) [V]
`Guard(u, x, y, r)` = `Brain::SetupGuardLogics` (Guard + Suspection + Aggression(0) + CorpseWatcher + Fear(0)); `Sentry` likewise with a sentry; `UMClear` /
`Idle` = `ClearLogics`; `UMGuard[Ex]`, `UMSentry`, `UMPatrol*`, `UMFollow`, `UMSuspection`, `UMAggression`, `UMRevenge`, `UMCorpseWatcher`, `UMFear(flag)`,
`UMStandard`, `UMAg(preset)` add single logics through `Brain::AddLogic`; `UMPlayer` = a party-member brain; `ResetTarget` = `Brain::ResetLogic` (memory cleared,
the current .mob slot rebuilt); `MoveToPoint` / `MoveToObject` = `Brain::OrderMoveTo`; `RotateTo` = `Brain::OrderTurnTo`; `Follow` = `OrderFollow`; `Attack` =
`OrderAttack`; `Cast` = `OrderCastAt`; `SetDiplomacy` / `SetPlayer` / `SetEnemy` = the masks; `InvokeAlarm` = `ActivateLogicSlot(force)`.
Note the `UMFear` flag is the **player-fear** byte (`+0x28`: afraid of player-owned units it sees, .mob AggrMode 3), not "always afraid".

## 7. Target selection and spells

### 7.1 The spell tables (`AiSpellInfos_Build` 0x5CEED0, rebuilt by `BuildFromLogicSlot`; `AiSpellInfo` 0x28 = `{kind, slot, range, uses, radius, effect, 7 damage shorts}`) [V]
Attack table: non-heroes with a natural weapon (`+0x28c != 0x10`) get `{0 weapon, -1, GetAttackRange, GetDamage7}`; every spell slot 0..7 by
`Spell_GetAiCategory` 0x5CEDC0 (spell class = proto +0x18 >> 16): 1 direct attack (0..2, 0x28, 0x29), 2 area (3..5), 3 area needing a fighting caster (6..8), 4 buff
on others (0x19, 0x1e, 0x20) -> attack table; 5 (9..0xb, 0x1a, 0x1d), 6 (0xc..0x11, 0x1f, 0x21) buffs and 7 heal (0x18) -> support table; 8 = unusable.
**Heroes get only the weapon entry**: the AI never casts for a player's hero. `AiSpellInfo_GetLevel` rates entries (weapon max damage, x 1.3 for attack spells,
1 buffs, 10 heal); the table maximum sits at brain +0xcc [?] its reader was not found.

### 7.2 The score (`AiSpellInfo_Score` 0x5CF9A0; `sum` = the positive parts of `damage - target absorption (CUnitServer +0x8e4)`, `n` = candidates - 1) [V]
| kind | score |
|---|---|
| 0 weapon | `10 if self + sum` |
| 1 attack spell | `10 if self + sum x 1.3 + 3 + sum x min(uses, n)` |
| 2 area | sum over every candidate within `radius` of the target, then as kind 1 with `min(uses, n + 1)` |
| 3 area in combat | -1e30 unless the caster's state != 0; `(effect / 8) x` that sum, then as kind 2 |
| 4 / 5 / 6 buffs | -1e30 when the target already has the buff kind (`CMapObject::HasBuffKind`); else `10 if self + 1 + min(uses, n)` |
| 7 heal | -1e30 when target HP > max HP (buffed) - 2; HP x 3 < max: `3 x caster mana ratio (+0xa4c)` (+ the ratio when the caster has full HP); else `10 if self + 5 + 2 min(uses, n)` |
Then, when >= minScore: `- 25 x max(0, distance - range)` (x 10 and no range for kinds >= 5) and for kinds 0..3 `- 2 x (target HP / sum)` (20 when sum is 0):
**the AI prefers the target it kills in the fewest hits**, not the nearest. The old target keeps its score as `minScore`, so a switch needs a strictly better score.

### 7.3 `Brain::ChooseTarget` 0x5D0030 (`this = brain +0xa4`, brain, out `{spell, target}`, candidates, pos, filter, direct) [V]
1. the current order's `{spell, target}` (`Order::GetSpellAndTarget`) is kept as the baseline when `AiSpellInfos::IsTargetClear` 0x5CF7F0 accepts it (area spells:
   not when a fighting friend of the neighbour list stands within 7 m of the target or the target is out of radius; other kinds always); with `filter` and a one-entry
   attack table the baseline is returned at once;
2. `filter == 0`: every candidate x every attack entry; always: itself and every living, non-ignored **friend** of the neighbour list x every support entry (kind 6
   skipped under `filter`); scores above the baseline go to a list;
3. from the best down: `IsTargetClear`, under `filter` either `Brain::IsNearPath(target, 30 m)` (the post / patrol segment) or `CanReachOrderTargetFrom`, then
   `Brain::TryAttackTarget(direct)` 0x5CF3C0 (attack position = the unit (direct / self) or the end of `CAIMap::FindPath(unit, target, max 3 d + 10)` with the target
   removed from the collision map; `CanReachOrderTargetFrom(order, position)`); a candidate that fails and was the only entry for its target goes to the ignore pairs
   for 20 ticks; the first that passes wins; none -> the baseline.
`Brain::CastSupportSpell` 0x5CFE40 (the stationary `Update`s) is `ChooseTarget(empty, filter 1)`: heals / buffs on friends only. `AiLogicAggression` uses filter 0
with the seen enemies; `Regroup` filter 0 with the attackers; `Fear` filter 1 direct 1.

## 8. Outputs: what the AI asks the unit to do

### 8.1 Entry points (all on the owner `CUnitServer`; the combat page has their insides) [V]
| call | by | parameters |
|---|---|---|
| `QueueOrderMoveTo(pos, flag, run, maxLen)` 0x554740 | every position move (brain kind 1 / 2); `TickFollow` uses `maxLen = 3 d + 10`, 10 or 20 | flag 0, run 0, 1e6 |
| `QueueOrderTurnTo(dir[3], speed)` 0x5546B0 | `TickMoveOrder` (idle action turns), script `RotateTo` | queued order kind 2 |
| `QueueOrderKindB()` 0x555200 | `TickMoveOrder` when the action's `playAnim` byte is set | order 0xb (idle animation) |
| `QueueOrderAttack(target, 6, 0, run)` 0x554B30 | `Brain::OrderAttack` (Aggression, ApplyCastOrder), `Regroup`, `CastSupportSpell`; `run = -1` when `CanReachOrderTarget` (player units) else 0x1c; `AttackTarget` / `OrderAttackChecked` pass 2 | hit location 6 = any |
| `QueueCastOn(slot, target, 0)` 0x554F50 / `QueueCastAt(slot, pos, run)` 0x554D00 | Aggression, ApplyCastOrder, CastSupportSpell, the player casts | brain kind 4 unless `IsQuickItemId` |
| `QueueOrderUseOn / Interact / Interact2 / Talk` | `Brain::OrderInteract` (player messages) | brain kind 5 |
| `ClearQueuedOrder()` 0x555180 | brain kind 0, `TickFollow` when close | |
| `MoveToWithParty(goal, group, 0, 1e6)` 0x554830 | `AiLogicPlayer::MoveTo` when followers lag | reachability report only |
| `NotifyPlayer(0x20 / 0x21)` 0x552780 | `AiLogicPlayer::Update` | player messages |
| `CWorldServer::ActivateLogicSlot(slot, pos, 0)` 0x593FC0 | `Brain::Tick` alarm | |
| `AiAcknowledge::Request(level)` 0x5AEA60 | motivations | voice |
| `CUnit::SetAlarmMask(mask)` 0x52E940 | heard alarm records | |

### 8.2 Idle actions (`Brain::BuildIdleActions` 0x5D5B40, `Brain::TickMoveOrder` 0x5D7520) [V]
`BuildIdleActions(out, pos, dir, stay, randomAnim)`: `(rand & 3) + 1` actions `{angle, 0, duration, flag}`: each direction = `dir + a random unit vector`,
re-rolled up to 6 times until the point 2 units away in that direction is visible from `pos` (`TraceVisibility == 1`, eye z + 1.4); `duration = stay x 15` ticks
(40 when stay < 0) randomised 25..100 %; `randomAnim` (the 1/11 roll) applies only to non-fighting, non-human units (model name not "unhu") and then
makes a single action of (count x duration) with the flag set. The brain order kind 2 carries them: after the walk (`TickMoveOrder` returns while the unit's
order is 1 / 2), `actionTimer -10000`: next action -> `QueueOrderTurnTo(cos/sin angle, turnSpeed)` and -9999; then `playAnim` -> `QueueOrderKindB`, the timer =
`tick + duration`; wait; -10000 again; no action left -> order kind 0. A move that ended more than 1 m from the goal (blocked, `CUnitServer +0x18c`) drops the order.

### 8.3 Follow (`Brain::TickFollow` 0x5D6F90, brain kind 6; `AiLogicPlayer` leaders and `UMFollow`) [V]
Every 8 ticks. Target moving (its order kind 1): predicted position 5 ticks ahead (vt[52] `GetPosAt`); when the unit is beyond `FollowMinDist` 2 of it and of its own
goal: `QueueOrderMoveTo(predicted, 3 d + 10)` if the predicted point is more than `FollowMaxDist` 4 from the own goal; when near: the first tick 6..99 ahead at
which the target passes within `sqrt(1.3)` m, then a spot 1.365 m beside the target's position 40 ticks ahead (perpendicular, on the unit's side),
`CAIMap::FindPath(10)`: walk there (10) or to the predicted position (20). Target standing: walk to it (`3 d + 10`) when farther than `FollowMinDist` and it is
more than `FollowMaxDist` from the own goal; else `ClearQueuedOrder`. Target dead / gone: order kind 0. (The pathfinding page's 5.5 says the same.)

### 8.4 `Brain::FindRandomReachableSpot` 0x5D7F80 [V]
Backoff `+0x68` (decremented per call); a random point in the square (up to 10 re-rolls to be inside the circle); rejected when `CAIMap::GetMaxSlopeAt > 0.3` or a
friend-side unit (including itself) stands within `2.5 x CUnit +0x32c` of it; `CAIMap::FindPath(unit, spot, 3 d + 10)` must succeed with a cost <= 10000 d, else
`+0x68 = min(10, d / 3)` ticks.

## 9. Saving and replication [V]
`Brain::Write` 0x5D8860 / `Read` 0x5D8310: kind, pos, actions, timer, target / attacker ids, alarmCondition, near / far / speed factor, waypoints, the logic list
(kind + each logic's slot 12 / 11), `AiMemory::Write` (the four lists as object ids, noticed pos, interest, +0x20, ignore pairs, heard sounds, decay), alarm
flag / slot, the 5 mob slots (`UnitLogic::Write` 0x45D760), wait, active, both spell tables, max level, current slot (save versions 0x4a / 0x59 / 0x70 gates).
`StimulusMap::Write` saves the (always empty) stimulus grid with the world. Nothing of the brain is replicated to clients except what the unit orders cause
(`MoveRequest`, stance, `alarmMask` / side / player via net section 0xb, the detection factors via 0xc, the battle button via 0xd).

## 10. What is not known
* Readers of `brain +0x44 / +0x48` (near / far range) and of `+0x4c idleSpeedFactor` outside the brain, of `AiSpellInfos +0x28 maxLevel`, of `FollowLimit`,
  of `normal.dat` (`g_0x7C43C4`) and of `AiMemory +0x20` beyond the scan countdown [?].
* The exact activation rule for sleeping units other than damage / AlwaysActive / alarm (`TickActiveObjects` mode and who calls `ActivateObject`).
* `MobLogicSlot +0x30` (chunk 0xBBBC000A) and which chunk fills `+0x44 Wait` (the switch read has no case for it; `UnitLogic::Read` loads it from saves).
* `HeardSounds_AppendAlarm` writes an uninitialised stack position into the record (harmless: flagged records never use it).
* The player mode `CPlayerServer +0x5120` values 1 / 2 beyond "a hero is hurt / fighting" (`docs/monster-ai.md`), and the UMAg preset table of gs-functions.md
  (Fear(1) for preset 0) against the .mob AggrMode mapping (Fear(1) only for mode 3): both read as written, not re-verified here.

## 11. Corrections to earlier notes
1. `docs/monster-ai.md`: "a monster that is hit or sees an enemy calls its friends within CallForHelpRadius" -> the radius is the unit's own `.mob` Help value
   (`brain +0x38`), 7 only when the .mob left it at the default; the call happens on every hit (`OnAttackedBy`) and **every tick** while Aggression has a target.
2. `docs/monster-ai.md`: "FollowLimit 15 ... a monster chases at most 15 units; leaving it behind works" -> no reader of FollowLimit; there is no leash, and an AI
   unit never drops a seen enemy by distance (4.1, 6.6).
3. `docs/monster-ai.md`: "RangedRunDist / RangedAttackDist ... the logic of a monster that flees: keeps 4 away and shoots from 7" -> they are the kiting
   distances of `AiLogicFear::Evaluate` for ranged units: retreat when an enemy is inside 7 (4 while attacking); the shooting is Aggression.
4. `docs/monster-ai.md`: "A monster knows a unit once a factor drops below 1; the factors are ... kept at most 3, one per sense" -> `ComputeDetectionFactors` is the
   hero's detection meter (player units only, `CUnit +0x4c8`); monsters use the vision / sense-life / hearing tests of section 4 (same ranges, plus the view cone and
   occlusion for vision; hearing is event-driven with a 26-tick delay).
5. `docs/monster-ai.md`: "hearing: footsteps (StimulusMap::PerceiveFootsteps) and sounds ... a fight, a loot, a cast make noise with a radius of noise x 1.5" ->
   footsteps are `HearSound` events (loudness `footstepMult x noise x material`); attacks and casts are `x 2`, loot `x 1.5`; `PerceiveFootsteps` is dead code.
6. `docs/monster-ai.md`: "smell: a third range (+0x2b4), used by animals" -> dead (only the detection meter reads smell).
7. `docs/monster-ai.md`: "a target that cannot be reached or attacked is ignored for 20 ticks" -> only when it was the sole scored option for that target.
8. `docs/monster-ai.md` / `hierarchy.md`: "+0x18 attention (0..1000, decays by +0x1c), +0x1c notice kind" -> +0x18 is the interest, +0x1c the per-tick decay
   (-1 / -2 / -3) which also identifies the kind; "+0x24 ObjRefList forced-seen" -> the neighbour list; "+0x14c list" -> the friendly-corpse list head.
9. `hierarchy.md`: slots "6 Leave(brain), 7 (ret 4)" -> 6 = Begin (new current), 7 = End (old current); "4 Enter" runs every tick for every logic (Evaluate).
10. `hierarchy.md`: "0x4BA710 Suspection, 0x4BA610 Aggression" -> 0x4BA710 constructs Aggression (vtable 0x73D260), 0x4BA610 Suspection (0x73D1F8).
11. `hierarchy.md`: "+0xe0 / +0xe4 current point, +0xe8 stay time" -> these are `AiMemory` noticedPos / interest (brain +0xd0 + 0x10 / +0x18).
12. `hierarchy.md`: "CAIMap (class name from the StimulusMap::Load string; one global at 0x7B2798)" -> the object at 0x7B2798 is the `StimulusMap` (CAIMap +0x26c8).
13. `functions.tsv` (before this batch): `UnitGrid_Insert` 0x5C9770 is `StimulusMap::EmitFootstep`; `SelectCombatTarget` 0x5CFE40 casts support spells only;
    `WorldServer::OrderMoveTo / OrderOnTarget` are `AiLogicPlayer` methods; `Alertness` is the voice reaction object; `AggroTable_Add` is `PlayerRelations::DeclareHostile`
    and `CWorldServer +0x12c64` a `PlayerRelations`; `UnitAI_OnAttackedBy` is `Brain::OnAttackedBy`; `Brain::IsTargetHostile` tests friendship (no caller).
14. `docs/game-exe/pathfinding.md` 5.5: "`AiLogicPlayer::Update` ... `IsNearLeader`" -> the function is `Brain::AreFollowersClose` (true when every follower
    of this unit is near); the distances 9 / 6.6 m are right.
15. `docs/game-exe/gs-functions.md` UMFear: "flag != 0 is the always afraid variant" -> player-fear (6.10).
16. `full/structs.tsv`: `CUnitServer +0x980 brain` is embedded (0x190 bytes), not a pointer; `+0x174` is used as a byte "fighting" flag by the AI; `CUnit +0x24c`
    is the owning player, `+0x248` the attackable-sides mask.

## 12. Function index
Perception: StimulusMap::Load 0x5C9310, ExpireBucket 0x5C9530, Add 0x5C9630 (dead), EmitFootstep 0x5C9770, RemoveByOwner 0x5C97B0, HearSound 0x5C98C0,
PerceiveFootsteps 0x5C9CD0 (dead), PerceiveStimuli 0x5CA060 (dead), RefreshNearbyUnits 0x5CA1D0, SetEyeAndDirection 0x5CA410, SetPos 0x5CA520, SeeUnits 0x5CA5A0,
UpdateVision 0x5CAA00, Read / Write 0x5CAE30 / 0x5CAFA0, SightMap_Build 0x5CD210 (debug effect). AiMemory: Clear 0x5CB0F0, CollectHostiles 0x5CB260,
CollectAttackers 0x5CB480, FindNearestSeenEnemy 0x5CB670, Tick 0x5CB700, IsFriendOrSeen 0x5CB7F0, ComputeDetectionFactors 0x5CB850, SetNoticed 0x5CBBE0,
ForceSee 0x5CBC80, SeeCorpse 0x5CBE90, Forget 0x5CC020, Read / Write 0x5CC1E0 / 0x5CC350, TickHeardSounds 0x5CC9D0, HeardSounds_AppendLatest 0x5CCD40,
HeardSounds_AppendAlarm 0x5CCEA0, IgnorePairs_Set 0x5CC620. AiInfo: StaticInit 0x5CDFB0, Load 0x5CE0F0. Spells: AiSpellInfo_Read / Write 0x5CE860 / 0x5CE920,
GetLevel 0x5CE9B0, Spell_GetAiCategory 0x5CEDC0, AiSpellInfos_Build 0x5CEED0, Brain::ApplyCastOrder 0x5CF170, Order::GetSpellAndTarget 0x5CF220,
Brain::CanReachSpellTarget 0x5CF2F0, Brain::TryAttackTarget 0x5CF3C0, AiSpellInfos::IsTargetClear 0x5CF7F0, AiSpellInfo_Score 0x5CF9A0, Brain::CastSupportSpell 0x5CFE40,
Brain::ChooseTarget 0x5D0030. Logics: base Begin 0x5D0A50 / Evaluate 0x5D0A80 / Update 0x5D0A90 / stationary Begin 0x5D0AF0; Guard 0x4BA1C0 ctor, 0x5D0B70 init,
0x5D0BE0 Update, 0x5D0EB0 / 0x5D0F00 Read / Write; Patrol 0x5D0F60 ctor, 0x5D12B0 AddPoint, 0x5D16B0 Evaluate, 0x5D1700 Update, 0x5D1930 / 0x5D1A40; Sentry 0x4BA360,
0x5D1B70 Update; Follow 0x4BA450, 0x5D1E10; Suspection 0x4BA610, 0x5D1E90 Evaluate, 0x5D1F80 Update, 0x5D21A0 Begin, 0x5D2310 End; Aggression 0x4BA710 / 0x4BA830,
0x5D2390 Evaluate, 0x5D2580 Update, 0x5D26D0 Begin, 0x5D2710 End; CorpseWatcher 0x4BA920, 0x5D27D0 Evaluate; Fear 0x5D2850 ctor, 0x5D2960 Evaluate, 0x5D30A0 Update,
0x5D3650 Begin, 0x5D3680 End, 0x5D3690 / 0x5D36F0; Player 0x5D3760 init, 0x5D39E0 MoveToNow, 0x5D3AE0 MoveTo, 0x5D3C30 AttackTarget, 0x5D3D20 CastOn, 0x5D3D90 CastAt,
0x5D3DE0 SetLeader, 0x5D4170 Regroup, 0x5D4A90 Update, 0x5D4CD0 / 0x5D4DB0; Brain::CollectCallers 0x5D3F50, CollectFollowChain 0x5D4600, AreFollowersClose 0x5D4860,
CanWalkBetween 0x5D37E0. Brain: ClearLogics 0x5D4DF0, Reset 0x5D4E80, Init 0x5D4F70, SetOwner 0x5D5080, IsNearPath 0x5D50E0, Tick 0x5D53B0, OnObjectRemoved 0x5D58D0,
SelectLogicSlot 0x5D5940, CallForHelp 0x5D5990, BuildIdleActions 0x5D5B40, FindLogic 0x5D5DD0, GetPatrolLogic 0x5D5E30, GetPlayerLogic 0x5D5E60, GetFollowTarget 0x5D5E90,
GetPlayerLogicTarget 0x5D5EE0, GetCurrentTarget 0x5D5F20, AddLogic 0x5D5F60, RemoveLogicKind 0x5D6140, SetupGuardLogics 0x5D6230, AddLogicGuard 0x5D6300,
AddLogicSuspection 0x5D63B0, AddLogicAggression 0x5D63F0, AddLogicCorpseWatcher 0x5D6440, AddLogicFear 0x5D6480, SetupSentryLogics 0x5D64E0, AddLogicSentry 0x5D65B0,
BuildFromLogicSlot 0x5D6630, SetMobLogics 0x5D6D20, TickFollow 0x5D6F90, TickMoveOrder 0x5D7520, CheckOrderTarget 0x5D76A0, ClearOrder 0x5D7790, OrderMoveTo 0x5D7AE0,
OrderTurnTo 0x5D7B30, OrderAttack 0x5D7B50, ResetLogic 0x5D7C30, OrderAttackChecked 0x5D7C60, OrderInteract 0x5D7CE0, OrderCastAt 0x5D7DA0, OrderFollow 0x5D7E40,
OnAttackedBy 0x5D7E90, IsAttackerFriendly 0x5D7F40, FindRandomReachableSpot 0x5D7F80, Read / Write 0x5D8310 / 0x5D8860. Outside: CUnitServer::Update 0x552300,
UpdatePerceptionFactors 0x5536B0, PropagateAlarm 0x552E00, DeclareEnemies 0x552D00, WitnessMurder 0x5538D0, OnProvoked 0x551AE0, AiAcknowledge 0x5AEA40 / 0x5AEA60 /
0x5AEA80, PlayerRelations 0x4554B0-0x455680, CWorldServer::ActivateLogicSlot 0x593FC0, TickActiveObjects 0x58D670, ActivateObject 0x58D5D0, UnitLogic_OnChunk 0x46CBC0,
UnitStats_Recompute2 0x524560, CUnit::IsPlayerInMask 0x530150, SetAlarmMask 0x52E940, SetAlarmMaskForce 0x52E970, SetMask248 0x530180.
