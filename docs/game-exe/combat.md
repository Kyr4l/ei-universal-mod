# Unit combat and the server unit tick in game.exe (CUnitServer, UnitStats, CUnit, CUnitClientSpecific)

Written 2026-10-27 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x520000-0x529000 stats and
damage, 0x52E000-0x53A000 CUnit / CUnitClientSpecific, 0x545000-0x55E000 CUnitServer, plus the callers in 0x664000-0x674000 (player
messages), 0x5136D0 (stats from the prototype), 0x67BE20 (arrow effect)). Movement itself is `docs/game-exe/pathfinding.md`; the unit AI
(Brain, AiLogic*, 0x5C8000-0x5E2000) is documented separately: here only its entry points into CUnitServer are named. Batch files:
`_cpr/claude-re/full/batch-combat.tsv` (212 function rows) and `batch-combat-structs.tsv` (217 field rows).

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions (strong), **[?]** guess / not resolved.
Addresses are VAs; offsets are bytes. "tick" = the server world tick `CWorldServer +0x64` (55 ms at normal speed, 18/s, see pathfinding.md).

## How to re-implement it (short version)

1. **One tick per unit** (`CUnitServer::Update` 0x552300): regenerate every 15 ticks, refresh the spatial grid, tick the passive item spells,
   the AI (threat factors, alertness, `Brain::Tick`), the alarm propagation; then, if the unit is in a "thinking" state (0 walking, 1 turning,
   2 standing, 6 idle) and not frozen: `Think` (dispatch the queued order, stance changes, combat order step), `TickState` (the state's own
   step: movement, action timing, animation end) and `TickAnimation` (advance the animation queue and replicate the clip). Dead units only sink.
2. **Orders are a two-slot queue**: a caller writes the *queued* order (`+0x1B4`, 0x24 bytes, kinds 0..11) and `DispatchQueuedOrder` 0x54A900
   moves it into the *current* one (`+0x190`) and starts the matching state. A combat order (3 attack, 4/5 cast, 6 interact) runs every Think
   through `TickCombatOrder` -> `ExecuteCombatOrder`: out of reach -> chase (`ApproachTarget`), in reach -> plan the action
   (`PlanActionApproach`: the animation, the exact start/strike/end ticks, the facing) and enter **state 5** (`StartAction`).
3. **The hit is decided when the swing starts**, not when it lands: `ExecuteCombatOrder` builds the hit record (`BuildHitRecord` 0x54F630:
   damage roll, type factors, backstab, called shot) and asks the *target* to roll it (`ResolveHitChance` 0x553DF0 -> `ComputeHitChance`
   0x555BD0: skill + random band - distance term + weight ratio vs the target's defence). At the action's end tick `CompleteOrder` ->
   `FireAttack`: melee applies `target->vt[31]` (= `CUnitServer::TakeHit` 0x553F40) at once; bows/crossbows launch a `CEffectArrowFigure` that
   carries the already-resolved hit and applies it on arrival.
4. **Damage is per body part**: `UnitStats_ApplyHit` 0x5254B0 subtracts the placement armor (7 damage types, scaled by difficulty), the armor
   tiers of the part absorb more, the rest hits the part's HP (`UnitStats_ApplyDamage` 0x525070). Parts cripple (one blow >= part max of a
   crippling type) or die (HP <= 0). **The unit HP is derived**: `HP = maxHP - sum over parts of (max-hp)/max * rate * maxHP`
   (`UnitStats_ApplyBodyPartPenalties` 0x5259B0). Death = the placement HP < 1 (`specific vt[17]`), handled in `OnDamaged` 0x553AC0.
5. **Stats** live in `UnitStats` (0x704 bytes at `CUnitServer +0x1E0`): base attributes + perks -> current, `maxHP = str/25 * 30 * 1.1^L`,
   `L = log(exp/50)/log(1.61)`, action speed `max(1, (1+quickness%) * armFactor * (dex*0.2+10))`, attack period = weapon frames * 15 / action
   speed. Everything the client shows comes from the packed 0x108-byte copy at `CUnit +0x330` (net section 4).
6. **The client plays, it does not decide**: it receives the clip index/start tick (section 8), the stance/weapon (6), the packed stats (4), the
   move request (10); it computes the animation playback, sounds, blood, wound textures and a HUD hit-chance estimate (`CUnit::HitChanceVs`).

## 1. Conventions

| Thing | Value |
|---|---|
| unit object | `CUnit` (0x4DC bytes, kinds 0x32..0x34; 0x32 = humanoid with the full animation set); server side object `CUnitServer` (0xB20) at `CUnit +0x240`, its owner at `CUnitServer +0x1C`; client side `CUnitClientSpecific` (0xDC) at +0x240 |
| placement | `CMapObjectServer +0x20` (reached as `unit->specific(+0x170) + 0x20`): `+4 regen tick, +8 HP, +0xC max HP, +0x10 HP regen, +0x14 mana, +0x18 max, +0x1C mana regen, +0x20.. 7 armor pairs {base, current}` [V] |
| stats | `UnitStats` at `CUnitServer +0x1E0` (so `CUnitServer +0x250 = stats +0x70` etc.); packed form `CUnit +0x330` (`UnitStats_PackToRecord` 0x522CE0) |
| time | ticks (ints); animation durations in ticks = clip frames * `CUnit::AnimDurationFor` factor; attack period = `weaponProto +0x6C * 15 / actionSpeed` [V] |
| distances | metres; reach = own radius `CUnit +0x32C` + target radius + weapon range `CItemWeapon +0x58` (natural `stats +0x70`) [V] |
| damage types | index 0..6 = impaling, slashing, crushing, thermal ("termical"), chemical, electrical, general (log strings in `UnitStats_ApplyDamage`) [V] |
| hit locations | 0 head, 1 torso, 2/3 arms (log "LHand"/"RHand"), 4/5 legs, 6 = any / whole body [V] (`TakeHit` log table, `PickHitLocation` pairs) |
| weapon kinds (`CItemWeapon::GetDamageKind` 0x6A07F0 = weapon proto +0xC) | 0 sword 1 axe 2 dagger 3 spear 4 bludgeon 5 bow 6 crossbow, 8 natural/unarmed [I]; the attack kind mask `1 << kind` selects the animation set |
| random | ISAAC words `g_0x7A9D48[g_0x7A9D44]` (`rand % n`); the hero damage/accuracy use CRT `rand()` |
| difficulty | `g_0x79BEC0` indexes two float tables: 0x7C54A0[] scales the armor of non-player targets, 0x7C54AC[] scales the natural attack skill of monsters (both 1.0 at static init) [V] |
| stats constants (`AiParams_Init` 0x5CDFB0) | 0x7C5454/68 = 25, 0x7C5458/6C = 30, 0x7C545C/70 = 50, 0x7C5460/74 = 1.1, 0x7C5464/78 = 1.61, 0x7C5488 = 200, 0x7C548C = 1.61, 0x7C543C/40/4C = 0.5, 0x7C5448 = 0.33, 0x7C549C = 0.3, 0x7C5438 = 100 (turn mult), 0x7C5430 = 3.0 (backstab base) [V]; a config may override them at run time [?] |

