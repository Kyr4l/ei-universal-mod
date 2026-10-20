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

## Perception (`AiMemory::ComputeThreatFactors` 0x5CB850, `StimulusMap`)

A monster knows a unit once a *factor* drops below 1; the factors are distance / (range x modifiers), kept at most 3, one per sense:
- **sight**: the other's sight range (+0x904: 8.5 for the animals and monsters seen, 13 for a player's hero; the perk "night vision" raises the hero's) x the looking unit's factor (+0x900) x the seen unit's visibility (+0x294) x the looker's (+0x2C4), and times the terrain's line of sight (`CAIMap::TraceVisibility`: trees and rocks hide). The view cone is narrow for animals (the UNITS command shows `fov=50`, a hero 75).
- **hearing**: footsteps (`StimulusMap::PerceiveFootsteps`) and sounds (`StimulusMap::HearSound`: a fight, a loot, a cast make noise with a radius of the unit's noise value x 1.5).
- **smell**: a third range (+0x2B4), used by animals.
A sneaking or crawling hero is harder to see and hear (its visibility and noise factors are smaller), a running one easier.

## Choosing a target (`Brain::ChooseTarget` 0x5D0030, `SelectCombatTarget`)

Every candidate is scored for every spell / attack the monster has (`AiSpellInfo_Score` 0x5CF9A0); the old target is kept while it is valid and nothing scores better; a target that cannot be reached or attacked is ignored for 20 ticks. Behaviours (`AiLogicGuard`, `Patrol`, `Sentry`, `Follow`) decide where the monster stands when it has no target.

## What um-bot does with it

- It picks lone monsters before ones in a pack: every other hostile within 7 units of a candidate (the call-for-help radius) adds 5 to its distance.
- Its sight (13) is longer than the animals' (8.5): an archer can start a fight outside their sight; a melee fighter walking up from outside the cone is noticed later.
- A monster chases at most `FollowLimit` (15) units: leaving it behind works, so the bot's leash (25) is longer than the chase.
