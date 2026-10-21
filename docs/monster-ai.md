# How the game's monsters notice and fight (notes for um-bot)

Read from game.exe (the AI module 0x5C8000 - 0x5D3000: `StimulusMap`, `AiMemory`, `Brain`, `AiLogic*`) and from a running game's memory (2026-10-20). Used by um-bot to pick its targets.

## The AI parameters (`AiInfo`, the global at 0x7C53F8, loaded by `AiInfo_Load` 0x5CE0F0 from aiinfo.res's [Logic] section; values read live in the multiplayer rig)

| offset | name | value | meaning |
|---|---|---|---|
| +0x04 | GuardStayTime | 10 | a guard stays this long at its post |
| +0x08 | SentryStayTime | 10 | a sentry stays this long |
| +0x0C / +0x10 | FollowMinDist / FollowMaxDist | 2 / 4 | a follower keeps this distance from its leader |
| +0x14 | GuardRadius | 10 | a guard reacts inside this radius of its post |
| +0x18 / +0x1C | CallForHelpRadius / ...Default | 7 / 7 | **a monster that is hit or sees an enemy calls its friends within this radius** |
| +0x20 | FollowLimit | 15 | how far a chase may go from the start |
| +0x24 | PlayerCallForHelp | 16 | the radius for a player's call |
| +0x28 / +0x2C | RangedRunDist / RangedAttackDist | 4 / 7 | ranged monsters keep away to 4 and shoot from 7 |
| +0x30 | SwarmRadius | 10 | the radius of a swarm (packs of animals) |
| +0xA0 | QuestsToRespawn | 3 | quests until a dead hero respawns in a campaign |

(The skill / HP / mana tables of the same block: +0x5C.. HP 25, 30, 50, 1.1, 1.61; +0x70.. mana the same; skill 50, 5; ability base 1.95; exp 200, 1.61: see `docs/game-memory.md`.)

## Who reads the [Logic] values (found 2026-10-21 by scanning game.exe for the globals 0x7C53FC..0x7C5428)

| value (global) | read by |
|---|---|
| GuardStayTime (0x7C53FC) | `GuardPoint_Init` 0x5D0B70, `Brain::AddLogicStand` 0x5D6300, `AiLogicSentry::Update` 0x5D1B70 |
| SentryStayTime (0x7C5400) | `AiLogicSentry::Update` |
| FollowMinDist / FollowMaxDist (0x7C5404 / 08) | `Brain::TickFollow` 0x5D6F90 (a follower walks when farther than Max, stops inside Min), `Brain::IsNearLeader` 0x5D4860 (Max) |
| GuardRadius (0x7C540C) | `GuardPoint_Init`, `Brain::AddLogicStand` |
| CallForHelpRadius / ...Default (0x7C5410 / 14) | `Brain::BuildFromLogicSlot` 0x5D6630, `Brain::AddLogicSentry` 0x5D65B0, `Brain::Read` 0x5D8310 (a unit whose own radius equals the default gets the first one) |
| FollowLimit (0x7C5418) | **no reader found** (the 15-unit chase limit is not this value) |
| PlayerCallForHelp (0x7C541C) | `Brain::CollectCallers` 0x5D3F50 |
| RangedRunDist / RangedAttackDist (0x7C5420 / 24) | only `AiLogicFear::Enter` 0x5D2960 (the logic of a monster that flees: it keeps 4 away and shoots from 7) |
| SwarmRadius (0x7C5428) | `AiLogicPlayer::Regroup` 0x5D4170 |

## The logic of a player's own units (`AiLogicPlayer`, brain kind 6): what the game does for a hero that has no order