## 2. The server unit tick

`CUnitServer::Update` 0x552300 (slot 6, called by the world for every unit) [V]:

```
alive (CUnit +0x274 == 0):
  Placement_Regenerate 0x523740 (every 15 ticks)  -> changed: UnitStats_Recompute2 0x524560, net sections 0 and 4
  (worldTick & +0x914) == 0 -> UnitGrid_Insert(0x7B2798, pos)  (+0x918 cell; the mask comes from the unit's speed, UpdateSpeedFactors)
  TickPassiveSpells 0x552B90
  player unit: AiMemory::ComputeThreatFactors(+0xA50) -> net 12; +0x938 idle counter: > 500 and rand%1001 == 0 -> player message 0x29, counter = 400
  Alertness::Tick(+0xB10); Brain::Tick(+0x980)        <- the AI (other document); it writes the queued order
  flags &= ~0x8000; PropagateAlarm 0x552E00
  state in {0,1,2,6} and !(flags & 0x1000000 frozen) and +0x144 == 0:  Think 0x54B390; TickState 0x5499F0; TickAnimation 0x546F30
  else: state 5 and queued order kind 1 -> CancelAction(force); TickState; TickAnimation
dead: z -= (+0x8F4 += 0.01) per tick until 0 (net 2); once (+0x18D): CWorldServer::OnUnitDied 0x593AF0, player notified, SetPlayer(0),
      Brain::Reset, both orders cleared; TickAnimation
```

`Think` 0x54B390: `DispatchQueuedOrder` (returns 1 when it started something: done); `UpdateStanceFlags` 0x54A620; `ApplyPendingStance`
0x54A150 (a stance transition animation is running: done); current order 1/2 in state 6 = finished: `+0x18C = 1`, player message 0xE when
`+0x145` (blocked) and no follow target, queue <- kind 0 (stand); 3..6: `TickCombatOrder` 0x54DD70; 7: queue <- 0; then `DispatchQueuedOrder` again.

`TickState` 0x5499F0: state 5 and `!TargetInRange` 0x549D40 -> `CancelAction(0)`; frozen -> `+0x20 = 1`; `+0x144` (a hit reaction
interrupted the animation) -> restore the saved queue `+0x148/+0x15C`; then per state (table below); `+0x160 = -1`.

`TickAnimation` 0x546F30: paused (`+0x20`) -> the clip, the move request ids and all timers (`+0x38, +0x15C, +0xDC, +0xE0, CUnit +0x258`) are
shifted by one tick (time stands still for the unit); `+0x21` (frozen animation) shifts the timers only; else `AdvanceAnimQueue` 0x546B40 pops
the finished entries and `CUnit::SetAnim` 0x52EA90 replicates the head (`+0x254` clip, `+0x255` next, `+0x256` flags, `+0x258` start tick,
section 8); looping entries (+8 == 1) keep the start tick moving.

### 2.1 States (`CUnitServer +0x164`) [V]

| state | meaning | entered by | TickState does |
|---|---|---|---|
| 0 | walking a path | `BeginMoveTo` 0x548700 | weapon switch (`+0x270 != +0x26C` -> vt[25] `SelectWeapon`); `+0x146` -> re-plan; `TickMovement` 0x547890 |
| 1 | turning in place | `StartTurnTo` 0x548960, order kind 2 | `TickMovement` |
| 2 | standing (waiting: order 0, or waiting for the target to come into reach) | `DispatchQueuedOrder` kind 0/11, `ApproachTarget` | weapon switch; pending hit reaction `+0x160` -> `RestartIdleAnim` 0x547100 |
| 3 | one-shot animation (stance transition, script animation, death, action recovery) | `PlayOneShotAnim` 0x548DE0 (was "StartAttacking") | queue empty -> state 6, callback `+0x978`, Think |
| 4 | looping script animation (order kind 8) | `PlayLoopAnim` 0x548FA0 (was "StartCasting") | like 2 |
| 5 | action wind-up (attack / cast / use item) | `StartAction` 0x5494D0 (was "StartAttackAnim2") | `TickAction` 0x549160 |
| 6 | idle, no order | `Stop` 0x555840, `CancelAction`, arrival | nothing (Think dispatches the next order) |

`SetState` 0x547090: 5 sets the timeout `+0x180 = now + 10`; every other state calls `CUnit::UpdateMoveParams` 0x530570 and drops the hit
effect `+0x17C`. The earlier names "3 attacking, 4 casting, 5 strike, FinishDying" were wrong (corrections below).

### 2.2 Stance, combat mode, running

`CUnit +0x264` = applied stance (0 crawl, 1 sneak, 2 walk, 3 run, 4 sit), `+0x268` = wanted stance. The server keeps two byte pairs:
`+0x168` current stance with 3 folded to 2 + `+0x16D running`, `+0x16C combat mode` (weapon drawn); `+0x170/+0x175/+0x174` the wanted
versions [V]. `UpdateStanceFlags` 0x54A620 derives them every Think: combat mode wanted = 1 for an attack order (kind 3), 0 for player units
otherwise (monsters keep it); run wanted (`+0x175 bit 1`) when the order's run byte (`+0x1B0`) is set and the unit stands.
`ApplyPendingStance` 0x54A150: wanted != current -> the new stance is tested on the passability map (refused: message 0xD), walking units
stop, the transition clip (`BuildTransitionAnim` 0x5516D0) plays in state 3 and the current order is pushed back to the queue; returns 1 while
the transition runs. `UpdateStance` 0x548520 (from `BeginMoveTo`) turns running on when wanted, standing, on walkable ground and not
crippled/overloaded (`UnitStats_EffectLevel` 0x5241C0: 1 = leg ratio < 0.5, 2 = carried > carry max -> messages 0x14/0x15), writes
`+0x264 = 3` and sets `+0x146` (the path is re-planned). A player's stance message (`CPlayerServer::OnMsg_AttackMode` 0x6709D0 ->
`RequestStance` 0x555270, was "SetAttackMode"): `StanceAllowedHere` 0x54A5A0, run refused when `EffectLevel(1) != 0`, message 9 on success.
Speeds per stance: `CUnit::GetSpeeds` 0x52F920 (pathfinding.md 5.3); `UpdateSpeedFactors` 0x5536B0 (slot 45) derives the AI factors:
noise `+0x8F8` (crawl 0.4, sneak 0.2, walk 1, run x2; 0 when standing), visibility `+0x900` (0.5 crawl, 0.75 sneak, x1.5 while casting a
non-silent spell), night visibility `+0x904 = world+0x88 * ((1-night) + night * nightSight%)`, grid refresh mask `+0x914` by speed.

