# NEON GRID v2
## Game Design, Technical Architecture & Vibe-Coding Development Specification

Genre Competitive Arcade Arena  Futuristic Combat Racing  
Perspective Top-down  slightly angled 3D  
Players 1–4  
Multiplayer LAN multiplayer  
Offline Single-player vs AI  
Online Multiplayer Not required for v2; architecture should leave room for it later  
Platform Target PC  
Core Inspiration Futuristic light-cycle  trail-combat games  
Core Identity High-speed arena combat where players control space using glowing trails, steering, positioning, and powerups.

---

# 1. GAME CONCEPT

Neon Grid is a fast-paced futuristic arena game centered around high-speed vehicles that leave glowing energy trails behind them.

Players are constantly moving.

The trail they create becomes an obstacle.

As the arena fills with trails, the available space decreases and players are forced into increasingly dangerous maneuvers.

Unlike traditional light-cycle games that lock players to four-directional 90° turns, Neon Grid uses continuous steering.

Players can make

- Small corrections
- Gradual curves
- Sharp turns
- Wide arcs
- Aggressive hairpins

The vehicle visually leans into turns to communicate speed and direction.

The result should feel more like driving a futuristic high-performance vehicle through a dynamically changing arena rather than moving a game piece around a grid.

---

# 2. CORE GAMEPLAY RULE

The game can be understood immediately

 Keep moving. Control space. Don't hit a trail.

The player is simultaneously

- Driving
- Creating territory
- Avoiding existing trails
- Cutting off opponents
- Managing speed
- Collecting powerups
- Deciding when to take risks

---

# 3. GAME MODES

## 3.1 Offline Solo

One player fights against AI-controlled opponents.

Example

```text
PLAYER
VS
3 AI
```

The AI should use the same fundamental movement and trail rules as human players.

Solo mode should not feel like a separate game.

It should be the same underlying match system with AI filling player slots.

---

# 4. LAN MULTIPLAYER

LAN is the primary multiplayer mode.

One player hosts a match.

Other players connect using separate devices on the same local network.

Example

```text
                 ROUTER

       ┌──────────┼──────────┐
       │          │          │
     PC 1       PC 2       PC 3
     HOST         P2         P3
                    
                     PC 4
                      P4
```

Players do not share one screen.

Each player runs their own copy of the game.

The host runs the authoritative match simulation.

---

# 5. NO COUCH MULTIPLAYER

Neon Grid does not support shared-screencouch multiplayer.

Do not implement

- Multiple keyboard players on one PC
- Multiple controllers controlling different players on one PC
- Split-screen
- Shared-screen local multiplayer

Every human player in multiplayer uses their own device.

This substantially simplifies the input architecture and allows the game to focus on LAN networking and responsive vehicle control.

---

# 6. FUTURE ONLINE MULTIPLAYER

Internet multiplayer is not part of the initial game.

However, the networking layer should be designed so that

```text
Offline
   ↓
LAN
   ↓
Future Online
```

can eventually use the same gameplaynetwork interfaces.

Do not design the LAN implementation in a way that permanently prevents online multiplayer.

---

# 7. DESIGN PILLARS

## Pillar 1 — Movement Is the Game

Driving should feel excellent before powerups, progression, or additional content are added.

## Pillar 2 — Continuous Steering

Players are not locked to four directions.

The vehicle can smoothly change heading.

## Pillar 3 — Trails Control Space

The primary weapon is the player's ability to manipulate the arena through trail placement.

## Pillar 4 — Powerups Create Opportunities

Powerups should create tactical decisions rather than replace the core trail mechanic.

## Pillar 5 — Short, Replayable Matches

A complete match should be easy to restart.

## Pillar 6 — LAN First

Multiplayer should work reliably across separate PCs on the same network.

---

# 8. CORE GAMEPLAY LOOP

```text
START GAME
    ↓
SELECT OFFLINE  LAN
    ↓
LOBBY
    ↓
SELECT VEHICLE  COLOR
    ↓
MATCH START
    ↓
ACCELERATE
    ↓
STEER
    ↓
CREATE TRAIL
    ↓
CONTROL SPACE
    ↓
COLLECT POWERUPS
    ↓
OUTMANEUVER OPPONENTS
    ↓
COLLISION
    ↓
ELIMINATION
    ↓
LAST PLAYER ALIVE
    ↓
ROUND WIN
    ↓
NEXT ROUND
    ↓
MATCH VICTORY
```

---

# 9. MOVEMENT SYSTEM

## 9.1 Continuous Heading

The vehicle should maintain a heading represented by an angle.

Conceptually

```text
heading = 73°
```

