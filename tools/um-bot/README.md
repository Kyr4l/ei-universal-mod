# um-bot

A companion player for Evil Islands multiplayer. It joins the game as an ordinary player through its own
network client, with no game copy and no second game instance. In the game it follows the human player,
heals them and fights their targets. It levels up and keeps its own inventory like any player.

**Status: 0.1.31.** It joins a game like a player (handshake, login with the server's key, its character from the `.mp`),
enters the quest the host chose, follows the host with the game's own follow (the server walks it after the host, wherever the host stands; the bot takes over only for the leash) at the host's stance, fights on top of the game's own fighting logic (a hero without orders defends itself and keeps its target, as for any player: the bot
gives no order while its hero is attacking, helps the host against the units attacking it, orders attacks from where it stands - the server walks a
bow or a sword into range - and only walks closer when an order is refused twice; a target refused eight times is ignored for 30 s), drinks the belt's healing potion below the set health, keeps
a leash, goes back to the lobby with the host (and enters the next quest), reconnects after a failure and saves its
progress (experience, items) into its `.mp` from the server's party messages. When its hero dies it asks the server for a new one on the same connection (6 s later; the new hero appears at the arrival point and the host shows it normally), then walks to its body and loots it when no known enemy is within 20 units of it (three tries per hero, up to 5 minutes); if the respawn fails it leaves the world and enters again after 45 s. The host cannot leave a quest zone while other players are still in it, so the bot leaves first: it knows the zone's exits from the server's first message (their rectangles), and when the host has stayed 2.5 s in an exit it came into from outside it leaves the zone itself (the final save, then back to the lobby; setting `LEAVE_AT_EXIT`, "Leave first at an exit"). The host can also type `bot leave` (or `<the bot's first name> leave`) in the chat. Not done: several bots in one window.

**Its character:** the bot needs a multiplayer character file (`.mp`, from the game's or the mod's `mp` folder).
A fresh character is recommended. Its name and clan tag are the bot's ("Kevina | BOT") and both can be changed in the window.
The change is saved into the `.mp`, and the original is kept as `.bak`.

**Experience:** in a multiplayer session the game splits every kill's experience among all the players by the square root of their experience totals, whoever struck the blow. A bot with a few hundred experience next to a host with millions gets a fraction of a percent; next to a host of about its own level it gets about half. Pick the bot's character accordingly.

The window shows the character read only: its experience, money, attributes, skills, abilities and equipment (item
names from the mod's database, `<mp folder>/../res/databaselmp.res`). Health and mana are computed by the game.

**Several bots:** run one um-bot per character, each from its own folder (its own `um-bot.cfg` and character). Two bots joined the same quest at once in the tests.

`um-bot --connect-test` joins the configured server with the configured character, without the window, prints what
happens, and leaves (exit code 0 when the server listed the bot).

## Installing (Linux)

`um-bot --install-desktop` adds it to the application menu with its icon (the Sacred flower, the quest item
`driadidol00`); `--install-desktop --remove` takes it out. The icons are made by `assets/make_icons.py` from
`assets/logo-source.png`; on Windows the .exe carries the icon.

## Settings

The settings are saved in `um-bot.cfg` next to the program.

- **Connection**: the server (the hosting player's computer), its UDP port (8888 by default) and the bot's
  name.
- **Build**:
  - the build: Fighter (melee, damage first), Tank (melee, armour and health first) or Archer; there is no mage build, but every build keeps a Healing spell (see Spells);
  - a Damage ↔ Defence slider, which decides where its skill points and gear go.
- **Behaviour**:
  - the priority: keep the player alive, focus the player's target, or guard the player;
  - when to heal the player and itself;
  - the mana it keeps for heals;
  - potions;
  - engagement: Passive, Defensive or Aggressive.
- **Chat commands**: say them in the game's chat, after "bot" or the bot's first name (`bot stay`, `Kevina, follow`):
  `stay` / `wait` / `hold` (stand where it is), `follow` / `come`, `passive` / `defensive` / `aggressive` (the
  engagement), `reset` (back to the settings).
- **Database**: the game's or the mod's `databaselmp.res` / `database.res` (the "Database..." button; empty: it looks in
  the `res` folder next to the `mp` folder). It gives the item names and tells the bot which belt potion heals.
  The bot drinks that potion when its health falls below the setting, and uses it on the host when the host's health
  falls below "Heal the player below" (not verified live: the host was never hurt in the tests). The Healing spell is
  used first when the character has a spell container with it in its spell items, enough Astral Magic and mana
  (verified on itself: 5 -> 38 health in 0.8 s).
- **Shop and gear**: before a quest the bot visits the camp's shop of the allod the session is on (Gipath = shop 1 Babur,
  Ingos 3, Suslanger 4, Jigran 5; found from the session's quest list). It draws the stock the way the game does (every
  weapon and armor prototype of that shop, one material each, potions in quantities of 20-50), sells the loot it was
  given and the gear it replaces (price x 0.5, minus the wear), and buys and wears what its role and the Damage <-> Defence
  slider call for, never above its money or its lift capacity, and repairs what it still wears (price x 0.2 x the wear, the camp's own rule; all four multiplayer shops use the same
  price table: buy 1.0, sell 0.5, repair 0.2). "Gear target" lists the gear it builds first and keeps
  (`bone long bow:dragon red bones; gipat highest plate`); the shop's materials are drawn anew every session, so it saves
  the money for a wished item the shop does not show yet. Items are built with the game's own arithmetic (checked against
  the live shop item for item) and the server accepts them byte for byte.
- **Experience**: before a quest (before the shop) it spends its free experience like a player in the camp's skills window, at the game's own prices (the skill and ability costs were
  checked against the running game and against saved characters): skill points for the build (Melee or Archery, and some Astral Magic for the Healing spell), the ability of the weapon it wields, and
  Health, Strength, Quickness, Lift and the like, chosen by worth per experience point, with the Damage <-> Defence slider weighing health. "Spend experience" turns it off.
- **Spells**: if it has no usable Healing spell, it wears one it carries, or buys the shop's Healing blueprint and makes the spell the way the camp's constructor does (spell object and container item, exactly
  the game's values, checked against saved characters and a live cast). It only takes a spell it can use: the magic skill, and the mana (cost x 25 / Intelligence against its maximum) - "Spend experience" raises the skill the spell needs first.
- **Healing upgrade**: when it has the Healing spell it also looks at making a stronger one: the same blueprint with "effect" runes (each adds to the heal and to the mana cost and the skill needed), the best mix of up to
  six runes (effect runes, and mana runes that take back the mana the effect runes add) it can use (castable at least twice from a full mana pool, within half of its money, at least 25 % stronger than the worn one). The old spell is replaced, and "Spend experience" raises the Astral skill the better spell needs.
- **Swaps** ("Accept swaps"): a player can click the bot's head in the lobby to swap; it accepts, takes the offered items (the loot it
  sells) and the money offered (checked: 500 -> 520 for an offer of 20), and gives nothing.
- **Progress**: "Save progress" writes the character the server reports (experience, items, quest variables; sent
  every 15-60 s and when leaving the world) back into the bot's `.mp`. The first save keeps the original as `.bak`.
- **Movement**:
  - the pace: like the player (walks, sneaks and crawls when they do, also when the player stands still while changing it), always run, or always walk;
  - how far behind the player it follows;
  - the leash: past that distance from the player, it drops the fight and comes back.

## Plan

0. **Its character: the `.mp` file** (`<game>/mp/N.mp`, about 1 KB, compressed; the header holds two sizes(?) and
   a checksum or key(?)). The bot joins with it and saves its progress in it. To decode from game.exe (the
   decompression first), with the game's and the mods' files as samples.
1. **Record the protocol.** um.dll logs the game's `sendto` / `recvfrom` (the game uses plain UDP through
   WSOCK32, not DirectPlay) while a real client joins a hosted game and plays.
2. **Decode it** from the captures and game.exe: the join handshake, keep-alives, the world and unit updates,
   and the orders (move, attack, cast, weapon, stance; see `docs/game-memory.md`).
3. **Probe client**: joins, stays connected, moves.
4. **The bot**: the decision loop over the settings above, and its state shown in the Status panel.

The bot picks lone monsters before packs (a hit monster calls its friends within 7 units): see `docs/monster-ai.md` for what the game's AI does.

## Tests

`make test` (in `tools/um-bot`) checks offline the rules that were verified in the running game: the skill and ability prices, the spell constructor against the saved characters in `Universal-Mod/mp`, the compressed upload, the shop stock, the mana formula and the settings. 44 checks.

## Building

```
make linux   # um-bot
make win     # um-bot.exe (MinGW)
make all
```

It uses um-multitool's vendored Dear ImGui and GLFW (`../um-multitool/vendor`). On Linux, GLFW picks X11 or
Wayland by itself.

## The log file

`um-bot.log` is written next to the program (the previous run's log is kept as `um-bot.log.1`): every line of the window's log with the clock time, each order the bot sends with the reason (`order: move to ... [follow the host]`, `order: attack unit ... [fight: target ...]`), what the SERVER says our hero is doing (`server order on our hero: ...`, so an order the bot did not send shows too), and every 3 s in a fight a `state:` line with the positions and the distance to the host. Send it with a bug report. If shopping is skipped it says why (`shop skipped: ...`), and a shop visit that buys nothing lists its reasons.