## 3. Orders

### 3.1 The order record (0x24 bytes, current `+0x190`, queued `+0x1B4`) [V]

| off | field | use |
|---|---|---|
| +0 | kind | 0 stand, 1 move to, 2 turn to, 3 attack, 4 cast at point, 5 cast on object, 6 interact, 7 script animation, 8 looping script animation, 9 none, 10 use quick item, 11 sit |
| +4 | param | 1: group flag (`CollectPartyFollowers`); 3: hit location 0..5, 6 any; 4/5: spell item id (bit 31 set = weapon-slot quick item `id & 0x7FFFFFFF`); 6: 0 use, 1/2 skills (science / stealing), 0x18A95 talk; 10: slot |
| +8 | target | refcounted object |
| +0xC | pos[3] | 1: goal, z = max path length; 2: direction; 4: cast point |
| +0x18 | exact | position-locked: reach 0.2 (`OrderReachDistance`), 1e10 in `PlanActionApproach`, no path validation |
| +0x1C | extra | 1: 1 = report the move result (message 1 / 0xE); 2: turn speed; 7/8: clip index |
| +0x20 | run | stand up and run: forces wanted stance 2 when crawling/sneaking/sitting and sets the run flag |

Entry points (all write the queued slot, then `ValidateQueuedOrder` 0x54E600 for player units, which also sends the path preview
`CPlayerServer::SendMsg0E_MoveState` and the failure message 0xB/0xE):