rather than

```text
NORTH
EAST
SOUTH
WEST
```

The vehicle moves continuously in its current heading.

```text
velocity = direction(heading) × speed
```

---

# 10. STEERING

The player controls the rate at which the vehicle changes heading.

Conceptually

```text
steering input
      ↓
turn rate
      ↓
heading changes
      ↓
vehicle follows new heading
```

Example

```text
Heading 90°
Steering LEFT
Turn rate -120°second

New heading
90 → 88 → 86 → 84 → ...
```

The exact turn rate should be tuned during playtesting.

---

# 11. TURNING DESIGN

The game should support a wide range of turn sizes.

### Small correction

```text
───────────╮
           ╰────
```

### Medium turn

```text
──────╮
      │
      ╰──────
```

### Sharp turn

```text
────╮
    │
    ╰──
```

### Large arc

```text
      ╭────
    ╭─╯
  ╭─╯
─╯
```

The player should be able to choose the appropriate maneuver based on the situation.

---

# 12. 90° TURNS

90° turns are still possible.

They are simply no longer mandatory.

A player can execute something close to a 90° turn by holding the steering input long enough.

This means the classic light-cycle maneuver remains possible while adding considerably more freedom.

---

# 13. 180° TURNS

The game should strongly discourage instant reversals.

The vehicle should not be able to instantly perform

```text
→ ←
```

Instead, a 180° reversal should require a sufficiently large turn radius and available space.

Possible design

```text
Maximum steering rate
+
Minimum speed
+
Turn radius
```

make a true instant U-turn impossible.

This prevents players from abusing steering to escape situations that should be fatal.

---

# 14. TURN RADIUS

Vehicle turning should be influenced by speed.

Recommended concept

### Normal speed

Moderate turning radius.

### High speed

Larger turning radius.

### Boost

Significantly larger turning radius.

This creates an important tradeoff

 Going faster makes you more powerful but less maneuverable.

---

# 15. LEANING

The vehicle should visually lean into turns.

This is primarily a visual system, not a separate gameplay mechanic.

Example

```text
STEERING LEFT

          ╲
           ╲
            ╲
          [VEHICLE]
             ╲
```

The vehicle body tilts toward the turn.

When steering right

```text
          [VEHICLE]
             ╱
            ╱
           ╱
```

The lean should be

- Smooth
- Responsive
- Limited
- Automatically returned toward neutral when steering stops

Do not let the visual lean alter the collision geometry.

---

# 16. LEAN AMOUNT

Example conceptual values

```text
Maximum visual lean 15–25°
Lean response fast
Return to neutral smooth
```

The exact values should be tuned visually.

The lean should communicate

 The vehicle is turning aggressively.

without making the vehicle look like it is tipping over.

---

# 17. SPEED MODEL

The vehicle should have a base cruising speed.

Example

```text
Base Speed 12
```

Potential future acceleration system

```text
Minimum speed
    ↓
Acceleration
    ↓
Cruising speed
    ↓
Boost
```

For the MVP, constant base speed is acceptable.

---

# 18. TRAIL SYSTEM

Every vehicle continuously creates an energy trail.

The trail is both

- A visual effect
- A physical obstacle

The trail should remain behind the vehicle after it passes.

Example

```text
             PLAYER
                ↓
───────────────╮
               │
               │
               ╰────────────
```

---

# 19. TRAIL DATA

Represent trails as line segments or curve segments rather than individual physical objects.

Each segment should contain

```text
start
end
ownerID
creationTick
```

Potential future properties

```text
energyState
powerupEffect
expirationTime
```

---

# 20. TRAIL RENDERING

The rendered trail should be independent from its collision representation.

Example

```text
Trail Collision
      │
      └──────→ invisible mathematical geometry

Trail Renderer
      │
      └──────→ glowing visual mesh
```

This allows the visual trail to have

- Glow
- Bloom
- Particles
- Animation
- Thickness

without affecting gameplay collision.

---

# 21. COLLISION RULES

A player dies when their vehicle collides with

- Arena boundary
- Any active trail
- Their own trail
- Another vehicle

Powerups may temporarily modify these rules.

---

# 22. COLLISION DETECTION

The collision system should detect intersections between the vehicle's movement path and trail geometry.

Do not create thousands of rigidbody colliders.

For example

```text
Vehicle movement
       ↓
Line  swept collision
       ↓
Trail spatial lookup
       ↓
Intersection test
       ↓
Collision
```

A spatial partitioning system can be added if trail count becomes large.

Possible optimization

```text
Grid
Quadtree
Spatial Hash
```

---

# 23. ARENA

