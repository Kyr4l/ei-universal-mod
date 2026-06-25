# SpellAddon – Detailed Configuration Guide

## General Information & Author Contacts

* **Community:** A group dedicated to a full storyline add-on for Evil Islands (EI): [vk.com/evil_islands_addon](https://vk.com/evil_islands_addon)
* **Author's Website:** [evilislandsaddon.forumotion.com](https://evilislandsaddon.forumotion.com/)
* **Co-author Discord (PlayHard_GoPRo):** `arrogant2818`
* **Support the Author:** You can donate to the PrivatBank card `5168745153252199`. Supporting the author is completely optional but significantly speeds up the development of new features, shops, spells, and upcoming storyline expansions.

---

## Configuration File Breakdown (`SPELLADDON.INI`)

This add-on allows you to customize various aspects of the game engine through the `SPELLADDON.INI` file.

### `[skills]`

* `extendedskills=0` *(1 = Enabled / 0 = Disabled)* – Modifies the maximum leveling cap for all skills **except** Sleight of Hand (Stealing).
* `skills=130` *(Integer)* – Sets the maximum level for non-stealing skills.

### `[stealing]`

* `extendedstealing=0` *(1 = Enabled / 0 = Disabled)* – Modifies the maximum leveling cap for Sleight of Hand (Stealing).
* `stealing=130` *(Integer)* – Sets the maximum level for the Sleight of Hand skill.

### `[gameplay]`

* `randomlessaimedhits=1` – Target selection behavior for mobs:
  * `0` = Disabled.
  * `1` = Mobs target more vulnerable body parts after "testing" armor with an initial hit to the legs.
  * `5` = Mobs immediately target the best body part based on maximum potential damage (**Recommended: 5**).
* `headmod=1.30` *(Float)* – Aim priority multiplier for the head.
* `legmod=0.90` *(Float)* – Aim priority multiplier for the legs.
* `handmod=1.90` *(Float)* – Aim priority multiplier for the arms.
* `criticalmod=2.50` *(Float)* – The damage threshold factor required to switch targeting priority to that specific limb.
* `healthdowntoignore=11.00` *(Float)* – How many times a limb's health must be lower before the engine forces a switch to another limb.
* `rangeddistance=5.50` *(Float)* – The distance (in meters) from which a unit will prioritize shooting at the target's legs.
* `rangedweaponbackstabs=1` *(1 = Enabled / 0 = Disabled)* – Enables backstab damage bonuses for ranged weapons.
* `newattackdefence=0` – Anti-random combat system. Changes how Attack and Defense stats interact:
  * `0` = Disabled (Vanilla behavior).
  * `1` = Attack and Defense only affect damage *after* armor deduction.
  * `2` = Same as `1`, but weapon weight and distance do not scale.
  * `3` = A more advanced system where Attack/Defense affect damage post-armor.
  * `4` = Same as `3`, but misses are entirely disabled.
* `divmod=100.0` *(Float)* – The value used to divide Attack minus Defense after subtracting `submod` (applicable in modes 1 or 2).
* `submod=1.0` *(Float)* – Base Attack/Defense value for modes 1 or 2, and the minimum damage multiplier for modes 3 or 4.
* `possessioncharmlink=1` *(1 = Enabled / 0 = Disabled)* – Unlocks high-quality, previously bugged, or unused spells that were restricted in the original game engine.
* `unlockmoney=1` *(1 = Enabled / 0 = Disabled)* – Decrypts the money value in the game's memory.
* `extendedparams=1` *(1 = Enabled / 0 = Disabled)* – Allows expanded attribute ranges (Strength, Dexterity, Intelligence) from 5 to 45 during multiplayer character creation.
* `manygameexe=1` *(1 = Enabled / 0 = Disabled)* – Allows launching multiple instances of the game process simultaneously.
* `longnick=1` *(1 = Enabled / 0 = Disabled)* – Extends the maximum multiplayer character nickname length from 10 to 28 characters.
* `fullexpforall=0` *(1 = Enabled / 0 = Disabled)* – Experience is no longer divided among party members; everyone receives the full amount.
* `enablekillsinmulti=0` *(1 = Enabled / 0 = Disabled)* – Disables anti-godmode player immortality, making PvP fully lethal in multiplayer.
* `enablecheatsinmulti=0` *(1 = Enabled / 0 = Disabled)* – Enables cheats in multiplayer mode.
* `aggrmod=4` – Threat/Aggro reset mechanic behavior at the engine level:
  * `0` = Disabled.
  * `2` = Dynamic randomized aggro drop with an exponential dependence on distance.
  * `3` = Aggro drop with a non-exponential dependence on distance.
* `aggrrange=0` – Unknown setting, do not modify.
* `losehardcore=35000` – Difficulty rating for dropping mob aggression.
* `losefactor1=10` *(Integer)* – Aggro loss stabilizer. Multiplies game speed and divides the time returned by `getTickCount` (Only active in Mode 4).
* `losefactor2=4` *(Integer)* – Aggro loss stabilizer. Divides the second time divisor by $2^{\text{losefactor2}}$ (Only active in Mode 4).
* `visiondependence=0.5` *(Float)* – The ratio beyond sight range required for a unit to lose line of sight on a target.
* `follow=0` – Mobs tracking behavior: `1` = Mobs stop chasing at the aggro drop point; `0` = Mobs chase indefinitely.
* `improve=1` – Core AI optimizations (**Recommended: 1**).
* `loseallyfast=1` – Speeds up aggro loss when an ally dies (**Recommended: 1**).
* `visionfix=1` *(1 = Enabled / 0 = Disabled)* – Fixes line-of-sight stability. When enabled, everything that intersects the sight radius is immediately lost if visibility conditions fail.
* `newspells=1` *(1 = Enabled / 0 = Disabled)* – Expands the spell system and enables new spells: *Ensorcell* (Charm - ID 42), *Inspiration* (Stamina restore - ID 43), *Summon Undead* (ID 45), and *Fire Sacrifice* (ID 49).
  > **Modding Note:** Variables passed via `GSGetVar` include: `S0e` (Spell effect), `S1d` (Duration), `S2t` (Spell ID), `S3x` (X coordinate), and `S4y` (Y coordinate). Check `AutoRunMobFile123.mob` for examples.
* `importantalerts=1` *(1 = Enabled / 0 = Disabled)* – Enables critical notification messages.
* `rewriteperssize=1` *(1 = Enabled / 0 = Disabled)* – Enables character model scaling in the shop interface.
* `perssize=0.8` *(Float)* – Visual size multiplier for characters in the shop.
* `extendedmerc=1` *(1 = Enabled / 0 = Disabled)* – Allows changing the maximum number of mercenaries you can hire.
* `maxcountofmerc=2` *(Integer)* – Maximum mercenary count.
* `saveall=1` *(1 = Enabled / 0 = Disabled)* – Allows seamless free-roam between Allods by saving map progression data instantly across all locations.
* `unbreakablebodyparts=1` *(1 = Enabled / 0 = Disabled)* – Disables limb crippling.
* `bodypartbreakmodifier=-5` *(Float)* – Determines how deeply limb durability can drop (acts as a damage multiplier to that body part).
* `autorunmobfile=1` *(1 = Enabled / 0 = Disabled)* – Automatically executes a special mob script (`AutoRunSpellAddon123.mob` inside the `Maps` folder) upon entering any map.
* `noruinszoneanimation=1` *(1 = Enabled / 0 = Disabled)* – Blocks characters on `zone1` from being affected by forced built-in animation scripts.
* `infinitydurability=1` *(1 = Enabled / 0 = Disabled)* – Completely disables item degradation and breaking.
* `nonstopmanaregen=1` *(1 = Enabled / 0 = Disabled)* – Ensures stamina/mana regenerates constantly under any state.
* `splashdamageonair=1` *(1 = Enabled / 0 = Disabled)* – Allows Area of Effect (AoE) offensive spells to strike flying units.
* `infiniteaddmobspawn=1` *(1 = Enabled / 0 = Disabled)* – Forces the `AddMob` script command to run endlessly.
* `batterycharge=0` *(1 = Enabled / 0 = Disabled)* – Prevents slotting spells with certain runes into wands.
* `shopschanges=1` *(1 = Enabled / 0 = Disabled)* – Enables free items via scripting commands `@GSSetVar( 0, "i.onitemconstr", 1 )` and `@GSSetVar( 0, "i.onspellconstr", 1 )`.
* `rewritenpcvisionarc=1` *(1 = Enabled / 0 = Disabled)* – Enables dynamic modification of field-of-view angles for characters.
* `visionarc=-2.5` *(Float)* – Sight angle in radians for all dynamic NPCs/mobs.
* `archersdefence=1` *(1 = Enabled / 0 = Disabled)* – Ensures Defense stats apply correctly to ranged units.
* `controlmagic=1` – Determines how crowd-control (CC) spells interact with targets:
  * `0` = Disabled.
  * `1` = CC success depends entirely on the target's resistances.
  * `3` = CC potency scales with the caster's Intelligence attribute.
  * `4` = CC potency scales with both the caster's Intelligence and Experience level.
* `slownessmultiplier=11.05` *(Float)* – Slow effect formula:
  $$\text{Final Effect} = \frac{(\text{Base Effect} \times \text{slownessmultiplier}) - \text{Resistance}}{\text{slownessmultiplier}}$$
* `weaknessmultiplier=0.55` *(Float)* – Weakness effect formula:
  $$\text{Final Effect} = \frac{(\text{Base Effect} \times \text{weaknessmultiplier}) - \text{Resistance}}{\text{weaknessmultiplier}}$$
* `durcoeff=0.04` *(Used for Mode 3)* – Determines how much the Intelligence gap between the caster and victim increases or decreases spell duration (percentage based).

#### Crowd Control (CC) Spell Classifications (`1` = Yes, `0` = No)

| Spell Flag Key | Value | Description |
| :--- | :---: | :--- |
| `prot_fire` / `prot_electro` / `prot_acid` | `0` | Elemental Protections (Not CC) |
| `eagle_sight` / `infravision` / `detect_life` / `invisibility` | `0` | Utility Vision/Stealth (Not CC) |
| `silence` / `lichdom` / `antimagic` | `0` | Specific debuffs/states (Not CC) |
| `strength` / `regeneration` / `speed` / `enlarge` / `shrink` | `0` | Buffs / Stat changes (Not CC) |
| `necro` / `destroy` | `0` | Direct damage/offensive (Not CC) |
| `stench` | `1` | Treated as Crowd Control |
| `stun` | `1` | Treated as Crowd Control |
| `weak` | `1` | Treated as Crowd Control |
| `feeblemind` | `1` | Treated as Crowd Control |
| `slow` | `1` | Treated as Crowd Control |

#### Base Un-runed Spell Effects (`b[spellname]`)

*These values provide flat, base adjustments to spells that cannot be upgraded via runes.*

* `beagle_sight=1.0` \| `binfravision=15.0` \| `bdetect_life=2.0` \| `binvisibility=15.0` \| `bsilence=15.0`
* `blichdom=15.0` \| `bstench=25.0` \| `bstrength=95.0` \| `bweak=25.0` \| `bregeneration=80.0`
* `bspeed=180.0` \| `bslow=180.0`
* *(All others default to `0.0`)*

---

### Additional Gameplay & UI Tweaks

* `showallquestsinmulti=1` *(1 = Enabled / 0 = Disabled)* – Instantly displays all quests in multiplayer.
* `infowindowschanges=1` *(1 = Enabled / 0 = Disabled)* – UI layout presets for an improved information window.
* `spellsaimod=1` *(1 = Enabled / 0 = Disabled)* – Smart spell management AI fixes.
* `spellsarealwaysconstructable=1` *(1 = Enabled / 0 = Disabled)* – Allows players to assemble and assign spells to heroes at any time.
* `diffchange=1` *(1 = Enabled / 0 = Disabled)* – Locks out the "Change Difficulty" button inside the in-game options menu.
* `versionalerts=1` *(1 = Enabled / 0 = Disabled)* – Notifies the player if outdated mod files are left in the game folders.
* `nogameoveronleadersdeath=1` *(1 = Enabled / 0 = Disabled)* – Prevents an instant "Game Over" screen if the main hero dies.
* `lastspellsdamagetype=1` *(1 = Enabled / 0 = Disabled)* – Overrides damage types for specific spells like Tka-Rik's Arrow, Curse's Lightning, and vanilla counter-parts.
  * *Damage Type IDs:* `0` = Piercing, `1` = Slashing, `2` = Bludgeoning, `3` = Fire, `4` = Acid, `5` = Lightning, `6` = General.
* `astralbeam=6` – Damage type for Astral Rift (Curse) [Set to General].
* `destroyray=6` – Damage type for Tka-Rik's disintegrating ray [Set to General].
* `lightningbeam=4` – Damage type for standard Lightning [Set to Acid].
* `firearrow=4` – Damage type for Fire Arrow [Set to Acid].
* `heroalwayscanrun=1` *(1 = Enabled / 0 = Disabled)* – The hero can run even when heavily over-encumbered.
* `npcsmana=1` *(1 = Enabled / 0 = Disabled)* – NPCs and enemies consume stamina/mana when casting spells.
* `showtype=1` *(1 = Enabled / 0 = Disabled)* – Displays movement type indicators within the information window.
* `speedboost=1` *(1 = Enabled / 0 = Disabled)* – Certain actions provide temporary movement speed boosts.
  * `speedbase=1.0` (Base speed multiplier)
  * `speedadd=1.0` (Speed addition modifier)
* `damageboost=1` *(1 = Enabled / 0 = Disabled)* – Scales damage based on core attributes prior to armor calculations.
  * *Formula impact:* Scaling strength per point = $\frac{1}{\text{attribute}}$. For example, with `str=50`, `dex=999999`, `int=200`: Each point of Strength over 25 grants +2% damage, Dexterity scales almost no damage, and each point of Intelligence increases spell potency by +0.5%.
* `changeweightrangebonuses=1` *(1 = Enabled / 0 = Disabled)* – Changes how weapon weight and distance affect hit chance (enhanced compatibility when `newattackdefence=1`).
  * `weight=20.0` (How positively weight influences hit chance)
  * `range=10.0` (How negatively distance reduces maximum accuracy cap—e.g., down to 90%)
* `spellsprioritys=1` – AI targeting logic adjustments for spellcasters.
  * `sametarget=10.0` (Desire factor to focus fire on a single target)
  * `magicattack=3.0` (Flat bias added to all offensive magic logic)
  * `damagemult=1.0` (Melee/Weapon attack execution priority)
  * `magicdamagemult=1.3` (Spells execution priority)
  * `savedamagemult=3.0` (Healing spells priority)
  * `rangecost=25.0` (AI penalty calculation per meter of casting distance)
  * `kickprice=2.0` (Prioritizes targets that are killing units quickly)
* `changeexpforint=1` *(1 = Enabled / 0 = Disabled)* – Adjusts the experience multiplier granted by Intelligence.
  * `expforint=1.01` (Multiplier per point of Int. Vanilla was 1.04).
* `copystatscut=0` *(1 = Enabled / 0 = Disabled)* – When cloning units using `CopyStats`, setting this to 1 retains the target's original racial traits instead of a perfect duplicate.
* `removenpcsblock=1` – Strips standard NPC safety restrictions, forcing them to behave aggressive like monsters.
* `strengthstats=1` – Enables adjustments to the Strength attribute's scaling coefficients.
  * `strhp=0.001` (HP scaling per Strength. Vanilla: 0.005)
  * `strdmg=0.001` (Damage scaling per Strength. Vanilla: 0.003)
  * `strweight=10.50` (Carry weight scaling per Strength. Vanilla: 12.0)
* `expirementalfxes=1` *(1 = Enabled / 0 = Disabled)* – Patches an engine-level bugged function to eliminate frequent CTDs (Crash to Desktop).

---

### Economy & Dynamic Restocking

* `shopsnewnormalchanges=1` *(1 = Enabled / 0 = Disabled)* – Enables custom item stock minimums/maximums and sell-back pricing modifiers across maps.
* **Stock Constraints:**
  * Weapons: `minsweapons=5` to `maxsweapons=76`
  * Light Armor: `minsarmorslight=15` to `maxsarmorslight=76`
  * Heavy Armor: `minsarmorsheavy=65` to `maxsarmorsheavy=76`
  * Potions: `minspotions=25` to `maxspotions=76`
  * Prototypes: `minsprototypes=35` to `maxsprototypes=76`
  * Runes: `minsrunes=54` to `maxsrunes=76`
* **Allod Vendor Sell Price Multipliers (Percentage of item value returned to player):**
  * Gipat (Babur): `gipatb=0.0` (0% - Items cannot be sold to Babur)
  * Gipat (Witch): `gipatz=0.95` (95%)
  * Ingos: `ingos=0.1` (10%)
  * Suslanger: `suslanger=0.2` (20%)
  * Jigran (Caves): `jigran=0.45` (45%)

---

### Advanced Mechanics Alterations

* `liftchanges=1` *(1 = Enabled / 0 = Disabled)* – Adjusts enemy carry capacities to scale proportionally with their Strength score.
* `liftdebuffs=1` *(1 = Enabled / 0 = Disabled)* – Encumbrance status applies severe attribute debuffs.
  * `liftkey=4.5` (Intensity factor of the encumbrance debuff)
* `everyoneishero=2` – Changes unit engine flags: `2` = Forces all entities in the game to be classified as "Heroes".
* `improvedstealing=1` *(1 = Enabled / 0 = Disabled)* – Segregates normal corpse looting from live pickpocketing tables. Picks items directly from `Items` configuration lists.
* `teleportboost=1` – Teleport speed adjustments:
  * `0` = Disabled.
  * `1` = Speed up casting transition using effect runes.
  * `2` = Instant teleportation.
* `standtrans=1` – The script command `Stand` additionally revitalizes/wakes up the targeted unit.
* `possessionpatch=1` – The *Possession* spell no longer blinds or strips the vision away from the original spellcaster.
* `waslootedtrans=2` – Expands the `WasLooted` command arguments to return specific status bit-flags:
  * `28` = Under Feeblemind effect
  * `24` = Stunned
  * `20` = Pickpocketed
  * `19` = Knocked out
  * `15` = Moving
* `castscripttrans=1` – Modifies script tracking; `Cast` now accepts a second argument indicating the specific Spell ID used.
* `armorspell=1` – *In development (Currently non-functional)*.
* `newrangedweapontypes=1` – Allows non-bow/crossbow projectile weapons to fire freely at airborne units (Specifically integrated for the *Jamais Vu* mod).
* `inspirationintelligence=1` *(1 = Enabled / 0 = Disabled)* – Smart logic for *Inspiration*. Functions like healing spells; it will refuse to cast if the target's stamina/mana is already at 100%.
* `lightsboost=1` – Enables game illumination engine overwrites.
  * `newlightsvalue=0.01` (Global ambient light intensity)
* `shrinkenlargerebalance=1` *(1 = Enabled / 0 = Disabled)* – Rebalances *Shrink* and *Enlarge* spells so they adjust stats **without changing character model dimensions**.
* `hideobjecttrans=1` – *In development (Experimental)*.
* `hidefireworks=1` – Makes spell-based fireworks/particle explosions completely invisible.
* `wallsfrequency=1` – Changes fire rate/tick speed for Wall-type spells.
  * `frequencyvalue=15` (Wall spell triggers damage once every 15 engine ticks).
* `wallspercentage=1` – Adjusts damage tick percentages for Wall elements:
  * Fire Wall: `firepercentagevalue=0.2` (20% damage per tick)
  * Lightning Wall: `lightningpercentagevalue=0.3333...` (33.3% damage per tick)
  * Acid Wall: `acidpercentagevalue=0.1428...` (14.2% damage per tick)
* `acidattack=0` – *In development (Currently non-functional)*.
* `newperks=1` *(1 = Enabled / 0 = Disabled)* – Injects brand new perks into the engine: *Fortitude* (Стойкость) and *Ghostliness* (Призрачность).
* `dmgtypes=1` – Customizes UI damage display elements inside the Camp interface (Left-hand panel).
  * `dmgtypescount=7` (Displays up to 7 distinct damage types).
* `avgarmors=1` *(1 = Enabled / 0 = Disabled)* – Displays **Average Armor** ratings inside the Camp's left panel rather than Max Armor.
* `itemslightchanges=1` *(1 = Enabled / 0 = Disabled)* – Modifies the glowing outline colors for enchanted gear via slots `light00` through `light31`.
* `offensivespells=1` *(1 = Enabled / 0 = Disabled)* – *Stench* and *Shrink* are no longer flagged as hostile actions by default.
* `nhits=1` – Enables specific combat tracking variables required for animations in *Jamais Vu*.
* `effectredraws=1` – Adds a `+` modifier to active effects, driven by baseline config structures (`b[spellname]`).
* `npcsspellcastingtactics=1` *(1 = Enabled / 0 = Disabled)* – Enhances AI spellcaster tactics: Mobs draw weapons to attack when out of mana, and switch back to spellcasting once their reserves replenish.
* `vampirism=0` – *Experimental test feature (Do not modify)*.
* `stenchremaster=1` – *Stench* alters stealth mechanics; affected units become significantly easier to see and hear.
  * `stenchmul=0.1` (Stench profile visibility modifier).
* `regenerationremaster=1` – Overhauls natural health regeneration. Adds the spell's core potency multiplied by `regenerationmul` directly to the unit's native tick rate.
  * `regenerationmul=0.1`
* `speedslowremaster=2` – Changes *Speed* & *Slow* spell behavior:
  * `0` = Disabled.
  * `1` = Spell effect is flatly multiplied by `speedslowmul`.
  * `2` = *Speed* & *Slow* scale character movement dynamically when `encumbrancemovespeed=1` is active.
  * `speedslowmul=0.1` \| `defaultspeedmul=1.0` (Base) \| `minimumspeedmul=0.5` (Hard floor speed cap).
* `infowindowremake=1` *(1 = Enabled / 0 = Disabled)* – Compacts Attack and Defense readouts in the UI into a single streamlined row separated by 4 spaces.
* `actionspatch=1` – Allows overwriting the core action speed calculation formula.
  * Default Formula: $\text{Actions} = \text{Dexterity} \times 0.2 + 10$
  * `actionsmul=0.2` (Multiplier modification)
  * `actionsadd=10.0` (Flat addition modification)
* `mainmenuweather=0` – Forces weather states on the Main Menu screen: `0` = Off, `1` = Rain, `2` = Snow.
* `visioninweather=1` – Weather dynamically impairs sight distance.
  * `visionweathermul=0.95` (Vision range multiplier during poor weather).
* `allodsinfo=1` – Displays localized Allod text blurbs (`adesc [Allod Name]` fetched from `texts.res`) inside a custom UI panel bounding box.
  * `framex=340` (X axis resolution frame dimensions)
  * `framey=340` (Y axis resolution frame dimensions)
* `allowheroesarmors=1` *(1 = Enabled / 0 = Disabled)* – Heroes possess a baseline, non-reducible natural armor rating originating from their body stats.
* `encumbrancemovespeed=1` – Encumbrance penalizes movement speed based on weight.
  * Formula:
    $$\text{Penalty} = \left(\frac{\text{Current Weight}}{\text{Current Weight} + \text{Max Weight}}\right)^3 \times \text{encumbrancemovemul (1.100)}$$
* `encumbranceattackspeed=2` – Encumbrance penalizes combat attack speeds.
  * Formula:
    $$\text{Penalty} = \left(\frac{\text{Current Weight}}{\text{Current Weight} + \text{Max Weight}}\right)^3 \times \text{encumbranceattackmul (1.100)}$$
* `speedslowmovemul=0.2` – Degree to which *Speed* & *Slow* debuffs interact with encumbered movement speeds.
* `notrash=1` *(1 = Enabled / 0 = Disabled)* – Optimizes memory; removes the simultaneous requirement for both `s` & `t` subfolders inside the `speech.res` archive.

---

### Memory Offsets and Tracking Data

#### `hptrans=1` (HP/Stat Tracking Offsets)

When enabled, returns the following raw entity states:

* `+10` HP Current \| `+11` HP Max \| `+12` HP Regeneration
* `+13` MP Current \| `+14` MP Max \| `+15` MP Regeneration
* **Absorptions/Resistances:** `+16` Piercing \| `+17` Slashing \| `+18` Bludgeoning \| `+19` Thermal \| `+20` Chemical \| `+21` Electric \| `+22` General

#### `mptrans=1` (Extended Stat Tracking Offsets)

Maps various internal attributes, limb parameters, and visibility modifiers (Offsets span from `+1` total experience, attribute totals, tracking metrics, down to individual limb breakdown stats such as `+125 head.hp.curr`, `+186 body.hp.curr`, `+308 rhand.hp.curr`, and `+430 rleg.hp.curr`).

---

### Engine Systems Mapping

* `camp=2` – Camp mapping shortcuts: `0` = Disabled, `1` = Enters via script `@SendStringEvent( 0, "constr" )`, `2` = Enters via script **or** by pressing the `Home` key.
* `showmanaforeveryone=1` *(1 = Enabled / 0 = Disabled)* – Enemy stamina/mana pools are visible on their UI plates just like player characters.
* `meterscale=1` – Modifies UI measurement indicators.
  * `meter=19.0` (Measurement scaling factor).
* `newplayersafetyconditions=1` – Changes AI awareness ranges:
  * `safetymuldist=2.0` (Distance modifier for sensing hostile entities).
  * `maxvisibilitymul=3.0` (AI direct reaction distance multiplier).
* `genocideincvars=10` – Aggression counters tracking: `Hits` grows from hitting enemies (Fetched via `GSGetVar(0, "Hits")`).
* `genocidedipvars=3` – Sets variables: `Hits` tracks the diplomatic group of the last struck target; `Kils` tracks the last killed target.
* `dontmergegipathzones=1` *(1 = Enabled / 0 = Disabled)* – Prevents isolated Gipat map zones from joining together.
* `backstabsforall=1` *(1 = Enabled / 0 = Disabled)* – Extends backstabbing capabilities to all entities, including standard map monsters.
* `sizerebalance=1` *(1 = Enabled / 0 = Disabled)* – Item weights dynamically scale how loud a character is while moving, affecting stealth loops.

---

### `[saves]`

* `savecontrol=1` *(1 = Enabled / 0 = Disabled)* – Allows scripts to explicitly toggle saving permissions. `GSSetVar(0, "Save", 0)` enables saving (Default), while `GSSetVar(0, "Save", 1)` locks saving mechanics out.

### `[speed]`

* `allowmpspeed=1` *(1 = Enabled / 0 = Disabled)* – Unlocks fast-forward speeds during multiplayer games.
* `rewritespeed=0` – Allows custom engine game speeds mapped to mouse clicks.
  * `normal=55` \| `fast=27` (Values proportional to game slow-down metrics).
* `rewritespeedkey=0` – Enables keyboard-mapped speed steps.
  * `normalkey=1` \| `fastkey=99`
* `rewriteotherspeed=1` – Enforces default speeds upon execution triggers (e.g., loading a fresh save game file).
  * `normal1=18` \| `normal2=18` \| `fast2=37`
* `disablepause=0` – Replaces standard game pauses with an ultra-slow game speed alternate.
* `gsgetvar=1` & `gssetvar=1` – Allows fetching or writing game speeds globally if characters 4-7 of a global string match `"peed"`, or fetching difficulty if set to `"Diff"`.
* `pause=25` \| `pausekey=56` – Fine-tunes pause slow-down speeds.

### `[rebalanceengine]`

* `useonlymindamage=1` *(1 = Enabled / 0 = Disabled)* – Disables the maximum damage roll ceiling for mobs; they will only roll their minimum base damage values.
* `rewriteherolevelcounting=1` *(1 = Enabled / 0 = Disabled)* – Overrides mob level formulas:
  $$\text{Level} = \text{mullvla} \times \log_2\left(\text{addlvla} + \frac{\text{Experience}}{\text{divlvla}}\right) + \text{addlvlb}$$
  * *Values:* `mullvla=2.94`, `divlvla=295.0`, `addlvla=1.00`, `addlvlb=1.00`
* `rewriteskillscounting=0` – Custom skill costing formulas: $\text{Cost} = (\text{pow}^{\text{lvl}}) \times \text{mul} + \text{add}$.
* `rewriterunabilitytime=1` – Activates custom constants for relative stamina draining while sprinting. 0=(nowMP=nowMP-maxMP*runconst) 1=(nowMP=nowMP-runconst) 2=(nowMP=nowMP-maxMP*runconst-runconstex)
* `newrunsystem=0` – Switches to an *absolute* stamina drain format, completely overriding relative modes.
  * `runconst=0.001666...` (Stamina cost calculation step per frame).
* `backstabvisualbase=1` *(1 = Enabled / 0 = Disabled)* – Modifies how backstab numbers look within description texts.
  * `bsbase=221` (Forces 221% backstab damage visualization within character perks panels).
* `meleesum_low=1` & `meleesum_2=1` – Engine recalculation structures for Attack and Defense math. *(Note: Avoid enabling `meleesum_1=1` as it is unstable).*

### `[animationsspeed]`

* `rewriteactionsspeed=1` *(1 = Enabled / 0 = Disabled)* – Overrides global skeletal animation speed rates.
  * `attackspeed=1` (Attacks) \| `castspeed=1` (Spells) \| `hitspeed=1` (Flinching) \| `deathspeed=1` (Ragdoll physics decay) \| `specialspeed=1` (Special actions) \| `idlespeed=1` (Resting state loops).

### `[fonts]`

* `changefont=1` *(1 = Enabled / 0 = Disabled)* – Replaces native font mappings.
  * `newfont1=Arial` \| `newfont2=Times New Roman` \| `newfont3=Arial` \| `newfont4=Arial`

### `[speedrun]`

* `timervars=3` – Speedrun timer implementation. Freezes automatically during screen loads.
  * Key mapping and positioning presets: `starthk=57` (Start), `pausehk=F6` (Pause), `resethk=48` (Reset), `windowx=1082`, `windowy=751`, `opacity=255`.

---

## Installation Instructions

### Compatibility Constraints
>
> ⚠️ **Important Starter Rules:**
>
> * If you run the game alongside the **Standard Starter**, you must disable the extended skills limit: set `extendedskills=0`.
> * If you run the game alongside the **Newest Starter**, you must additionally turn off animations rewriting (`rewriteactionsspeed=0`) and turn off extended stealing limits (`extendedstealing=0`).

### Clean Setup (Manual Installation)

1. Navigate to your game folder and remove any legacy files from older releases of `SpellAddon` found in your primary directory or the Starter's `Engine` directory.
2. Extract and drop `SpellAddonX.asi`, `SpellAddonInfo.txt`, and `SPELLADDON.INI` directly into your main game directory.
3. Move `AutoRunSpellAddon123.mob` into the `Maps` folder inside your game directory.
4. **Starter Check:** When playing with the absolute newest EI Starter, copy and duplicate `SpellAddonX.asi`, `SpellAddonInfo.txt`, and `SPELLADDON.INI` into the **`Engine`** folder of your starter layout if initialization errors occur.

### Mod-Specific Configurations

* Setting a completely clean, default config inside the primary directory prevents SpellAddon from executing changes on standard vanilla game states or overarching cross-mod files.
* To configure SpellAddon to trigger **only on a specific mod**, move your customized `SPELLADDON.INI` configuration profile inside that specific mod's folder layout.
* Custom scripts are supported uniquely per mod; place any tailored version of `AutoRunSpellAddon123.mob` within that specific mod's `Maps` directory.