| function | caller | queued |
|---|---|---|
| `QueueOrderMoveTo` 0x554740 / `QueueOrderMoveTo2` 0x5546B0 | AI, `WorldServer::OrderMoveTo` 0x5D39E0 (click), script `Order_MoveTo` | 1 / 2 (pathfinding.md) |
| `QueueOrderAttack` 0x554B30 (target, location, run, msg) | `WorldServer::OrderOnTarget` 0x5D3C30 <- `WorldServer::OnAttackOrder` 0x670190 (the player's click: unit ids, target id, location); `Brain::OrderAttack` 0x5D7B50 / `OrderAttackChecked` 0x5D7C60, `Brain::SelectCombatTarget` 0x5CFE40, `AiLogicPlayer::Regroup` 0x5D4170 | 3; a dead body part -> 6; melee on a unit of size >= 1.5 (`proto +0x24 + z`) without a bow -> message 0xC and refused |
| `QueueCastAt` 0x554D00 / `QueueCastOn` 0x554F50 (spell id, pos / target, run) | `CPlayerServer::OnMsg_CastAt` 0x6708D0 / `OnMsg_CastOn` 0x670820 -> `AiLogicPlayer::CastAt/CastOn` 0x5D3D90/0x5D3D20; `Brain::OrderCastAt` 0x5D7DA0, `Brain::ApplyCastOrder` 0x5CF170, `AiLogicAggression::Update` | 4 / 5; subtype-8 quick items (scrolls) are cast immediately; player units need `MagicSkill(school) >= spell +0x50` (message 0x13) and no flag 0x10000000 (silenced); validated with message 3 when affordable |
| `QueueOrderInteract` 0x5553B0 / `Interact2` 0x555480 / `UseOn` 0x555570 / `Talk` 0x555630 | `Brain::OrderInteract` 0x5D7CE0 | 6 with param 1 / 2 / 0 (+0x1C = param) / 0x18A95 |
| `QueueScriptOrder` 0x555700 (clip name) | `SSCScript::Run`, dialog scenes | 7 (8 when the clip entry's +0x14 bit 31 = loop), +0x1C = clip index |
| `QueueOrderSit` 0x555200 (was KindB) | script, `Brain::TickMoveOrder` | 11 |
| `QueueOrderUseQuickItem` 0x5557E0 (was KindA) | (no caller found) | 10 |
| `ClearQueuedOrder` 0x555180 | everywhere an order ends | 0 (stand) unless a script animation is queued |
| `RequestWeapon` 0x5552E0 (was SetOwnerField270) | `CPlayerServer::OnMsg_UnitByte` 0x670960 | `CUnit +0x270`; TickState switches via vt[25] `SelectWeapon` 0x553610 (needs durability `+0x20` > 0; `DropOtherWeapons` 0x5528F0 unlinks the other weapon items) |

`DispatchQueuedOrder` 0x54A900 [V]: nothing queued -> 0. Sitting (stance 4) and not kind 11 -> stand up first. The run byte with a
non-walking stance and no reachable combat target -> wanted stance 2. Then by kind: **0** `RebuildMoveAnims` idle set, `StopMove`, state 2;
**1** move (arrival pre-test `|d|^2 < 0.02`, `CollectPartyFollowers`, `+0x43C = IsCellWater`, `BeginMoveTo(pos, z)`, `ReportMoveResult`
when `+0x1AC`); **2** `CMover::ResetWithTarget` + state 1; **3..6** `+0x145 = 0`, `ExecuteCombatOrder`; **7** one clip
(`AnimFrames`) via `PlayOneShotAnim`; **8** via `PlayLoopAnim`; **10** `BuildActionAnimQueue(kind 6)` into the action record and
`StartAction(1, 0)`; **11** wanted stance 4 (refused -> message 0x1B); `+0x93C = tick`.

### 3.2 Combat orders per Think

`TickCombatOrder` 0x54DD70: player units must "know" a unit target (`AiMemory::IsFriendOrKnown` / `CPlayerServer::IsIdKnown`, else message
0x12); spells blocked by flag 0x10000000; a dead target (vt[55] != 0; talk/use on bodies allowed) or `+0x145` -> message 0xB (attack) / 0xE,
order cleared and `CancelAction(0)`; otherwise `ExecuteCombatOrder` 0x54D960:

1. weapon switch; `PlanActionApproach(order, +0xC4)` 0x54CA40 — fails (not in reach at any tick of the turn window) -> `ApproachTarget`
   0x54D140 (chase; pathfinding.md 5.5) and return.
2. stance: spells of humanoids while crawling -> wanted 1, any other order -> wanted 2; `ApplyPendingStance` running -> `+0x18E = 1`, return.
3. big living target (size >= 1.5) in melee -> message 0xC, order dropped.
4. attack period: an attack whose strike tick `+0xDC` would be before `+0x184` (next-attack tick) chases instead; otherwise
   `+0x184 = max(+0x184, now + AttackAnimDuration)` with `AttackAnimDuration` 0x54B9C0 = `round(weaponProto +0x6C * 15 / actionSpeed +0x210)` ticks.
5. spells: `CanAffordSpell` 0x54BF80 (mana `placement +0x34 >= cost +0x40 * 25/int`; quick items of subtype 5: charge `+0x28 >= cost`) else
   messages 0xF / 0x11.
6. kind 3 on a unit: `backstab = CUnit::IsBehindTarget` 0x530A60 (player units with a melee weapon; target facing . direction < -0.5), cancelled
   when the target is in combat mode or we use a bow; `BuildHitRecord(+0x108, backstab, +0xF0 location, +0xF4 called shot)`; the target's
   `ResolveHitChance(hit, us)` decides hit/miss now; `miss` -> the clip's miss flag (byte +2 of the entries: the client plays the miss sound).
7. `StartAction(keepFacing = target is not already attacking us, miss)` 0x5494D0: state 5, timeout `+0x180 = now + 10`, the mover turns the
   unit toward the target id `+0xE4` (`CMover::ResetWithTarget2`) or the point `+0xFC`, `CUnit::ResetMover`, `+0x278 = 0` (net 7).

### 3.3 The action record (`CUnitServer +0xC4 .. +0x104`) and the strike

`PlanActionApproach` builds it: an anim queue (the action clips with their durations, `+0xD8` start tick), `+0xDC strike tick`,
`+0xE0 end tick = start + duration`, `+0xE4 face target id`, `+0xE8 reach^2` (default 1e38), `+0xEC` hit effect flag, `+0xF0` hit location,
`+0xF4` called shot, `+0xF8` damage kind, `+0xFC` direction. It tries ticks `t = 0 .. round(pi / turnRate) + 3`: the target's predicted position
`vt[52](now + D + t)` must be within reach (moving targets: `(reach + 0.5)^2`, or `(0.5 * targetSpeed * (window/2 + D) + reach + 0.5)^2`
when it approaches), the bearing within 0.01 rad of our facing (else one tick per radian), and `CanCastAt` 0x54C2F0 (spells: line of sight,
monsters' max cast range). Reach = `OrderReachDistance` 0x54C070: attacks on a unit = own radius + target radius + max(weapon range / natural
range, 0.6 (0.2 between party members)); spells = the spell's range `+0x34` (10 without); talk = own radius + 0.3 * target radius.

`TickAction` 0x549160 (state 5): at `+0xDC` the record's clips become the current animation and, when `+0xEC` and `proto +0x114`, a
`CEffect` kind 0x4A (ids 0x2027..0x202E by `+0xF8`, 0x200B default) is attached (`+0x17C`); until `+0xE0` `Mover::Predict` keeps the unit
facing the target; at `+0xE0`: `CompleteOrder(+0xF0)` 0x549EC0, record reset, state 5 -> 3 (the recovery plays out), mover settled.
`CompleteOrder`: 3 -> `FireAttack(target, +0x108)`; the order stays unless the part is dead (then location 6) — the next swing is planned at the
next Think; 4/5 -> `CastOrderSpell` 0x54EE10; 6 -> `ExecuteInteraction` 0x54F000; 10 -> consume the quick item; 4..6/10 clear the order.
`CancelAction` 0x549690 (was FinishDying): before the strike tick -> state 6; after it -> the recovery is spliced and state 3; called by
`TickState` (target out of reach: `TargetInRange` tests `|target - us|^2 < +0xE8` at the end tick), `TickCombatOrder` (order failed) and
`Update` (a move order arrived while acting).

## 4. The attack

### 4.1 Attack parameters (`vt[50]`: `CUnitServer::GetAttackParams` 0x555A00, client mirror 0x538960) [V]

| index | value | heroes with a weapon in slot `+0x26C` | everyone else |
|---|---|---|---|
| [0] | damage kind | weapon proto +0xC | natural `stats +0xAC` |
| [1] | skill class | `CItemWeapon::GetSkill` | first nonzero index of `proto +0x50[7]` |
| [2] | weight | `CItem +0x34` | `stats +0x80` |
| [3] / [4] | damage min / max | `+0x50` / `+0x50 + +0x54` | `stats +0x84` / `+0x84 + +0x88` |
| [5] | accuracy spread | `stats +0x78` | same |
| [6] | attack skill | `UnitStats_WeaponSkill(kind)` 0x526C70 | `stats +0x74` (natural attack) |
| [7] | defence (parry) | `UnitStats_WeaponSkill` second form; **0 for bows/crossbows** | `stats +0x7C` |

`[3]`, `[4]` and the damage roll are scaled by the strengthen/weaken buffs: `(1 + 0.003*buff 0x1D) / (1 + 0.003*buff 0x1E)` (0x523D30).

`UnitStats_WeaponSkill(stats, kind, unit)` 0x526C70: melee kinds 0..4: `perkBonus(group = kind) + meleeByte +0x6CB` + (heroes) `weaponProto +0x7B`
+ `attribute - 25` (the governing attribute by the first letter of the skills table entry 0: 'd' dexterity, 'i' intelligence, 's' strength);
kinds 5/6: `archeryByte +0x6CC + proto +0x7C + perk group 5/6 + attribute - 25`; kind 8 (unarmed): melee byte + proto +0x7B + `SkillValue(0)`;
kind 11 the same without the perk; 7/9/10 -> 0. Monsters: just the byte (+ perk). Groups 0..6 = sword, axe, dagger, spear, bludgeon, bow, crossbow.

### 4.2 Hit chance [V]

`ComputeHitChance` 0x555BD0 (on the defender, A = attacker params, D = own):

```
roll   = A[5] > 0 ? 2 * rand(0..round(A[5])) - (A[5] + 1) : 0          # uniform in [-A5-1, A5-1], step 2
chance = roll + A[6] - dist(attacker, me) / (10 * attackerRadius vt[60]) + (backstab ? 0 : A[2] / (D[2] * 20))
chance = max(0, chance)
```
`ResolveHitChance` 0x553DF0: called shot (`hit[14]`): head -25, locations 2..5 -10 (torso 0), clamped; `defence = D[7]`, **0 against a
backstab** (`hit[13] > 1`); `hit[8] = +INF` when `chance >= defence` else `-INF`. A defender in combat mode (`+0x174` byte 0) cancels the
backstab first (`hit[13] = 1`). The client HUD percentage (`CUnit::HitChanceVs` 0x530CD0) is the same chance without the random term,
with `-2*dist/10` for ranged weapons, mapped over a +-5 band around the defence: `(chance + 5 - defence) * 10 %`, 100 above, 0 below.

### 4.3 The hit record (15 dwords, `CUnitServer +0x108`, `BuildHitRecord` 0x54F630) [V]

| idx (off) | heroes with a weapon | others | meaning |
|---|---|---|---|
| 0..6 (+0) | weapon proto +0x98.. 7 factors | `stats +0x90` 7 factors (udb +0x50) | damage type distribution; a spell/effect hit puts a direction into [3..5] (`Hit_HasDirection` 0x522B80) |
| 7 (+0x1C) | `CItemWeapon +0x4C` | 0 | not read by the damage code |
| 8 (+0x20) | random | `stats +0x74 * difficulty 0x7C54AC[]` | the roll; overwritten with +-INF by `ResolveHitChance`; a finite value = unresolved (spells are rolled in `TakeHit`) |
| 9 (+0x24) | `proto +0x50 + rand * +0x54` (`ItemProto_RandomValue50` 0x6A2DE0) | `stats +0x84 + rand(0..+0x88)` | **damage**, then * strengthen/weaken |
| 10 (+0x28) | 1.0 | 1.0 | armor factor (multiplies the armor subtracted) |
| 11 (+0x2C) | hit location | | 0..5, 6 = whole body |
| 12 (+0x30) | 0 | | direction code handed to `OnDamaged` |
| 13 (+0x34) | 1.0, backstab: `3.0 + backstab perk %` (`UnitStats_PerkBonusB` 0x525D60, group 22) | 1.0 | > 1: no defence and the damage multiplier ("backstab with power" in the log) |
| 14 (+0x38) | called shot byte | | |

Player `+0x108 > 1` (a cheat level) replaces the record with `Hit_Init` 0x522B40 (all factors 1, damage 3.4e36).

### 4.4 Damage application (`TakeHit` 0x553F40 -> `UnitStats_ApplyHit` 0x5254B0 -> `UnitStats_ApplyDamage` 0x525070) [V]

`CUnitServer::TakeHit(hit, attackerUnit, attackerObject)` (slot 20; reached through `CMapObject` slot 31 from `FireAttack`, the arrow and
the spell effects): unresolved hits are rolled here; alive targets are activated and logged ("Unit N was attacked by M skill .. damage ..
destHL Head/.."). **Player units take no damage while their player's `+0x108` (god mode) is set or when the attacker belongs to another
player** (no PvP) [V]. Then `hit[8]` must reach own `[7]` (0 vs backstab); `+0x8D8 |= 0x10`, net 4, `TriggerArmorSpells(1)` (on-hit item
spells), `UnitStats_ApplyHit` with the attacker's equipped weapon:

```
for each type i:  eff_i = max(0, dmg * f_i - hit[10] * placementArmor_i (+0x24 + 8i) * diff)   # diff = 0x7C54A0[difficulty] for monsters, 1 for player units
                  absorbed (dmg*f_i - eff_i) wears the attacker's weapon: Placement_Damage(weapon, 0x24 low / 0x26 broken)
                  f_i := eff_i / dmg
total <= 0 -> absorbed, nothing happens.  location != 6 -> ApplyDamage(part)   location 6 -> split over the live parts by rate/maxHP
ApplyDamage(part = +0x110 + loc*0xF4):   state < 2 -> nothing
  per type: d = f_i * dmg; the armor tiers (heroes 2..0, others tier 0 only) with state > 1 absorb hit[10] * tier_i each (armor items worn:
            Unit_DamageArmorSlots 0x525720 -> messages 0x23/0x25), the rest * hit[13]: part.hp -= d
            d >= part.max and type bit in part.crippleMask (+4), or hp < -5*max  -> state 1 crippled, hp = -5*max, message 0x1E (arm) / 0x1F (leg)
            else hp <= 0 -> state 2 ("HL is dead")
  hit[13] != 1 -> statFlags |= 1;   placement.hp = ApplyBodyPartPenalties(placement.maxHP)
```
`UnitStats_ApplyBodyPartPenalties(stats, v)` 0x5259B0 = `v - sum over parts with state > 0 of (hp < 0 ? v*rate : (max-hp)/max * rate * v)`
(`rate` = part +0xF0): the unit HP is always this of the max HP. `CUnit::DropBrokenItems` 0x532AC0 unequips items with durability < 1.

After damage passed (`ApplyHit != 0`): `CUnit +0x350 = death kind` (7 = no attacker / effect, else the weapon kind, natural kind of a unit,
`typeId + 0x10` of an object), `Recompute2`, net 0/4, `OnDamaged(attacker, hit[12])` 0x553AC0 (was "OnKilledBy"): **still alive**
(`specific vt[17] == 0`, HP >= 1): `TriggerArmorSpells(0)`, `AggroTable_Add` 0x455680, `UnitAI_OnAttackedBy` 0x5D7E90, alarm flag 0x80000 ->
`PropagateAlarm`, `+0x160 = HitDirectionFlag` 0x5518F0 (front 0xC00000 / back 0x1000000 / sides 0x800000, 0x400000: the hit reaction
`BuildHitDeathAnimQueue(dir, 0)` plays at the next TickState through `RestartIdleAnim`, interrupting the current clip and freezing the timers
`+0x21`). **Dead**: attacker alertness 9 or player message 0x30; `WitnessMurder` 0x5538D0 on both known-unit lists; `ClearBuffs`; `SetDead(1)`
0x546720 (`CUnit +0x274`, grid re-registration, net 7); the death clip (`BuildHitDeathAnimQueue(dir, 1)`: class 0x1C0000, variants per stance)
via `PlayOneShotAnim` (state 3); stimuli removed; party members `AiMemory::SeeCorpse`; multiplayer flag 0x20000000. Back in `TakeHit`: a
player unit's kill of a non-player unit gives `CWorldServer::GiveExperience(player, proto +0x54)` 0x5911C0 (split among the player's live
heroes, or by `sqrt(exp)` weighting when no player). A miss / absorbed hit returns 0 and only records aggression (`AggroTable_Add`,
`UnitAI_OnAttackedBy`). The corpse: `Update` sinks it 0.01 m/tick^2; the client plays "mo_dw" wound textures and effect 0x2043.

`FireAttack` 0x54F450 also triggers the weapon's embedded spell (`TriggerWeaponSpell` 0x54F850: `item +0x44`, flag bit 16, cost <= charge
`+0x28`) and makes noise (`StimulusMap::HearSound`, radius `stats +0xFC * 2`, kind 0x1A) unless the hit is a backstab.
**Ranged**: `CEffectArrowFigure` (kind 0x5B) gets the owner's attach point, `+0x150` target id, `+0x15C` shooter, the 15-dword hit at
`+0x160`; `CEffectArrowFigure::Update` 0x67BE20 moves it along `ComputeFlightVector` and, when `+0x14C` (arrived) on the server, calls
`target->vt[31](hit, shooter, shooter)`: **an arrow always reaches its target and the hit/miss was already rolled at the shot** [V]; there is no
projectile collision with other units in this code.

### 4.5 Attack animation selection

`ChooseAttackAnim` 0x54F970: the body part (order param 6 = random from {0 x2, 1 x6, 2..5 x3 each}, resolved by `UnitStats_PickHitLocation`
0x525B20: the wanted part if functional, else its pair 0<->1, 2<->3, 4<->5, else a random live part, else torso); the strike height band from
both units' heights (`CUnit +0x14C * 2`): flag 0x1000000 high / 0x2000000 middle / 0x3000000 low (| 0x800000), by slope for targets farther
than 5 m; non-humanoids: 0x2000000.. by reach `(stats +0x64 + 1) * radius`. `BuildActionAnimQueue` 0x550100: clip key = `attackKindMask |
flags | 0x90000`, with stance transitions around it; duration = frames * `CUnit::AnimDurationFor` 0x5327D0 (per-unit factors `+0x4B0` attack,
`+0x4B4` cast, `+0x4B8` hit, `+0x4BC` death, `+0x4C0` transition, `+0x4C4` special). Clip keys (`CUnit::FindAnimRecord` 0x5301B0): bits
0..6 weapon kind, 0x8000 sit / 0x10000 walk / 0x20000 sneak / 0x30000 crawl (`MoveModeAnimFlag` 0x5500A0), class bits 0x3C0000 (0x80000 attack,
0xC0000 cast, 0x1C0000 death, 0x200000 hit, 0x240000 transition, 0x40000 special), direction/height bits above.

## 5. Stats (`UnitStats`, 0x704 bytes at `CUnitServer +0x1E0`)

Origin: `Unit_InitStatsFromProto` 0x5136D0 (placement, stats, proto name) from the units database record (`UnitProto`, 0x120 bytes,
`g_0x7AF358`) and its "udb" entry (0x168 bytes, `g_0x7AF398`, index `proto +8`); heroes also from the party record (attributes, skill bytes,
experience, perk names). The full field list is in `batch-combat-structs.tsv`; the ones that matter:

| off | field | source / formula |
|---|---|---|
| +4 | experience | hero record +8; +0x700 = fractional accumulator (skill costs, timed gain) — **not an HP fraction** |
| +8 | expSpent | |
| +0xC/+0x10, +0x14/+0x18, +0x1C/+0x20 | str, dex, int base / current | monsters: `proto +0x48 (HP)/3`, `proto +0x4C (MP)/3`, 25; current = base + perk groups 23/24/25 |
| +0x24, +0x28 | base speed, speed | `proto +0x60`; `speed = base * legFactor` -> `CUnit +0x344` |
| +0x2C, +0x30 | action speed base, action speed | `dex*0.2 + 10` (15 for monsters); `max(1, (1+quickness%) * armFactor * base + buff 0x21 - buff 0x22)`; **attack period = weapon frames * 15 / this** |
| +0x34..+0x40 | stance speed multipliers | udb +0x1C.. -> `CUnit +0x340..+0x343` |
| +0x44 | cos(view angle / 2) | udb +0x18; **-2.0 for player units** (360 deg vision) |
| +0x50 / +0x54 | carried weight / carry max | sum of item `+0x34`; `str * 12 * (1 + lift%)` (1e6 for monsters) |
| +0x58 / +0x5C | leg / arm factor | 1, 0.5 (ratio <= 0.5), 0.33 (ratio <= 0) by the worst leg / arm part; legs only when move type +0x60 == 1 |
| +0x70..+0x88 | natural attack: range, attack, spread, defence, weight, damage min, damage spread | proto +0x64..+0x74, +0x80, +0x84 |
| +0x90[7], +0xAC | natural damage factors, natural attack kind | udb +0x50, proto +0x78 |
| +0xB0.. | 6 perception pairs {base, current} | proto +0x98..: sight (`CUnitServer +0x290/+0x294`), night sight (+0xBC *= 1 + night vision perk %), hearing?, ...; buffs 12/13/14 |
| +0xE0.. | 6 resistance pairs | proto +0xB0..; buffs 15/16/17 subtract, 19 adds; +0xFC = noise radius |
| +0x110 | 6 body parts x 0xF4 | kind (+0: 2 arm, 3 leg), cripple mask (+4), 3 armor tiers (+8, 0x48 each: 7 values at +4, state at +0x3C), state +0xE0 (3 ok, 2 dead, 1 crippled), hp +0xE4, max +0xE8 = `maxHP / torsoFraction(+0x2F0) * fraction(+0xEC)`, rate +0xF0 |
| +0x6C8[3] | attribute skills (science, stealing, ?) | `UnitStats_AttrSkill` 0x525E10: `dex - 25 + byte` (int for 2) |
| +0x6CB[8] | melee, archery, ?, magic schools 0..2 / 3..4 / 5..7, ?, ? | `UnitStats_MagicSkill` 0x527EB0 = `int - 25 + school byte + perk` |
| +0x6D3[36] | perk levels per group | 26 groups x 3 levels; perk table entry `g_0x7AF274[group*3 - 1 + level] +0x34` = bonus % |
| +0x6F8 | stat flags | bit 0 backstabbed, 1 head hit, 3 HP > 0 |
| +0x6FC | stored max HP | monsters (proto +0x48) |

Derived values [V]: `MaxHP` 0x523E80 = `(str/25) * 30 * 1.1^L * (1 + health perk %)`, `MaxMana` 0x523F50 the same with dex and the mana perk,
`L = log(exp/50)/log(1.61)` [I: the decompiler shows the two logs and a pow helper; 1.1 is read by `UnitStats_BoostMaxHP`]; `GetLevel`
0x556760 = `round(log(exp/200)/log(1.61))`. Max HP is then multiplied by the strengthen/weaken ratio `(1+0.005 b1)/(1+0.005 b2)` and
**capped at 1.0** in every refresh (`Recompute2`, `RefreshMaxHP` 0x5243D0) — the placement max HP seems to be kept as a fraction [?].
Mana cost of a spell = `spell +0x40 * 25 / int` (`UnitStats_ManaCostFactor` 0x523CD0). Regeneration (`Placement_Regenerate` 0x523740, every
15 ticks): mana `+= (1 + spirit%) * manaRegen * maxMana` only without an order; HP: `rate = (1 + vitality%) * hpRegen * buff 0x1F`, the
parts are healed by `UnitStats_SpendEffectBudget` 0x525800 (shared by part rate, a part becomes functional again at hp >= 0.001) and the HP
re-derived. `Heal` 0x5543B0 (slot 21) = the same with an amount. Skills raise through `UnitStats_RaiseSkill` 0x528AB0 (cost into the experience
accumulator and `+8`), perks through `UnitStats_LearnPerk` 0x526450 (see docs/game-memory.md for the costs, verified live).

## 6. Items in combat

`CUnit +0x440[4]` weapon slots, `+0x450[4]` quick slots, `+0x460[8]` inventory (spells: item `+0x44`), `+0x480[7]` armor (slot = `CItem::GetType`).
The weapon in hand is the id `+0x26C` (`+0x270` requested). Armor: `UnitStats_RebuildFromArmor` 0x523300 (slot 27) clears the tiers and
`UnitStats_AddArmorModifiers` 0x5230C0 adds each equipped armor's 7 values (`+0x4C`, second set `+0x68`) into the parts it covers (table
0x73E048[type*3 + k]) at its tier (`CItem::GetSlotClass`), setting the tier state 3 and the cripple mask. Durability: items are placements too
(`+0x20` durability, `+0x24` max, `+0x48` count): weapons wear by the damage absorbed by armor, armor by the damage it absorbs; < 1 ->
`DropBrokenItems`. Ammunition: none in this code — bows need no arrows (the client only shows "rh3.arrow00", `UpdateArrowVisibility` 0x538C20).
Quick items (`CItemQuick` typeId 0x13): subtype 2 consumables (order 10), 5 charged magic items (charge `+0x28` >= spell cost), 8 scrolls
(cast at once by `QueueCastAt/On`, `Unit_ConsumeItemCharge` 0x595020). Passive item spells: `TickPassiveSpells` 0x552B90 casts affordable
embedded spells of the `+0x920` list on self on a countdown (`spell +0x20 - 3`, min 8); kind 0x18 ones only while hurt. Armor spells:
`TriggerArmorSpells` 0x54F8E0 on hit (kind != 0x18) and on death (kind 0x18).