The initial arena should be simple.

## THE GRID

Large rectangular arena.

```text
┌─────────────────────────────────────┐
│                                     │
│       P1 →              ← P2        │
│                                     │
│                                     │
│                                     │
│       P3 →              ← P4        │
│                                     │
└─────────────────────────────────────┘
```

The arena should provide enough space for

- Long turns
- Boosts
- Powerup collection
- Defensive driving
- Aggressive cuts

---

# 24. FUTURE ARENAS

## THE CROSS

```text
        │
        │
────────┼────────
        │
        │
```

## THE RING

Central structure.

## THE MAZE

Static walls and corridors.

## THE VOID

Leaving the playable area results in elimination.

## THE SPLIT

Multiple connected areas with narrow passages.

---

# 25. POWERUP SYSTEM

Powerups are a core part of the v2 design.

They should create tactical opportunities without becoming the primary gameplay mechanic.

Powerups spawn periodically throughout the arena.

Example

```text
             ◆

     P1 →

                 ◆

          P2 ←

                       ◆
```

Players must decide whether going for a powerup is worth exposing themselves to a trail or losing positioning.

---

# 26. POWERUP SPAWNING

Powerups should

- Spawn at predefined saferandomized locations
- Never spawn directly inside trails
- Avoid spawning immediately next to a player
- Have a limited lifetime
- Be synchronized by the host

The host determines

```text
spawn location
spawn type
spawn time
pickup result
```

Clients only display the authoritative result.

---

# 27. POWERUP LIMIT

Avoid flooding the arena.

Suggested starting rule

```text
Maximum active powerups 2–4
```

The exact number should depend on arena size and playtesting.

---

# 28. POWERUP TYPES

## 28.1 BOOST

Temporarily increases speed.

Example

```text
Speed
12 → 22
```

Duration

```text
1–2 seconds
```

Tradeoff

 Faster movement + reduced maneuverability.

---

# 29. SHIELD

The vehicle gains one collision protection charge.

```text
SHIELD ACTIVE
```

When hitting a trail

```text
Collision
   ↓
Shield consumed
   ↓
Player survives
```

The player should still receive strong visualaudio feedback.

The shield should not permit unlimited contact.

---

# 30. PHASE

Temporarily allows the player to pass through trails.

Example

```text
Duration
1.5–3 seconds
```

The vehicle should become visually transparent or otherwise clearly indicate that Phase is active.

Important

Phase should not necessarily allow passing through the arena boundary.

---

# 31. EMP

Targets another player.

Effect

- Briefly disables their powerup
- Or temporarily reduces their maneuverability
- Or temporarily disrupts their HUD

Do not make EMP completely disable vehicle movement.

The victim should still be able to play.

---

# 32. TRAIL CUTTER

Removes a limited portion of trail.

Example

```text
───────────────████████
               ↑
          CUT HERE
```

The powerup removes a short trail segment.

This is particularly useful when a player accidentally traps themselves.

---

# 33. OVERDRIVE

Temporarily increases

- Speed
- Acceleration

but significantly reduces

- Steering responsiveness

This creates a high-riskhigh-reward powerup.

The player becomes extremely fast but difficult to control.

---

# 34. POWERUP INVENTORY

The player should only carry a limited number of powerups.

Recommended

One active powerup at a time.

Pickup behavior

```text
Player already holding powerup
        ↓
New powerup
        ↓
Cannot collect
```

This prevents hoarding.

A future system could allow two slots, but one is preferable for MVP.

---

# 35. POWERUP ACTIVATION

Recommended control

```text
Keyboard
SPACE

Controller
R2  Right Trigger
```

The exact control can be configurable.

Powerups should be activated manually rather than automatically.

This adds strategic timing.

---

# 36. POWERUP UI

Display the current powerup clearly.

Example

```text
┌──────────────┐
│ SHIELD       │
│ PRESS SPACE  │
└──────────────┘
```

When active

```text
SHIELD
████████░░
```

The player should always understand

- What they have
- Whether it is ready
- How long it lasts

---

# 37. POWERUP BALANCE

Powerups should not allow a losing player to automatically win.

They should create opportunities.

Good

 I can use Phase to escape this trap.

Bad

 I lost, but the game randomly gave me a weapon that instantly kills the leader.

The trail remains the primary source of skill expression.

---

# 38. POWERUP SPAWN FAIRNESS

The host should avoid spawning powerups in situations where one player receives an overwhelmingly advantageous position.

Potential future logic

```text
Evaluate player distance
Evaluate nearby trails
Evaluate arena accessibility
Select valid spawn location
```

The first implementation can use randomized valid spawn points.

