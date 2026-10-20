# um-bot

A companion player for Evil Islands multiplayer. It joins the game as an ordinary player through its own
network client, with no game copy and no second game instance. In the game it follows the human player,
heals them and fights their targets. It levels up and keeps its own inventory like any player.

**Status: 0.1.17.** It joins a game like a player (handshake, login with the server's key, its character from the `.mp`),
enters the quest the host chose, follows the host at the host's pace, fights hostile monsters (walks to 3 units, orders
the attack, retries, ignores a refused target for 30 s), drinks the belt's healing potion below the set health, keeps
a leash, goes back to the lobby with the host (and enters the next quest), reconnects after a failure and saves its
progress (experience, items) into its `.mp` from the server's party messages. When its hero stays dead for 45 s it
leaves the world and enters it again with a new hero. Not done: looting its own corpse, several bots in one window.

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
  - the role: Melee, Ranged, Mage or Hybrid;
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
  checked against the running game and against saved characters): skill points for the role (Melee, Archery, the Astral / Elemental magic), the ability of the weapon it wields, and
  Health, Strength, Quickness, Lift and the like, chosen by worth per experience point, with the Damage <-> Defence slider weighing health. "Spend experience" turns it off.
- **Spells**: if it has no usable Healing spell, it wears one it carries, or buys the shop's Healing blueprint and makes the spell the way the camp's constructor does (spell object and container item, exactly
  the game's values, checked against saved characters and a live cast). It only takes a spell it can use: the magic skill, and the mana (cost x 25 / Intelligence against its maximum) - "Spend experience" raises the skill the spell needs first.
- **Mage / Hybrid**: they also make an attack spell (an instant elemental spell aimed at a unit: Fire Arrow, Lightning Arc, Acid Ray; at most a third of the money, and only one the character can use) and cast it at the
  enemy it has walked up to, when it has the mana for it besides the share kept for heals; if three casts in a row cost no mana it goes back to its weapon. Casting at enemies is checked only partly (the orders are
  accepted and cost mana; the damage was not measured).
- **Chat report** ("Report in chat"): at the camp it says in the game's chat what it did: `trained (2392 exp): Archery 1 -> 38, Strength1`, `made the spell Healing (200)`,
  `bought: stone short bow (silicon) for 240, ...`, `sold loot for N`, `repaired N item(s) for N`, `bought 2 healing potions`, `money left: N`.
- **Swaps** ("Accept swaps"): a player can click the bot's head in the lobby to swap; it accepts, takes the offered items (the loot it
  sells) and gives nothing.
- **Progress**: "Save progress" writes the character the server reports (experience, items, quest variables; sent
  every 15-60 s and when leaving the world) back into the bot's `.mp`. The first save keeps the original as `.bak`.
- **Movement**:
  - the pace: like the player (runs, walks, sneaks and crawls when they do), always run, or always walk;
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

## Building

```
make linux   # um-bot
make win     # um-bot.exe (MinGW)
make all
```

It uses um-multitool's vendored Dear ImGui and GLFW (`../um-multitool/vendor`). On Linux, GLFW picks X11 or
Wayland by itself.