## 7. Spells cast by units (entry points only)

`QueueCastAt/On` -> order 4/5 -> `PlanActionApproach` uses `BuildSpellAnimQueue` 0x550620 with cast time `spell +0x4C * 15 / actionSpeed`
(a hold entry pads a short clip) -> `StartAction` -> `CompleteOrder` -> `CastOrderSpell` 0x54EE10: mana `placement +0x34 -= cost * 25/int`
(player units) or the quick item's charge, `CWorldServer::CastSpellMulti` 0x58E850 (spell, caster, caster pos, target, target pos), noise
unless `spellProto +0x10 == 1` (silent). Range / line of sight: `OrderReachDistance` (spell `+0x34`), `CanCastAt` 0x54C2F0 ->
`ComputeCastTarget` 0x54BBC0 (`CAIMap::TraceVisibility`). Interrupt: a hit reaction does not cancel state 5 (`RestartIdleAnim` only runs in
states 2/4); the cast is lost when the target leaves `+0xE8` (`CancelAction`), when the order is replaced by a move (`Update`), or when the
unit dies. Silence = unit flag 0x10000000 (`v49_UpdateSilenced` 0x553220: buff kind 0x20). `CastFromSlot` 0x54ED60 serves the client
messages 0x32/0x33 (use weapon-slot item). Spell damage comes back through `CSpellEffect` -> `TakeHit` with a directional hit record.