---

# 39. GAME MODES

## Survival

Default mode.

Last player alive wins.

---

## Future Territory

Players score based on the amount of arena space controlled.

---

## Future Team Battle

2v2.

Teams share colors and potentially interact with each other's trails differently.

---

## Future Time Attack

Players score based on survival time.

---

## Future Elimination

Players are eliminated permanently across multiple rounds.

---

# 40. ROUND STRUCTURE

Default

First to 5 round victories.

Example

```text
P1  4
P2  3
P3  2
P4  1
```

Player 1 needs one more victory.

---

# 41. ROUND FLOW

```text
WAITING
 ↓
READY
 ↓
3
 ↓
2
 ↓
1
 ↓
GO
 ↓
ACTIVE
 ↓
ELIMINATIONS
 ↓
WINNER
 ↓
RESULTS
 ↓
NEXT ROUND
```

---

# 42. PLAYER DATA

Each player should have

```text
playerId
displayName
vehicleColor
trailColor
position
heading
speed
alive
score
powerup
powerupState
```

---

# 43. AI PLAYER DATA

AI players should use the same player representation as human players.

The only major difference should be input generation.

```text
Human
Input Device
    ↓
Player Controller

AI
AI Controller
    ↓
Player Controller
```

This is important.

Do not create a completely separate movement implementation for AI.

---

# 44. AI SYSTEM

The AI should understand

- Current heading
- Arena boundaries
- Nearby trails
- Other players
- Powerups
- Available space
- Collision risk

AI behavior can start simple.

### AI states

```text
CRUISE
TURN
AVOID
CHASE
COLLECT_POWERUP
ESCAPE
ATTACK
```

---

# 45. AI DIFFICULTY

Initial difficulties

### Easy

Large reaction time.

Poor trail planning.

### Normal

Reasonable navigation.

### Hard

Better spatial awareness.

### Expert

Advanced trail control and powerup usage.

Do not make Expert AI cheat by receiving different physics.

---

# 46. LAN ARCHITECTURE

Use a host-authoritative architecture.

```text
                    HOST
                     │
          ┌──────────┼──────────┐
          │          │          │
        CLIENT     CLIENT     CLIENT
          P2         P3         P4
```

The host is responsible for authoritative

- Player states
- Movement simulation
- Trail geometry
- Collision
- Powerup spawning
- Powerup pickups
- Eliminations
- Scores
- Round state
- Match state

---

# 47. CLIENT RESPONSIBILITIES

Clients handle

- Local input
- Rendering
- Audio
- Visual effects
- UI
- Local prediction
- Sending input commands

Clients do not authoritatively decide

- Their own position
- Other players' positions
- Collisions
- Scores
- Powerup results
- Deaths

---

# 48. NETWORK MESSAGE TYPES

Use structured messages.

```text
JOIN_REQUEST
JOIN_ACCEPTED
PLAYER_READY

PLAYER_INPUT

PLAYER_STATE
TRAIL_SEGMENT
TRAIL_UPDATE

POWERUP_SPAWN
POWERUP_PICKUP
POWERUP_ACTIVATE
POWERUP_STATE

PLAYER_ELIMINATED

ROUND_START
ROUND_END
SCORE_UPDATE

MATCH_START
MATCH_END

PING
DISCONNECT
```

---

# 49. INPUT NETWORKING

Clients send input rather than authoritative position.

Example

```text
PLAYER_INPUT

playerId 2
tick 1842
steering -0.75
boost false
activatePowerup false
```

The host processes this input.

This is more reliable than allowing clients to dictate their own state.

---

# 50. NETWORK TICK

Use a fixed simulation tick.

Example

```text
60 simulation tickssecond
```

Rendering can operate independently.

```text
SIMULATION
60 Hz

     ↓

RENDERING
Variable FPS
```

---

# 51. CLIENT PREDICTION

Local player movement should feel immediate.

Flow

```text
Local Input
     ↓
Immediate Local Prediction
     ↓
Input Sent To Host
     ↓
Host Simulation
     ↓
Authoritative State
     ↓
Client Reconciliation
```

LAN latency should generally be low enough that this system remains relatively simple.

---

# 52. REMOTE PLAYER INTERPOLATION

Remote players should be interpolated between received states.

Do not simply teleport them between network updates.

```text
STATE A
   ↓
INTERPOLATE
   ↓
STATE B
```

This produces smooth remote movement.

---

# 53. LAN DISCOVERY

When selecting

```text
LAN PLAY
```

the game should search for available hosts on the local network.

Example

```text
LAN GAMES

NEON GRID
Host SOHAM-PC
Players 24
Ping 3 ms

[JOIN]
```

