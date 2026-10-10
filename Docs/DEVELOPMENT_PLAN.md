# Neon Grid — Development Plan

Build in this order. Each milestone is playable and testable before the next one starts. Do not generate the whole game in one pass.

Rules for every milestone:

1. Read the spec section named below.
2. Inspect the existing Unreal project before editing.
3. Reuse the movement, trail, and match systems. Do not fork them.
4. Implement the smallest version that meets the goal.
5. Compile.
6. Test the cases listed here.
7. Write remaining issues into [TODO.md](TODO.md).

If driving is not fun at the end of milestone 1, stop. Do not add trails to cover a bad vehicle.

Specs: [GAME_DESIGN.md](GAME_DESIGN.md), [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md), [VEHICLE_SPEC.md](VEHICLE_SPEC.md), [AI_SPEC.md](AI_SPEC.md), [MULTIPLAYER_SPEC.md](MULTIPLAYER_SPEC.md), [DECISIONS.md](DECISIONS.md).

---

## Milestone 1 — Movement

**Specs:** VEHICLE_SPEC, TECHNICAL_SPEC sections 3 and 5.

**Build:**

- C++ game module on the current 5.8 Blueprint project
- Default map and game mode switched off the first-person template
- Split-screen disabled
- Rectangular arena bounds (The Grid)
- One vehicle, continuous heading, cruise speed, steering, yaw, cosmetic lean
- Boundary collision
- Fixed 60 Hz step
- Vehicle Data Asset with the starting fields

**Do not build:** networking, powerups, trails, AI, match scoring, polished UI.

**Manual Editor work:** create the module if the Editor must generate project files, set the startup map and game mode, create the Enhanced Input assets, place a placeholder mesh and a fixed camera.

**Done when:** the feel tests in VEHICLE_SPEC section 8 pass in the Editor, including a wider arc when speed is temporarily forced up.

**Test:** play for several minutes. Try flicks, held turns, near-90-degree corners, and attempted reversals.

---

## Milestone 2 — Trails

**Specs:** GAME_DESIGN trails and collision, TECHNICAL_SPEC section 6.

**Build:**

- Segment registry (start, end, owner, tick)
- Ribbon renderer separate from collision
- Swept collision and a grid spatial hash
- Death on any trail, own trail, and another vehicle
- A second pawn driven by a debug input or a duplicated local controller is optional and temporary. Prefer two scripted pawns over illegal split-screen.

**Do not build:** real AI, powerups, LAN.

**Done when:** two vehicles can trap and eliminate each other, including a self-collision, and the frame rate stays smooth as trails grow for a full round-length session.

**Test:** head-on trail cross, graze, self-box, boundary, vehicle-vehicle hit. Confirm lean does not change the hit.

---

## Milestone 3 — Solo AI

**Specs:** AI_SPEC.

**Build:**

- AI controller writing the shared input snapshot
- Cruise and Avoid, then the other states as the milestone allows
- Four difficulty assets
- Solo menu: 1–3 AI, difficulty, The Grid, Start
- Eliminations of AI and the player

**Done when:** a human can lose or win a single life against 1, 2, and 3 AI on at least Normal, and Easy is visibly worse than Hard without faster physics.

**Test:** player versus 1, 2, and 3 AI. Confirm Expert does not use a different collision radius or speed asset.

---

## Milestone 4 — Powerups

**Specs:** GAME_DESIGN powerup section, TECHNICAL_SPEC section 7.

**Build in this order:**

1. Spawner, one inventory slot, manual activation, HUD slot
2. Boost
3. Shield
4. Phase
5. Trail Cutter
6. EMP (steering-rate reduction only)
7. Overdrive

EMP and Overdrive are in this milestone, after the first four. They are not optional cuts. See DECISIONS.

Solo and the future host use one code path.

**Done when:** each powerup can be picked up, blocked when a slot is full, activated, expired, and observed to change collision or speed as designed. Two pickups do not occupy the same trail. The arena never exceeds the active cap.