`AiLogicPlayer::Update` 0x5D4A90 runs for every hero. The unit field +0x438 is the **"battle" button** of the hero panel (`CUIWindow742530::OnSelect` 0x649810 sends client message 0x3B: u32 unit, u8 0 or 1; the unit's default is 1; the value is stored in the party record). The player's mode +0x5120 (set by `CPlayerServer::SendMsg60_PartyStatus`) is 1 when a hero is hurt, 2 when a hero of the party is fighting.
- With +0x438 == 1 the idle hero runs `Regroup`: it keeps its current target while `TryAttackTarget` still allows it; else it collects the **units that attack it** (`AiMemory::CollectAttackers`; around the destination within SwarmRadius 10 while it walks to a point, else around itself), drops the ones farther than 1.1 x their own sight range, adds the **units attacking any other hero of the same player within PlayerCallForHelp 16** (`CollectCallers`: same player id only), scores them with `Brain::ChooseTarget` and orders `QueueOrderAttack(target, mode 6)`; when the player's mode is not 2 the hero's followers stop following first.
- So the game's defence reacts to attackers of the hero itself and of the same player's heroes. A **host's** attackers are another player's business: the bot's hero never reacts to them by itself, and a second player in a session is not a "caller".
- Any order the client sends (move, follow walk, attack) replaces what `Regroup` chose, so an order every 2 s (um-bot's follow) keeps the hero from ever fighting by itself.
- The attack order itself: the server walks the hero into range (`ApproachTarget` 0x54D140 / `OrderReachDistance` 0x54C070: weapon range, not melee: a bow needs no walk to 3 units); `CanReachTarget` lets ranged attackers (weapon type 5 / 6) attack any size, melee ones only targets of size < 1.5. The order is refused when the target is not in the hero's known list (see multiplayer-protocol.md).

## Perception (`AiMemory::ComputeThreatFactors` 0x5CB850, `StimulusMap`)

A monster knows a unit once a *factor* drops below 1; the factors are distance / (range x modifiers), kept at most 3, one per sense:
- **sight**: the other's sight range (+0x904: 8.5 for the animals and monsters seen, 13 for a player's hero; the perk "night vision" raises the hero's) x the looking unit's factor (+0x900) x the seen unit's visibility (+0x294) x the looker's (+0x2C4), and times the terrain's line of sight (`CAIMap::TraceVisibility`: trees and rocks hide). The view cone is narrow for animals (the UNITS command shows `fov=50`, a hero 75).
- **hearing**: footsteps (`StimulusMap::PerceiveFootsteps`) and sounds (`StimulusMap::HearSound`: a fight, a loot, a cast make noise with a radius of the unit's noise value x 1.5).
- **smell**: a third range (+0x2B4), used by animals.
A sneaking or crawling hero is harder to see and hear (its visibility and noise factors are smaller), a running one easier.

## Choosing a target (`Brain::ChooseTarget` 0x5D0030, `SelectCombatTarget`)

Every candidate is scored for every spell / attack the monster has (`AiSpellInfo_Score` 0x5CF9A0); the old target is kept while it is valid and nothing scores better; a target that cannot be reached or attacked is ignored for 20 ticks. Behaviours (`AiLogicGuard`, `Patrol`, `Sentry`, `Follow`) decide where the monster stands when it has no target.

**Checked live (2026-10-21, test rig, bot = Archer with +0x438 == 1, no order sent at all):** a monster (hp 11) came and bit the idle bot; the bot's own update showed an attack order on it within seconds (`k3`), the monster died after about 7 s and the bot lost 1 hp. The old um-bot fight loop (a move to 3 units before every attack, a new target every second) instead walked away from the monster biting it towards a farther one and died. So um-bot 0.1.24 sends no order while its hero attacks (its own field-10 mode 3) and adds only the host's defence, the aggressive pick, the leash and the follow. Not yet seen live: the host's defence and the follow during a fight (the host cannot be moved on the rig reliably: a poked host is not seen by the bot).

## What um-bot does with it

- It picks lone monsters before ones in a pack: every other hostile within 7 units of a candidate (the call-for-help radius) adds 5 to its distance.
- Its sight (13) is longer than the animals' (8.5): an archer can start a fight outside their sight; a melee fighter walking up from outside the cone is noticed later.
- A monster chases at most `FollowLimit` (15) units: leaving it behind works, so the bot's leash (25) is longer than the chase.
