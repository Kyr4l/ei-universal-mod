# um-bot

A companion player for Evil Islands multiplayer. It joins the game as an ordinary player through its own
network client, with no game copy and no second game instance. In the game it follows the human player,
heals them and fights their targets. It levels up and keeps its own inventory like any player.

**Status: 0.4.** It joins a game like a player: it asks the server for its info, logs in with the key the server
gives, then sends its character (name and unit, from the `.mp`). The host's chat announces it and its face shows
in the lobby (tested on a hosted lobby). Playing in the game (moving, fighting) comes next.

**Its character:** the bot needs a multiplayer character file (`.mp`, from the game's or the mod's `mp` folder).
A fresh character is recommended. Its name and clan tag are the bot's ("Kevina | BOT") and both can be changed in the window.
The change is saved into the `.mp`, and the original is kept as `.bak`.

The window shows the character read only: its experience, money, attributes, skills, abilities and equipment (item
names from the mod's database, `<mp folder>/../res/databaselmp.res`). Health and mana are computed by the game.

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