**Test:** the powerup matrix in MULTIPLAYER_SPEC section 9 for offline first (pickup, activation, expiration, collision, death, two vehicles).

---

## Milestone 5 — Round and match

**Specs:** GAME_DESIGN rounds, TECHNICAL_SPEC section 8.

**Build:**

- Countdown 3-2-1-GO
- Round start and end
- Last alive scores
- First to 5
- Match victory
- Rematch back to the solo lobby
- Results screen

**Done when:** a full offline match can be finished and restarted without leaving to the desktop.

**Test:** play to 5 with AI. Confirm a round cannot start without the countdown. Confirm scores match wins.

---

## Milestone 6 — LAN

**Specs:** MULTIPLAYER_SPEC.

**Build:**

- Listen server host and joining client
- LAN session search
- Direct IP and port
- Host and client lobbies, ready, color
- Input to the host
- Replicated player state, trail segments, powerups, deaths, scores, rounds, match end
- Explicit errors for the failure list

Prediction polish can be crude here if the vehicle is still drivable. Milestone 7 tightens it.

**Done when:** four PCs (or the closest available set, including host + 1 and host + 3) finish a Survival match and see the same winner.

**Test:** every case in MULTIPLAYER_SPEC section 9. Do not mark this milestone done from a single-machine PIE test alone. PIE is a smoke test. LAN needs separate processes or machines.

**Manual Editor work:** shipping or development firewall prompts, default port, session plugin settings if the Online Subsystem must be enabled.

---

## Milestone 7 — Network polish

**Build:**

- Owning-client prediction and correction
- Remote interpolation
- Ping on the browser
- Connection status
- Clearer reconciliation when a hit is reversed by the server
- Disconnect copy for lobby, in-round, between rounds, and host shutdown

**Done when:** steering on a client feels immediate on a quiet LAN, remote vehicles do not stutter-step, and every disconnect path shows a message and a way back to the menu.

**Test:** repeat host + 1 with induced small delay if possible. Repeat the disconnect list.

---

## Milestone 8 — Visual polish

**Build:**

- Dark grid arena, bloom, haze, neon materials
- Vehicle mesh (Blender pipeline once the placeholder is no longer enough)
- Trail ribbon, pickup and vehicle VFX
- UI animation
- Small screen shake on the allowed events only

**Done when:** a stranger can tell heading, trail ownership by color, and which powerup is active without reading a paragraph. Collision geometry is still obvious. 60 FPS holds with four players and long trails.

**Manual Editor work:** import meshes, materials, Niagara, post-process volume.

---

## Milestone 9 — Audio

**Build:** the sound list in GAME_DESIGN, vehicle loop, powerup and collision stingers, round and match stingers. Dynamic music intensity by remaining players may land here or immediately after, but it is part of this phase in the guide.

**Done when:** pickup, activation, shield break, elimination, and victory are distinct with eyes closed.

---

## Milestone 10 — Additional content

Only after a finished Survival LAN match. Backlog, not a commitment to build all of it now:

- The Cross, The Ring, The Maze, The Void, The Split
- New powerups
- AI personalities
- Team mode
- Territory mode
- Tournament mode
- Vehicle cosmetics and customization
- Advanced arena mechanics
- Online transport behind the existing authority interface

Do not expose these in menus until they work.

---

## Always-on checks

- Offline: player versus 1, 2, and 3 AI
- LAN: host + 1, host + 2, host + 3
- Powerups: pickup, activation, expiration, collision, death, multiple players, replication, disconnect
- Performance: 60 FPS or better on a modern PC with four players, long trails, particles, bloom, and net updates

## Post-v2 order

Core MVP, then movement polish, powerup balancing, more arenas, more AI personalities, team mode, territory mode, advanced arena mechanics, vehicle customization, tournament mode, possible online multiplayer.
