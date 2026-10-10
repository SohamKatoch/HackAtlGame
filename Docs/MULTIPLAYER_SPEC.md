# Neon Grid — Multiplayer Specification

LAN multiplayer is a first-class v2 requirement. Offline solo uses the same rules with no remote clients. Internet play is out of scope, but gameplay code must not assume the transport can never change.

Player-facing lobby and mode rules are in [GAME_DESIGN.md](GAME_DESIGN.md). Simulation ownership is in [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md).

---

## 1. Topology

```text
Offline local authority
        |
Listen-server LAN host
        |
Future online transport
```

One interface in front of gameplay: the authority simulates, clients submit input and present state. Offline is a listen-style match with zero remote connections, not a second ruleset.

```text
                     HOST (listen server)
                      |
          +-----------+-----------+
          |           |           |
       CLIENT       CLIENT      CLIENT
         P2           P3          P4
```

The host is also player 1. There is no dedicated server in v2.

Maximum players: 4.

---

## 2. Authority

The host is authoritative for:

- Player position, heading, and speed
- Movement simulation
- Trail geometry (create, stop, cut)
- Collision and elimination
- Powerup spawn location, type, time, pickup, activation result, and expiration
- Scores
- Round state
- Match state

Clients own:

- Local input sampling
- Rendering
- Audio
- Visual effects
- UI
- Local prediction of their own vehicle
- Sending input commands

Clients do not decide their own authoritative position, other players' positions, collisions, scores, pickup results, or deaths.

Security rule, even on a trusted LAN: clients send "I am steering left." They do not send "I moved here and killed player 3." The host validates and simulates.

---

## 3. What replicates

### Replicated state (unreliable or low-rate where safe, reliable when the event must arrive)

- `PLAYER_STATE`: id, position, heading, speed, alive, colors if not already in player state
- `TRAIL_SEGMENT`: new compact segments (start, end, owner, tick). Batched.
- `TRAIL_UPDATE`: removal or split from Trail Cutter, or a trail stop on death
- `POWERUP_STATE`: active pickups and each player's held/active powerup
- `SCORE_UPDATE` and round index, via player state and game state
- Match phase

### Reliable events

- `JOIN_REQUEST` / `JOIN_ACCEPTED` (UE connection and login)
- `PLAYER_READY`
- `POWERUP_SPAWN`, `POWERUP_PICKUP`, `POWERUP_ACTIVATE` if not fully covered by replicated state
- `PLAYER_ELIMINATED`
- `ROUND_START`, `ROUND_END`
- `MATCH_START`, `MATCH_END`
- `DISCONNECT` notification to remaining players

### Client to server only

`PLAYER_INPUT`:

```text
playerId
tick
steering      (-1..1)
activatePowerup (bool)
```

The guide's sample also includes `boost`. There is no separate boost button in v2. Boost is a powerup activation, so the input bit is `activatePowerup` only. Do not add a hold-to-boost control unless the design doc changes.

`PING` is UE's existing net ping for the lobby list. Do not build a second time-sync protocol unless prediction cannot be corrected without it.

Map these names onto UE replicated properties and RPCs. Do not stand up a parallel UDP protocol that reimplements the same messages. Keep the gameplay calls ("submit input", "apply authoritative state") in one place so a future online transport can replace the listen-server calls.

---

## 4. Tick, prediction, interpolation

Simulation is 60 Hz on the host. See [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md).

### Local player

```text
local input
  -> immediate local prediction (same movement step)
  -> input sent to host with the simulation tick
  -> host simulates
  -> authoritative state returns
  -> client reconciles
```

Prediction exists so steering feels immediate. LAN latency should stay low enough that reconciliation can be a short rewind-and-replay of buffered inputs, or a small visual correction if the error is tiny. Do not import a general-purpose netcode framework for this.

If predicted death and server survival disagree, the server wins. Shield and Phase outcomes are server outcomes. The predicted client may show an anticipated effect, then snap to the server result.

### Remote players

Interpolate between received states. Do not teleport between updates unless the error is large (respawn, join, or a correction threshold).

Remote trails can appear as soon as the segment is received. Do not wait to build a mesh on the host and replicate that mesh.

### Cosmetic versus gameplay

| Kind | Where it runs |
| --- | --- |
| Lean, bloom, particles, shake, audio | Each client, from replicated state and events |
| HUD and menus | Local |
| Death, score, trail cut, pickup grant | Host only |
| Predicted motion | Owning client only, then corrected |

---

## 5. Discovery and joining

### Find games

Choosing LAN Play and Find Games searches the local network for hosts.

Display, per session:

- Game name: Neon Grid
- Host name (machine or player name)
- Players, for example 2/4
- Ping
- Join

### Join by IP

Required fallback when discovery fails:

- Host IP
- Port
- Connect

### Failures that must show an explicit message

Never fail silently. Cover at least:

- No games found
- Invalid IP
- Connection failed or timed out
- Lobby full
- Join rejected
- Disconnect during lobby
- Disconnect during a round
- Disconnect between rounds
- Host shutdown
- Client leaves on purpose

Copy can be plain. The requirement is that the player sees why the action stopped.

---

## 6. Lobbies

### Host lobby

Shows the host name, up to four slots (`READY` or `WAITING`), mode, arena, first-to score, powerups on or off, and Start Match.

The host configures only implemented settings:

- Mode (Survival until others exist)
- Arena (The Grid until others exist)
- Score limit
- Powerups on or off
- Powerup frequency

AI count is solo-only. Do not show it in the LAN lobby.

### Client lobby

Same roster. The local player sets color and readies. They do not start the match.

### Start

Host starts when the match settings are valid. Empty slots stay empty. LAN v2 does not fill empty slots with AI unless a later decision says so. The guide's solo mode is the place AI players are added. A LAN match is the humans who joined.

---

## 7. Disconnects

### Client leaves

Show that the player disconnected. Remove them from the match. Do not replace them with AI. If one human remains in an active round, that human wins the round under last-alive rules. If the lobby loses a client, the slot returns to Waiting.

### Host leaves

Host migration is not in v2. Every client sees that the host disconnected, the match ends, and they can return to the menu.

---

## 8. Bandwidth

The host must not stream trail meshes, skeletal poses, or per-particle state.

Send:

- Input each tick or a small coalesced window from each client (one steering float and one bit)
- Authoritative pose per player at a steady rate (position, heading, speed, alive)
- Trail segments as they are created, and compact edits when they change

Four players on a LAN should be a negligible load if trails stay as segments.

---

## 9. Testing bar

Do not describe multiplayer as done until these have been run on the real network path, not only by reading code.

### Player counts

- Host + 1 client
- Host + 2 clients
- Host + 3 clients

### Cases

- Discovery finds the host
- Direct IP connects when discovery is not used
- Join, ready, start
- Leave from the lobby
- Disconnect during a round
- Disconnect between rounds
- Host closes the game
- Invalid IP shows an error
- A fifth player cannot join a full lobby
- No hosts: the find screen says none were found

Also, for each powerup: pickup, activation, expiration, collision changes, death, all players seeing the same result, and a disconnect while the effect is active.

Offline tests (player versus 1, 2, and 3 AI) are required too, but they do not count as LAN tests.

---

## 10. Out of scope for v2

- Internet matchmaking, relays, or accounts
- Host migration
- Replacing a dropped human with AI
- Split-screen or multiple local players
- Client-authoritative movement
- A from-scratch networking library