## 8. Client side (`CUnitClientSpecific`, 0xDC bytes)

Replicated by `CUnit::WriteNetState` 0x52EC70 (sections): 4 = the packed stats `+0x330`; 5 = the 4 weapon + 7 armor item ids; 6 = stance,
wanted stance, weapon id; 7 = walking / dead / anim loop / pause bits; 8 = clip `+0x254`, next `+0x255`, start tick `+0x258`, flags `+0x256`;
9 = name; 10 = the move request; 11 = masks / side / player; 12 = threat factors `+0x4C8`; 13 = `+0x438`. The client computes itself:
animation playback (`SetAnimation` 0x5352B0: `CAnimationSystem2::SetAnimation / QueueAnimation` of the two clip indices, seeking to
`time + 1 - startTick`; -2 = the weapon-dependent idle set `ApplyWeaponAnimMask` 0x535480), movement replay (`ReplayMove` 0x535850),
sounds (`UpdateAnimSounds` 0x534B60 from the clip's events, the miss flag picks the miss sound; footsteps `GetStepSoundName` 0x534590;
voices `+0x9C` queue), hit effects (`v48_SpawnBlood` 0x5373D0 blood prints by `proto +0x108`, `v47_DeathEffect` 0x537C00, wound textures
`GetBodyPartDamageLevels` 0x536EA0 from the packed parts: 0 >= 75 %, 1 >= 33 %, 2 below, 3 dead), the HUD numbers (`CUnit +0x17E/+0x180/+0x182`
HP / max / mana shorts, `GetDamage7` 0x54BA30 displayed damage, `HitChanceVs` estimate, `GetLevel` 0x539160 from `+0x334`), weapon figure
attachment (`v25_SetWeaponFigure` 0x535590, `Equip/Unequip` 0x5381B0/0x5385A0). Nothing on the client rolls or damages.