---

# 54. DIRECT IP CONNECTION

Provide a fallback

```text
JOIN BY IP
```

Example

```text
HOST IP

192.168.1.42

PORT

[CONNECT]
```

This is important for networks where automatic discovery fails.

---

# 55. HOST LOBBY

```text
HOST LAN GAME

SOHAM-PC

P1  SOHAM       READY
P2  ALEX        READY
P3  JORDAN      READY
P4  WAITING

MODE
SURVIVAL

ARENA
THE GRID

FIRST TO
5

POWERUPS
ON

[START MATCH]
```

---

# 56. CLIENT LOBBY

```text
NEON GRID

SOHAM-PC

P1  SOHAM       READY
P2  YOU         READY
P3  JORDAN      READY
P4  WAITING

COLOR
GREEN

[READY]
```

---

# 57. DISCONNECT HANDLING

If a client disconnects

```text
PLAYER 3 DISCONNECTED
```

The host removes the player from the active match.

For MVP

Do not replace disconnected players with AI.

The player simply leaves the match.

---

# 58. HOST DISCONNECT

Host migration is not required for MVP.

If the host leaves

```text
HOST DISCONNECTED

MATCH ENDED

[RETURN TO MENU]
```

Host migration may be implemented later.

---

# 59. NETWORK SECURITY

Even though this is LAN-only

Never trust clients with authoritative state.

Clients send

 I am steering left.

They do not send

 I moved here and killed Player 3.

The host validates and simulates the result.

---

# 60. MAIN MENU

```text
NEON GRID

SOLO
LAN PLAY
OPTIONS
CREDITS
QUIT
```

---

# 61. SOLO MENU

```text
SOLO

AI PLAYERS
1
2
3

DIFFICULTY
EASY
NORMAL
HARD
EXPERT

ARENA
THE GRID

[START]
```

---

# 62. LAN MENU

```text
LAN PLAY

HOST GAME
FIND GAMES
JOIN BY IP

[BACK]
```

---

# 63. MATCH SETTINGS

Host can configure

```text
Mode
Arena
AI count (solo only)
Score limit
Powerups ONOFF
Powerup frequency
```

Avoid exposing settings that aren't actually implemented.

---

# 64. VISUAL STYLE

The game should have an original futuristic visual identity.

Target

 Dark digital arena + luminous vehicles + high-speed motion + neon energy.

Use

- Dark environments
- Grid surfaces
- Atmospheric haze
- Bloom
- Neon trails
- Bright vehicle cores
- Subtle holographic UI

Avoid copying specific movie assets.

---

# 65. VEHICLE DESIGN

Vehicles should be

- Small
- Fast-looking
- Low-profile
- Highly readable from the camera
- Visually distinct by color

The vehicle should visually communicate its heading.

---

# 66. LEAN VISUALS

When steering

```text
Vehicle
   ↓
Rotates
   ↓
Leans
   ↓
Trail curves
```

The stronger the steering input, the stronger the visual lean.

Example

```text
Small steering
   → 5° lean

Medium steering
   → 12° lean

Maximum steering
   → 20° lean
```

The values are tuning targets.

---

# 67. CAMERA

Use a slightly angled top-down camera rather than a completely flat orthographic camera if the visual presentation benefits from it.

The camera should prioritize

- Arena visibility
- Trail readability
- Vehicle readability
- Powerup visibility

Do not use cinematic camera movement during gameplay.

Competitive clarity comes first.

---

# 68. CAMERA ZOOM

For MVP

Fixed camera.

Future option

Slight dynamic zoom based on active player distribution.

Do not dynamically follow one player and hide the rest.

---

# 69. HUD

Example

```text
┌───────────────────────────────────────┐
│ P1  4              ROUND 7        P2 3│
│                                       │
│                                       │
│                ARENA                  │
│                                       │
│                                       │
│ P3  2                            P4 1 │
│                                       │
│              [ SHIELD ]               │
└───────────────────────────────────────┘
```

The HUD should show

- Scores
- Round
- Powerup
- Powerup state
- Player colors

---

# 70. POWERUP FEEDBACK

Every powerup needs

### Visual identity

Each type has a unique iconcoloreffect.

### Audio identity

Pickup and activation sounds differ.

### World effect

The vehicle changes visually while active.

This allows players to understand what's happening without reading text.

---

# 71. AUDIO

Required sounds

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

---

# 72. MUSIC

Music should dynamically increase in intensity as the arena becomes more dangerous.

Possible structure

