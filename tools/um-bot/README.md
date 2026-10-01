# um-bot

A companion player for Evil Islands multiplayer. It joins the game as an ordinary player through its own
network client, with no game copy and no second game instance. In the game it follows the human player,
heals them and fights their targets. It levels up and keeps its own inventory like any player.

**Status: skeleton (0.1).** The window and the settings work; the network client does not exist yet.

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
