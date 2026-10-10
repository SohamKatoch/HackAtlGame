# Neon Grid — Game Design

This document describes what Neon Grid is supposed to be. Implementation is in [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md), [MULTIPLAYER_SPEC.md](MULTIPLAYER_SPEC.md), [AI_SPEC.md](AI_SPEC.md), and [VEHICLE_SPEC.md](VEHICLE_SPEC.md). Milestones are in [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md). Architectural choices are in [DECISIONS.md](DECISIONS.md).

The authoritative original text is [Reference/ORIGINAL_GAME_GUIDE.md](Reference/ORIGINAL_GAME_GUIDE.md), copied from `NEON GRID v2.txt`. If this document and the guide disagree, the guide wins and the conflict is recorded in [DECISIONS.md](DECISIONS.md).

Numbers below are **tuning targets** from the guide. They are starting values, not locked balance.

---

## 1. Identity

Neon Grid is a competitive arcade arena game: futuristic combat racing from a slightly angled top-down 3D view.

Players control small, fast, low-profile vehicles that never stop creating glowing energy trails. The trail is an obstacle. As the arena fills, open space shrinks and maneuvers get more dangerous.

It is inspired by futuristic light-cycle and trail-combat games. It is not a copy of a specific film or game, and it does not use those assets.

It is not a lap race. There is no racing line, lap count, finish line, or braking model. Skill is movement, trail placement, positioning, and timed powerups. Match progression is rounds and a first-to-N score.

**Tagline:** Move fast. Control space. Outmaneuver everyone.

**Platform:** PC.

**Players:** 1–4.

**Multiplayer:** LAN. Each human uses their own device. Shared-screen play is out of scope (see section 4).

**Offline:** Single-player versus AI, using the same match rules. AI fills empty slots.

**Online:** Not part of v2. The architecture must not block a later online transport. See [MULTIPLAYER_SPEC.md](MULTIPLAYER_SPEC.md).

---

## 2. Design pillars

1. **Movement is the game.** Driving must feel excellent before powerups, progression, or extra content.
2. **Continuous steering.** Heading is an angle, not four locked directions.
3. **Trails control space.** The primary weapon is where the trail is placed.
4. **Powerups create opportunities.** They open decisions. They do not replace trail skill.
5. **Short, replayable matches.** A match is easy to restart.
6. **LAN first.** Separate PCs on one network must be able to play a reliable match.

Feature filter: if a feature does not make movement, positioning, trail control, or multiplayer more interesting, it belongs in the backlog.

Priority order:

1. Movement
2. Trail control
3. Positioning
4. Powerups
5. Advanced strategy

---

## 3. Core rule and loop

The game should be understandable immediately: **keep moving, control space, do not hit a trail.**

The player is always doing several of these at once:

- Driving
- Creating territory
- Avoiding existing trails
- Cutting off opponents
- Managing speed
- Collecting powerups
- Deciding when to take risks

Loop:

1. Start the game.
2. Select Offline or LAN.
3. Lobby.
4. Select vehicle color.
5. Match start.
6. Accelerate (MVP may be constant cruise speed; see [VEHICLE_SPEC.md](VEHICLE_SPEC.md)).
7. Steer.
8. Create a trail.
9. Control space.
10. Collect powerups.
11. Outmaneuver opponents.
12. Collision.
13. Elimination.
14. Last player alive.
15. Round win.
16. Next round.
17. Match victory.

---

## 4. Who plays, and how

### Offline solo

One human versus AI. Example: player versus 3 AI.

AI uses the same movement and trail rules as humans. Solo is not a separate game. It is the same match with AI filling slots. See [AI_SPEC.md](AI_SPEC.md).

### LAN

One player hosts. Others join from their own PCs on the same local network. Players do not share one screen. The host runs the authoritative match. See [MULTIPLAYER_SPEC.md](MULTIPLAYER_SPEC.md).

### Not supported

Do not implement:

- Multiple keyboard players on one PC
- Multiple controllers controlling different players on one PC
- Split-screen
- Shared-screen local multiplayer

Every human in multiplayer uses their own device. This keeps input simple and keeps the focus on LAN and responsive control.

### Future online

Internet multiplayer is not in the initial game. Offline, LAN, and a future online mode should be able to share the same gameplay-facing network interface.

---

## 5. Modes

### Survival (default, v2)

Last player alive wins the round.

### Future modes (do not build until Survival matches work)

- **Territory.** Score from arena space controlled.
- **Team Battle.** 2v2. Teams share colors and may treat allied trails differently.
- **Time Attack.** Score from survival time.
- **Elimination.** Players are eliminated permanently across multiple rounds.

Host settings must not expose modes, arenas, or options that are not implemented.

---

## 6. Movement (player experience)

Full handling, input, and tuning live in [VEHICLE_SPEC.md](VEHICLE_SPEC.md). The player-facing rules are:

- The vehicle has a continuous heading (for example 73 degrees), not North/East/South/West.
- It moves continuously along that heading: direction times speed.
- Steering changes heading at a turn rate. Holding steer produces a smooth curve. Small corrections, medium turns, sharp turns, wide arcs, and aggressive hairpins are all valid.
- A near-90-degree turn is possible by holding steer. It is not mandatory.
- An instant 180-degree reversal is not allowed. A reversal needs turn rate, speed, and space. Players must not steer-flip out of a fatal trap.
- Faster speed means a larger turn radius. Boost makes the radius significantly larger. Speed is power; maneuverability is the cost.
- The body visually leans into the turn, then returns to neutral. Lean is cosmetic. It does not change collision.
- MVP speed may be a constant cruise. A later model may add minimum speed, acceleration, cruise, and boost as a curve. Boost and Overdrive still temporarily change speed before that curve exists.

The vehicle should read clearly from the camera: small, fast-looking, low, color-distinct, heading obvious.

---

## 7. Trails

Every vehicle continuously leaves an energy trail. The trail stays after the vehicle passes. It is both a visual effect and a physical obstacle.

### Data the player experience depends on

Each segment has a start, an end, an owner, and a creation tick.

Possible later properties (not required for the first playable build): energy state, powerup effect, expiration time.

### Look versus collision

Rendering and collision are separate.

- Collision is invisible mathematical geometry.
- Rendering is a glowing mesh that may have glow, bloom, particles, animation, and thickness.

Visual thickness and effects must not change who dies. Effects must not hide collision geometry.

### What kills a vehicle

The vehicle is eliminated if it hits:

- The arena boundary
- Any active trail
- Its own trail
- Another vehicle

Powerups may temporarily change these rules. See section 9.

---

## 8. Arena

### The Grid (initial arena)

A large rectangle. Spawn layout places players toward the corners, facing inward (P1 and P3 moving right, P2 and P4 moving left in the guide diagram).

Implemented size: 14000 × 9000 cm (D23), after the first 8000 × 5000 version was judged too small for four vehicles.

The Grid must have room for long turns, boosts, powerup collection, defensive driving, and aggressive cuts.

### Future arenas (backlog)

- **The Cross.** Intersecting corridors.
- **The Ring.** A central structure.
- **The Maze.** Static walls and corridors.
- **The Void.** Leaving the playable area eliminates the player.
- **The Split.** Connected areas with narrow passages.

---

## 9. Powerups

Powerups are part of v2. They create tactical chances. They are not the main mechanic.

A losing player must not automatically win because a pickup kills the leader. Good: Phase lets you escape a trap. Bad: a random weapon that instantly kills the leader.

The trail remains the primary skill.

### Spawning

- Spawn at predefined, safely randomized locations.
- Never spawn inside a trail.
- Avoid spawning immediately next to a player.
- Each pickup has a limited lifetime.
- The host decides location, type, time, and pickup result. Clients display that result.

Suggested cap: **2–4 active powerups**. Starting config value: **3**. Tune to arena size.

First implementation may pick a random valid point. Later logic may score distance to players, nearby trails, and how reachable the point is, so one player does not get an overwhelming spawn.

### Inventory

One held powerup. If the player is already holding one, they cannot collect another. This blocks hoarding. Two slots are a possible later change, not the MVP.

Activation is manual, not automatic. Default: keyboard Space, controller right trigger. Bindings are configurable. Timing is part of the skill.

### Types

All six are in the v2 definition of done. Build Boost, Shield, Phase, and Trail Cutter first. Then EMP and Overdrive. Do not drop the last two. See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) and [DECISIONS.md](DECISIONS.md).

| Powerup | Player experience | Tuning target |
| --- | --- | --- |
| Boost | Temporary speed increase. Larger turn radius. | Speed about 12 to 22 for 1–2 seconds. |
| Shield | One protection charge. The next trail hit consumes the shield and the player survives, with strong visual and audio feedback. Not infinite contact. | One charge. |
| Phase | Pass through trails for a short time. Vehicle looks transparent or otherwise obviously phased. Does not have to pass through the arena boundary. | 1.5–3 seconds. |
| EMP | Affects another player. Must not disable their movement. They can still play. | MVP effect: a short reduction in steering responsiveness. Disabling their held powerup, or disrupting their HUD, are not the MVP effect. See [DECISIONS.md](DECISIONS.md). |
| Trail Cutter | Removes a short portion of trail. Useful when a player traps themselves. | Short segment, not the whole trail. |
| Overdrive | Higher speed and acceleration, much worse steering. High risk, high reward. | On top of the MVP constant-speed model, this is a temporary speed increase plus a strong turn-rate penalty. A separate acceleration curve is future work. |

Each type needs its own config (for example Boost: speed multiplier, duration, cooldown; Shield: charges, duration; Phase: duration; EMP: range, duration, effect strength).

### Feedback

The HUD shows what is held, that it is ready, and how long an active effect lasts. Example: a panel reading `SHIELD` and `PRESS SPACE`, then a draining bar while active.

Every powerup also needs:

- A unique icon, color, and effect
- Distinct pickup and activation sounds
- A visible change on the vehicle while active

Players should understand the state without reading text.

---

## 10. Rounds and match

Default match: **first to 5** round wins.

Round flow:

1. Waiting
2. Ready
3. Countdown 3, 2, 1
4. Go
5. Active play and eliminations
6. Winner
7. Results
8. Next round