```text
ROUND START
     ↓
Low intensity

Few players remain
     ↓
Higher intensity

Final 2 players
     ↓
High intensity

Final confrontation
     ↓
Maximum intensity
```

This can be implemented later.

---

# 73. PARTICLE EFFECTS

Use particles for

- Trail sparks
- Steering
- Boost
- Powerup pickup
- Shield impact
- Phase
- EMP
- Trail Cutter
- Vehicle destruction
- Match victory

Effects should never obscure collision geometry.

---

# 74. SCREEN SHAKE

Use very small screen shake for

- Major collisions
- EMP activation
- Powerup activation
- Final elimination

Do not overuse it.

---

# 75. PROJECT ARCHITECTURE

Suggested

```text
src

core
    GameManager
    MatchManager
    RoundManager
    GameState

player
    PlayerController
    PlayerMovement
    PlayerInput
    PlayerTrail
    PlayerCollision
    PlayerState

ai
    AIController
    AINavigation
    AIDecision
    AIDifficulty

gameplay
    CollisionSystem
    ScoreSystem
    SpawnSystem
    ArenaSystem
    PowerupSystem

powerups
    PowerupBase
    BoostPowerup
    ShieldPowerup
    PhasePowerup
    EMPPowerup
    TrailCutterPowerup
    OverdrivePowerup

network
    NetworkManager
    HostManager
    ClientManager
    LobbyManager
    LANDiscovery
    ConnectionManager
    NetworkMessages
    NetworkGameState

ui
    MainMenu
    SoloMenu
    LANMenu
    HostLobby
    ClientLobby
    HUD
    PowerupUI
    Countdown
    ResultsScreen

audio
    AudioManager
    SFXManager
    MusicManager

effects
    TrailEffect
    LeanEffect
    BoostEffect
    PowerupEffects
    DeathEffect

data
    GameConfig
    PlayerConfig
    ArenaConfig
    PowerupConfig
    GameModeConfig
```

Exact names may change according to engine.

The separation of responsibilities should remain.

---

# 76. GAME STATE MACHINE

```text
BOOT
 ↓
MAIN_MENU
 ├── SOLO
 │     ↓
 │   SOLO_LOBBY
 │     ↓
 │   COUNTDOWN
 │     ↓
 │   PLAYING
 │
 └── LAN
       ↓
     LAN_MENU
       ↓
     HOST  SEARCH  JOIN
       ↓
     LAN_LOBBY
       ↓
     COUNTDOWN
       ↓
     PLAYING

PLAYING
 ↓
ROUND_RESULTS
 ↓
NEXT_ROUND
 ↓
MATCH_RESULTS
 ↓
LOBBY  MENU
```

---

# 77. DATA-DRIVEN CONFIGURATION

Use configuration objects rather than hard-coded values.

Example

```text
GameConfig

maxPlayers = 4
scoreLimit = 5

baseSpeed = 12

maxTurnRate = 120

maxLeanAngle = 20

powerupsEnabled = true
maxActivePowerups = 3

countdownDuration = 3
```

These are starting values.

They should be easy to modify.

---

# 78. POWERUP CONFIGURATION

Each powerup should have its own configuration.

Example

```text
Boost

speedMultiplier
duration
cooldown
```

```text
Shield

charges
duration
```

```text
Phase

duration
```

```text
EMP

range
duration
effectStrength
```

This makes balancing much easier.

---

# 79. DEVELOPMENT PHASE 1 — MOVEMENT

Build only

- Arena
- Vehicle
- Continuous movement
- Steering
- Turn rate
- Speed
- Vehicle rotation
- Visual leaning
- Boundary collision

No networking.

No powerups.

No fancy UI.

### Goal

The vehicle should feel excellent.

If driving isn't fun here, stop and fix it before continuing.

---

# 80. DEVELOPMENT PHASE 2 — TRAILS

Add

- Trail creation
- Trail rendering
- Trail collision
- Self-collision
- Other-player collision
- Death

### Goal

The core game works.

---

# 81. DEVELOPMENT PHASE 3 — SOLO AI

Add

- AI controller
- Basic navigation
- Collision avoidance
- Trail awareness
- AI elimination
- AI difficulty

### Goal

One player can play a complete offline match.

---

# 82. DEVELOPMENT PHASE 4 — POWERUPS

Add

- Powerup spawning
- Pickup
- Inventory
- Activation
- Effects
- Host-independent local logic for solo mode
- Balancing

Start with

1. Boost
2. Shield
3. Phase
4. Trail Cutter

Then add

5. EMP
6. Overdrive

if they improve the game.

---

# 83. DEVELOPMENT PHASE 5 — ROUNDMATCH SYSTEM