## 9. What is not known

* `CItemWeapon +0x4C` (hit[7]) and `UnitStats +0x48/+0x8C` (proto +0x94/+0x5C), udb +0x44.. shorts: copied, never read in these modules.
* Whether the placement max HP is really a fraction (the `min(1.0, ...)` cap after the buff ratio in 0x524560 / 0x5243D0) or a decompiler
  artefact; live memory showed max HP ~ 70 at `specific +0x2C` (docs/game-memory.md), so the cap is probably on a different value [?].
* The exact pow in `MaxHP` (`atan2_wrap` is the x87 helper the decompiler could not name); the constants may be overridden from a config.
* The player's `+0x108` levels (0 normal, 1 no damage?, > 1 instant kill) and the unit event message codes of `SendMsg0B` beyond those
  listed in `NotifyPlayer`'s batch row (inferred from their call sites).
* `UnitStats_TickExperienceAndMaxima` 0x5239B0 (experience grows with dt): its callers are outside this round.
* The second value set of armor items (`+0x68`, tier `+0x20..`): written by `AddArmorModifiers`, not read by `ApplyDamage`.
* Order kind 10 has no caller found; `CWorldServer::SpawnDebugUnit` as the consumable effect looks like a misnamed function.

## 10. Corrections to earlier notes

1. `docs/game-exe/flows.md` "Server unit tick": Think/TickState run for states {0,1,2,6}, not "0..2 / 8"; `FinishDying` 0x549690 is
   `CancelAction` (aborts the current attack/cast), `StartAttacking` 0x548DE0 is `PlayOneShotAnim`, `StartCasting` is `PlayLoopAnim`,
   `TickAttackAnim` is `TickAction`, `StartAttackAnim2` is `StartAction`, `OnKilledBy` 0x553AC0 is `OnDamaged` (runs on every damaging hit).
2. `full/structs.tsv` `CUnitServer +0x164`: "3 attacking 4 casting 5 strike" -> 3 one-shot animation, 4 looping animation, 5 action wind-up.
3. `full/structs.tsv` `UnitStats +0x700 hpFraction` -> experience accumulator; `+0x2C moveSpeedBase` / `+0x30 actionPoints` -> action
   speed base / action speed (the attack period divisor); `+0x58/+0x5C crippleRatio` -> leg / arm factor; `+0x2F0 scale` -> torso HP fraction;
   `CUnit +0x34F` / `CUnitServer +0x28C bodyClass` -> natural attack kind; `CItemWeapon +0x4C..+0x58 range*` -> +0x50 damage min, +0x54 damage
   spread, +0x58 attack range.
4. `docs/game-memory.md` "Stance, weapons and orders": `+0x1A4 = 1000000.0 while attacking` is the order's pos z = max path length (pathfinding.md
   already); order type 5 is "cast on a unit" (4 = cast at a point), 0 is "stand" (queued by every finished order); "sub_54A900 moves the pending
   order" is right. `UnitStats_AccumulateHP` (0x523920) in `flows.md` / functions.tsv accumulates experience, not HP.