Match flow sits on the game state machine in section 13. After the score limit, show match results, then return to the lobby or menu. Rematch is part of the match milestone.

Each participant tracks: player id, display name, vehicle color, trail color, position, heading, speed, alive, score, powerup, powerup state.

---

## 11. Camera and HUD

### Camera

Slightly angled top-down if that reads better than a flat orthographic view. Prioritize arena visibility, trail readability, vehicle readability, and powerup visibility.

No cinematic camera movement during play. Competitive clarity comes first.

MVP camera is fixed. Do not follow one player and hide the others.

Later option: a slight zoom based on where the living players are spread out. Still show the whole contest.

> **Implementation note (D21).** The user explicitly chose a chase camera behind their own vehicle and dropped the whole-arena view, overriding the two paragraphs above. The arena's fixed camera is kept for the menu and a future spectator mode. See [DECISIONS.md](DECISIONS.md).

Very small screen shake only for major collisions, EMP activation, powerup activation, and the final elimination. Do not overuse it.

### HUD

Show scores, round number, each player's color, the held powerup, and powerup state. Corner layout from the guide: scores for P1–P4, round number, powerup prompt along the bottom.

### Menus

**Main menu:** Solo, LAN Play, Options, Credits, Quit.

**Solo:** AI count 1, 2, or 3; difficulty Easy, Normal, Hard, Expert; arena (The Grid until others exist); Start.

**LAN:** Host Game, Find Games, Join by IP, Back.

**Host lobby:** host name, up to four slots with ready state, mode, arena, first-to score, powerups on or off, Start Match. Empty slots show Waiting.

**Client lobby:** same roster, local color choice, Ready. The client does not start the match.

**Find games:** list host name, player count (for example 2/4), ping, and Join.

**Join by IP:** host IP, port, Connect.

Configurable match settings, host only, and only if implemented:

- Mode
- Arena
- AI count (solo only)
- Score limit
- Powerups on or off
- Powerup frequency

---

## 12. Presentation

### Look

Dark digital arena, luminous vehicles, high-speed motion, neon energy. Use dark environments, grid surfaces, atmospheric haze, bloom, neon trails, bright vehicle cores, and subtle holographic UI. Do not copy specific movie assets.

### Audio

Required sounds:

- Vehicle hum
- Steering
- Trail generation
- Powerup pickup
- Powerup activation
- Boost
- Shield impact
- Phase activation
- EMP
- Trail destruction
- Collision
- Player elimination
- Round start
- Round victory
- Match victory

### Music

Can come after basic audio. Intensity rises with danger:

- Round start: low
- Few players remain: higher
- Final two: high
- Final confrontation: maximum

### Particles

Trail sparks, steering, boost, powerup pickup, shield impact, phase, EMP, trail cutter, vehicle destruction, match victory. Effects must not obscure collision geometry.

---

## 13. Game state machine (player-facing)

```text
BOOT
  -> MAIN_MENU
       -> SOLO -> SOLO_LOBBY -> COUNTDOWN -> PLAYING
       -> LAN -> LAN_MENU -> HOST | SEARCH | JOIN -> LAN_LOBBY -> COUNTDOWN -> PLAYING

PLAYING
  -> ROUND_RESULTS
  -> NEXT_ROUND
  -> MATCH_RESULTS
  -> LOBBY or MENU
```

---

## 14. AI and opponents (experience)

AI opponents are other players. They follow the same trails, deaths, and powerups. Difficulty changes how well they drive and plan, not the physics. Easy is slow to react and poor at planning. Expert uses space and powerups well and does not cheat.

Behavior detail is in [AI_SPEC.md](AI_SPEC.md).

---

## 15. The intended moment

A player joins a LAN lobby from another PC, sees the host and the player count, picks a color, and waits through 3-2-1-GO. The vehicle moves immediately. A turn leans the body and bends the trail. An opponent approaches. The player can turn early, boost through, or take a powerup. They take Phase, pass a wall, then find they are moving too fast and have to steer harder. The opponent follows and crashes because there is not enough room. Two vehicles remain in a maze of trails. One has Shield, one has Boost. Music would intensify. The last open space is the fight.

That sequence is the target feel. Systems that do not support it wait.

---

## 16. Definition of done (player-facing)

A v2 match is done when a human can:

- Drive with continuous steering, speed-dependent turning, rotation, lean, and boundary death
- Die to trails, including their own, and to other vehicles
- Finish an offline Survival match against 1, 2, or 3 AI at a chosen difficulty
- Use Boost, Shield, Phase, Trail Cutter, EMP, and Overdrive with one held slot and manual activation
- Play rounds to a first-to-5 victory and rematch
- Host or join a LAN game by discovery or direct IP, with up to three other humans, and see the same trails, pickups, deaths, scores, and rounds
- Use the menus, lobby, HUD, powerup UI, and results, with neon presentation and basic audio

The checklist is tracked in [TODO.md](TODO.md).

### After v2

Movement polish, powerup balancing, more arenas, more AI personalities, team mode, territory mode, advanced arena mechanics, vehicle customization, tournament mode, then possible online multiplayer.