Add

- Countdown
- Round start
- Round end
- Scoring
- First-to-5
- Match victory
- Rematch

### Goal

A complete offline match exists.

---

# 84. DEVELOPMENT PHASE 6 — LAN NETWORKING

Implement

- Host
- Client
- LAN discovery
- Direct IP
- Lobby
- Player synchronization
- Input synchronization
- Trail synchronization
- Powerup synchronization
- Collision synchronization
- Death synchronization
- Score synchronization
- Round synchronization
- Match synchronization

### Goal

Four separate PCs can play a complete match.

---

# 85. DEVELOPMENT PHASE 7 — NETWORK POLISH

Add

- Client prediction
- Remote interpolation
- Ping display
- Connection status
- Better reconciliation
- Network error messages
- Disconnect handling

---

# 86. DEVELOPMENT PHASE 8 — VISUAL POLISH

Add

- High-quality materials
- Bloom
- Neon lighting
- Atmospheric effects
- Vehicle models
- Trail effects
- Powerup effects
- Particles
- UI animation
- Screen shake

---

# 87. DEVELOPMENT PHASE 9 — AUDIO

Add

- Music
- SFX
- Dynamic music
- Vehicle audio
- Powerup audio
- Collision audio
- Victory audio

---

# 88. DEVELOPMENT PHASE 10 — ADDITIONAL CONTENT

Possible additions

- More arenas
- New powerups
- More AI personalities
- Team mode
- Territory mode
- Tournament mode
- Vehicle customization
- Cosmetics
- Advanced arena mechanics

---

# 89. TESTING REQUIREMENTS

## Offline

Test

```text
Player vs 1 AI
Player vs 2 AI
Player vs 3 AI
```

## LAN

Test

```text
Host + 1 client
Host + 2 clients
Host + 3 clients
```

---

# 90. LAN TESTING

Test

```text
Game discovery
Direct IP
Joining
Leaving
Disconnect during lobby
Disconnect during round
Disconnect between rounds
Host shutdown
Invalid IP
Full lobby
No games found
```

The game should provide explicit error messages.

Never silently fail.

---

# 91. POWERUP TESTING

Every powerup must be tested for

- Pickup
- Activation
- Expiration
- Collision interaction
- Death interaction
- Multiple players
- Network synchronization
- Disconnect scenarios

---

# 92. PERFORMANCE

Target

60+ FPS

on modern gaming PCs.

The game should remain performant with

- Four players
- Long trails
- Multiple powerups
- Particles
- Bloom
- Network synchronization

Avoid unnecessary physics objects.

---

# 93. NETWORK PERFORMANCE

LAN should prioritize

- Low latency
- Smooth movement
- Minimal bandwidth
- Authoritative state
- Reliable event synchronization

The host should not continuously transmit enormous trail meshes.

Transmit compact trail geometryevents where possible.

---

# 94. NETWORK AUTHORITY

The host controls

```text
Player position
Player heading
Trail creation
Collision
Deaths
Powerups
Scores
Rounds
Match
```

Clients control

```text
Input
Rendering
Audio
UI
Local prediction
```

---

# 95. VIBE-CODING RULES

The AI coding agent must follow these rules.

### Rule 1

Do not rewrite functioning systems unnecessarily.

### Rule 2

Inspect the current project before modifying it.

### Rule 3

Implement one feature at a time.

### Rule 4

Buildtest after major changes.

### Rule 5

Do not introduce unnecessary dependencies.

### Rule 6

Do not mix networking logic into visual systems.

### Rule 7

Do not mix AI movement logic with the base player movement system.

### Rule 8

Do not create individual physics objects for every trail segment.

### Rule 9

Do not implement future systems before the current milestone works.

### Rule 10

Maintain this specification as the source of truth.

---

# 96. AI DEVELOPMENT LOOP

The coding agent should follow

```text
READ SPECIFICATION
       ↓
INSPECT PROJECT
       ↓
CHECK CURRENT MILESTONE
       ↓
IMPLEMENT SMALLEST CHANGE
       ↓
BUILD
       ↓
TEST
       ↓
FIX
       ↓
VERIFY
       ↓
UPDATE TODO
       ↓
NEXT FEATURE
```

Do not generate the entire game in one enormous code generation step.

---

# 97. DOCUMENTATION

Maintain

```text
GAME_DESIGN.md
```

Contains

- Game rules
- Movement
- Powerups
- Networking
- Modes
- Architecture
- Current features

Also

```text
TODO.md
```

Structured as

```text
DONE

IN PROGRESS

NEXT

BACKLOG

KNOWN ISSUES
```

---