5. `docs/game-memory.md` "Code writing HP": sub_525070 applies damage to a body part; the unit HP written by sub_524560 / sub_5243D0 /
   sub_523740 is always the derived `maxHP - part penalties`, so writing the HP directly is undone at the next recompute (edit the parts).
6. `CUnitServer::SetAttackMode` (0x555270 / 0x546830) set the *stance* (`CUnit +0x268`), not an attack mode; the combat mode is the byte pair
   `+0x16C/+0x174`.
7. `CUnit::InitIdFields` 0x52E0D0 writes the natural attack/defence skills into the attack-params array, not unit ids.

## 11. Function index

Tick: Update 0x552300, Think 0x54B390, TickState 0x5499F0, TickAnimation 0x546F30, SetState 0x547090, AdvanceAnimQueue 0x546B40, SetAnimQueue
0x546C40, PlayCurrentAnim 0x546C10, RestartIdleAnim 0x547100, CurrentAnimIsHitReaction 0x546EE0, PlayOneShotAnim 0x548DE0, PlayLoopAnim 0x548FA0,
UpdateSpeedFactors 0x5536B0, ComputeMoveClasses 0x552FB0, TickPassiveSpells 0x552B90, PropagateAlarm 0x552E00, DeclareEnemies 0x552D00.
Orders: DispatchQueuedOrder 0x54A900, ActivateQueuedOrder 0x54A6E0, ValidateQueuedOrder 0x54E600, QueueOrderAttack 0x554B30, QueueCastAt 0x554D00,
QueueCastOn 0x554F50, QueueOrderInteract* 0x5553B0/0x555480/0x555570/0x555630, QueueScriptOrder 0x555700, QueueOrderSit 0x555200, QueueOrderUseQuickItem
0x5557E0, ClearQueuedOrder 0x555180, Stop 0x555840, ResetOrdersAndStance 0x554450, ForgetObject 0x552670, Order_Clear/Init/Assign 0x545F40/0x545F70/0x54B320,
RequestStance 0x555270, SetWantedStance 0x546830, RequestWeapon 0x5552E0, SelectWeapon 0x553610, UpdateStanceFlags 0x54A620, ApplyPendingStance 0x54A150,
UpdateStance 0x548520, StanceAllowedHere 0x54A5A0; player side: OnAttackOrder 0x670190 -> OrderOnTarget 0x5D3C30, OnMsg_CastAt/On 0x6708D0/0x670820,
OnMsg_AttackMode 0x6709D0, OnMsg_UnitByte 0x670960, OnMsg_MoveTo 0x66FBD0.
Combat: TickCombatOrder 0x54DD70, ExecuteCombatOrder 0x54D960, PlanActionApproach 0x54CA40, ApproachTarget 0x54D140, OrderReachDistance 0x54C070,
CanReachTarget 0x54C800, CanReachOrderTarget 0x54C8B0, IsOrderTargetReachable 0x54C6B0, CanCastAt 0x54C2F0, StartAction 0x5494D0, TickAction 0x549160,
CancelAction 0x549690, CompleteOrder 0x549EC0, TargetInRange 0x549D40, AttackAnimDuration 0x54B9C0, ChooseAttackAnim 0x54F970, BuildActionAnimQueue 0x550100,
BuildSpellAnimQueue 0x550620, BuildHitDeathAnimQueue 0x550A80, HitDirectionFlag 0x5518F0, BuildHitRecordFor 0x54F580, BuildHitRecord 0x54F630, Hit_Init 0x522B40,
GetAttackParams 0x555A00 / 0x538960, ComputeHitChance 0x555BD0, ResolveHitChance 0x553DF0, HitChanceVs 0x530CD0, EstimateHitChanceVs 0x530BF0, IsBehindTarget 0x530A60,
FireAttack 0x54F450, TakeHit 0x553F40, OnDamaged 0x553AC0, Heal 0x5543B0, OnProvoked 0x551AE0, WitnessMurder 0x5538D0, TriggerWeaponSpell 0x54F850,
TriggerArmorSpells 0x54F8E0, CastOrderSpell 0x54EE10, CanAffordSpell 0x54BF80, CanCastSpell 0x54B950, ExecuteInteraction 0x54F000, arrow Update 0x67BE20,
GiveExperience 0x5911C0, AggroTable_Add 0x455680.
Stats: Unit_InitStatsFromProto 0x5136D0, UnitStats_Init 0x5233E0, Recompute2 0x524560, Recompute 0x524CD0, RefreshMaxHP 0x5243D0, RefreshPlacementMaxima 0x5288B0,
MaxHP 0x523E80, MaxMana 0x523F50, CarryWeight 0x524280, MoveSpeed 0x524320, LegFactor 0x524060, ArmFactor 0x524110, EffectLevel 0x5241C0, Placement_Regenerate 0x523740,
SpendEffectBudget 0x525800, UnitStats_Heal 0x525900, ApplyHit 0x5254B0, ApplyDamage 0x525070, ApplyBodyPartPenalties 0x5259B0, PickHitLocation 0x525B20,
Unit_DamageArmorSlots 0x525720, Placement_Damage 0x5253E0, Placement_ApplyHit 0x524FE0, ScaleParts 0x525A00, RescaleParts 0x525A70, HealAllParts 0x523C80,
AddArmorModifiers 0x5230C0, RebuildFromArmor 0x523300, ComputeEffective7 0x523020, ComputeAttack 0x553450, PackToRecord 0x522CE0, PackPart 0x522BC0,
WeaponSkill 0x526C70, SkillValue 0x527050, AttrSkill 0x525E10, AttrSkillWithWeapons 0x525E90, MagicSkill 0x527EB0, PerkBonus 0x5282D0, LearnPerk 0x526450,
RaiseSkill 0x528AB0, Perk_ComputeCost 0x528050, GetLevel 0x556760, ManaCostFactor 0x523CD0, ApplyRangeBuffs 0x523D30, AddExperience 0x523920.
CUnit: GetSpeeds 0x52F920, UpdateMoveParams 0x530570, UpdatePackedStats 0x532A20, SetAnim 0x52EA90, AnimDurationFor 0x5327D0, FindAnimRecord 0x5301B0,
SpellUsability 0x52F820, QuickItemUsability 0x52F8B0, EquipArmor 0x530730, DropBrokenItems 0x532AC0, GetCarriedWeight 0x5307B0, WriteNetState 0x52EC70, ReadNetState 0x52EF90.
Client: Update 0x5341A0, SetAnimation 0x5352B0, ApplyWeaponAnimMask 0x535480, PlayAnimation 0x537500, UpdateAnimSounds 0x534B60, GetBodyPartDamageLevels 0x536EA0,
v48_SpawnBlood 0x5373D0, v47_DeathEffect 0x537C00, v31_ApplyWoundTextures 0x537360, UpdateArrowVisibility 0x538C20, GetLevel 0x539160, ReplayMove 0x535850.