# 98. MVP DEFINITION OF DONE

## Core movement

- [ ] Continuous movement
- [ ] Continuous steering
- [ ] Speed system
- [ ] Turn radius
- [ ] Vehicle rotation
- [ ] Vehicle leaning
- [ ] Boundary collision

## Trails

- [ ] Trail generation
- [ ] Trail rendering
- [ ] Trail collision
- [ ] Self collision
- [ ] Other-player collision
- [ ] Efficient trail representation

## Solo

- [ ] AI players
- [ ] AI movement
- [ ] AI collision avoidance
- [ ] AI difficulty
- [ ] Complete offline match

## Powerups

- [ ] Powerup spawning
- [ ] Pickup
- [ ] Inventory
- [ ] Activation
- [ ] Boost
- [ ] Shield
- [ ] Phase
- [ ] Trail Cutter
- [ ] EMP
- [ ] Overdrive

## Match

- [ ] Countdown
- [ ] Rounds
- [ ] Score
- [ ] First-to-5
- [ ] Match victory
- [ ] Rematch

## LAN

- [ ] Host
- [ ] Client
- [ ] LAN discovery
- [ ] Direct IP
- [ ] Lobby
- [ ] Player synchronization
- [ ] Input synchronization
- [ ] Trail synchronization
- [ ] Powerup synchronization
- [ ] Collision synchronization
- [ ] Death synchronization
- [ ] Score synchronization
- [ ] Round synchronization
- [ ] Disconnect handling

## Presentation

- [ ] Main menu
- [ ] Solo menu
- [ ] LAN menu
- [ ] Lobby
- [ ] HUD
- [ ] Powerup UI
- [ ] Results
- [ ] Neon visuals
- [ ] Basic audio
- [ ] Particles

---

# 99. POST-MVP ROADMAP

```text
CORE MVP
    ↓
MOVEMENT POLISH
    ↓
POWERUP BALANCING
    ↓
MORE ARENAS
    ↓
MORE AI PERSONALITIES
    ↓
TEAM MODE
    ↓
TERRITORY MODE
    ↓
ADVANCED ARENA MECHANICS
    ↓
VEHICLE CUSTOMIZATION
    ↓
TOURNAMENT MODE
    ↓
POSSIBLE ONLINE MULTIPLAYER
```

---

# 100. FUTURE ONLINE ARCHITECTURE

The networking abstraction should eventually permit

```text
                  Network Interface
                         │
          ┌──────────────┼──────────────┐
          │              │              │
        OFFLINE          LAN          ONLINE
```

The gameplay code should not care whether the network transport is

- Local simulation
- LAN
- Internet

---

# 101. FINAL DESIGN PHILOSOPHY

Neon Grid should not become a game where the player spends more time managing abilities than driving.

The hierarchy should be

```text
MOVEMENT
   ↓
TRAIL CONTROL
   ↓
POSITIONING
   ↓
POWERUPS
   ↓
ADVANCED STRATEGY
```

Powerups are there to create openings.

They should not replace driving skill.

---

# 102. THE FEEL WE ARE AIMING FOR

The ideal experience is

You join a LAN lobby from another PC.

```text
NEON GRID
SOHAM-PC
34 PLAYERS
```

You choose your vehicle color.

The arena loads.

```text
3
2
1
GO
```

Your vehicle immediately accelerates.

You lean into a turn.

Your trail bends behind you.

Another player is approaching from the opposite direction.

You have three options

```text
TURN EARLY
BOOST THROUGH
TAKE THE POWERUP
```

You take the powerup.

It's Phase.

Another player has created a wall directly ahead.

You activate Phase.

Your vehicle turns translucent and passes through the trail.

But you are now moving too quickly.

You steer harder.

The vehicle leans aggressively.

You barely clear another trail.

Your opponent follows your path.

You make a sharp turn.

They don't have enough space.

CRASH.

The arena gets quieter.

Only two vehicles remain.

The trails have turned the arena into a maze of glowing walls.

You have a Shield.

Your opponent has Boost.

Neither player knows what the other is going to do.

Then

```text
FINAL TWO
```

The music intensifies.

You make the first move.

The opponent follows.

The entire arena becomes a battle over the last remaining open space.

That's the game.

---

# 103. CORE RULE FOR DEVELOPMENT

When deciding whether a new feature belongs in Neon Grid, ask

 Does this make movement, positioning, trail control, or multiplayer more interesting

If yes, consider it.

If no, it probably belongs in the backlog.

The game should remain centered around one exceptionally strong idea

# MOVE FAST. CONTROL SPACE. OUTMANEUVER EVERYONE.