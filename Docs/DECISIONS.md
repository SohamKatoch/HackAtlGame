# Neon Grid — Decisions

Recorded so later work does not silently pick a different game. The guide remains [Reference/ORIGINAL_GAME_GUIDE.md](Reference/ORIGINAL_GAME_GUIDE.md). These decisions interpret conflicts and underspecified engine choices. They do not delete guide requirements.

---

## D1 — Neon Grid v2 overrides the generic racing brief

**Status:** Accepted

The implementation brief asked for an arcade racer with racing lines, throttle, braking, lap-style progression, and local multi-human play. The filled specification is a trail-combat arena.

Accepted interpretation:

- No racing line, laps, finish line, or brake model.
- "Race progression" means Survival rounds and first-to-5.
- Overtaking means cutting an opponent off with trails and position.
- Recovery means escaping a trap with a legal arc or a powerup.
- Throttle is not an axis. Speed is cruise unless Boost or Overdrive is active.

## D2 — Multiple humans means LAN, one device each

**Status:** Accepted

The brief asked for multiple human players. Guide section 5 forbids shared-screen, split-screen, and multiple local input devices.

LAN is the multiplayer mode. The template's `bUseSplitscreen=True` is leftover and will be turned off in milestone 1. Do not implement couch co-op.

## D3 — Kinematic heading, not Chaos vehicles

**Status:** Accepted

Movement is `velocity = direction(heading) * speed` with a speed-dependent turn rate. Lean is visual and must not change the collision primitive.

A wheeled physics vehicle would add throttle, brake, and suspension the design does not use, and it would fight the fixed turn radius. Use a custom movement component.

## D4 — Constant cruise speed for the base model

**Status:** Accepted

Guide section 17 allows constant base speed for the MVP and describes a future acceleration curve. Boost and Overdrive still change speed before that curve exists.

Overdrive's acceleration increase is implemented as an immediate speed multiplier plus a stronger turn-rate penalty until a real acceleration model is justified. That preserves the risk tradeoff without a second movement mode.

The guide's `baseSpeed = 12` is a tuning token. Unreal centimeters would make 12 cm/s unplayable, so milestone 1 chooses a world speed that crosses The Grid in a few seconds and stores it in the Data Asset. The feel tests matter more than the literal number 12.

## D5 — EMP reduces steering, and does not stop the vehicle

**Status:** Accepted for MVP

The guide allows one of three effects: disable the victim's powerup, reduce maneuverability, or disrupt their HUD. It forbids disabling movement.

MVP effect: for a short duration, multiply the victim's turn rate down. They keep full speed and can still steer, only less sharply. Range, duration, and strength are Data Asset fields.

Not in MVP, and not combined on top:

- Stripping or disabling the victim's held powerup
- Scrambling their HUD

Those remain rejected alternatives unless playtests show steering reduction is invisible. A change requires a new decision. EMP still does not freeze or halt the pawn.

## D6 — All six powerups stay in scope

**Status:** Accepted

Section 82 says to add EMP and Overdrive after the first four "if they improve the game." Section 98's definition of done includes all six.

Resolution: all six ship in v2. EMP and Overdrive are steps 6 and 7 of the powerup milestone, not a cut list. "If they improve the game" informs balance passes, not deletion.

## D7 — One powerup slot

**Status:** Accepted

A second slot is explicitly a future option. Pickup while holding is refused.

## D8 — Listen server and UE replication, not a custom protocol

**Status:** Accepted

The guide lists message names and a host-authoritative sim. Unreal's listen server, replicated properties, and RPCs already provide that if gameplay never trusts client transforms.

A handwritten UDP stack would delay a playable LAN match and would still need prediction, sessions, and disconnect handling. Map the guide's message names onto UE RPCs and replicated state, behind a small "submit input / apply state" boundary so a later online transport can replace the calls.

Direct IP join stays mandatory because LAN session browse fails on some networks.

## D9 — Trail segments and a spatial hash, not bodies

**Status:** Accepted

One physics object per segment will not scale and violates the guide. Collision is a swept segment query against a grid hash. The ribbon is cosmetic.

Start with a uniform grid. Switch to a quadtree only if the grid is measured to be too slow.

## D10 — AI is input generation

**Status:** Accepted

Same pawn and movement as humans. Difficulty changes timing and decision quality only. Personalities are a post-MVP parameter layer, not a milestone 3 requirement.

LAN matches do not backfill disconnects or empty slots with AI. Solo is where AI count is chosen.

## D11 — Phase and Shield exceptions

**Status:** Accepted

- Phase ignores trails and does not ignore the arena boundary.
- Vehicle-versus-vehicle contact still eliminates during Phase. The guide does not grant vehicle pass-through.
- Shield is one charge per activation. The saved hit still plays strong feedback. The next hit kills.

## D12 — Camera

**Status:** Accepted

Fixed, slightly angled top-down view of the whole arena for v2. No cinematic camera. No follow cam that hides other players. Dynamic zoom based on living players is future.

## D13 — Template leftovers

**Status:** Accepted

`MyProject` is a 5.8 first-person demo: intro room, `BP_FirstPersonGameMode`, split-screen enabled, Lumen and ray tracing on. Neon Grid does not extend the first-person character.

Milestone 1 adds a game module and retargets the default map and mode. Template content can remain on disk until something needs the space. Gameplay must not depend on Lumen hardware ray tracing to read trails. Bloom and emissive materials are the intended look. Target is 60 FPS or higher with four players and long trails.

## D14 — Blender sources stay outside Content

**Status:** Accepted

Custom meshes are authored in Blender, exported, then imported. `.blend` files live under something like `ArtSource/Blender/`, not `Content`. Vehicle gameplay collision is the code primitive, not the imported mesh collision. Placeholder meshes are correct until the visual milestone.

## D15 — No host migration, no silent network failure

**Status:** Accepted

If the host leaves, the match ends and clients return to the menu. Dropped clients are removed and not replaced. Every failed join, full lobby, bad IP, and disconnect shows a message.

## D16 — Prediction is real but small

**Status:** Accepted

The owning client runs the same movement step immediately and reconciles to the host. Remote pawns interpolate. This is required by the guide. It is not a license to adopt a large netcode plugin before a two-PC match exists. LAN latency is the reason to keep reconciliation short.

## D17 — Documentation split

**Status:** Accepted

The guide asked for a single `GAME_DESIGN.md` that also covered networking and architecture, plus `TODO.md`. The project brief asked for separate design, technical, multiplayer, AI, vehicle, plan, and decision docs.

Resolution: `GAME_DESIGN.md` is the player-facing design. The other docs hold implementation and are linked from it. `TODO.md` exists so the guide's tracking requirement is kept. Nothing in the guide was dropped by the split. The verbatim guide is `Docs/Reference/ORIGINAL_GAME_GUIDE.md`. `NEON GRID v2.txt` was not edited.

## D18 — Offline authority is the same code as the host

**Status:** Accepted

Solo is not a second simulation. It is a match with AI controllers and no remote connections. Powerup and collision rules are not reimplemented for offline play.

## D19 — Trail contact rules for Milestone 2

**Status:** Accepted

- Any trail kills, including your own (guide: "touching any trail eliminates you").
- Own-trail grace: segments the vehicle updated within the last `SelfTrailGraceTicks` (10 ticks ≈ 0.17 s) are ignored, and the trail is emitted `TrailRearOffset` (70 cm) behind the vehicle center. Without both, a vehicle collides with the segment it is drawing. The grace is by tick, not distance, so it is deterministic and the same on host and client.
- Vehicle-vehicle contact eliminates both vehicles on the same tick. The guide says collision with a vehicle eliminates the player; it does not say the other survives. Both dying is the symmetric reading and avoids a tick-order advantage.
- A dead vehicle's trail stays as an obstacle for the rest of the round. The guide says "the trail should remain behind the vehicle after it passes" and is silent on what happens at death. Keeping it is the smaller implementation and matches the Tron-style reference. If the round design later wants dead trails to fade, that is a one-line change in `Eliminate` plus a renderer event, and should be recorded as a new decision.
- Trail segments are collision data only. The renderer mirrors them from events and is never consulted for hits.
- Trail recording happens after a step is accepted, so a vehicle that dies does not leave a segment through the thing that killed it.

## D20 — Camera stays fixed and oblique

**Status:** Superseded by D21

First feedback asked for "third person without third person". Interpreted as a more oblique fixed camera, not a follow cam. The guide requires every player to see the whole arena from one shared view (sections 67–68), and a follow cam would break that for local presentation and future spectating. `CameraPitch` went from −70° to −55°, with distance still auto-fit to the arena. If the user wanted a camera that tracks their own vehicle, that is a design change and needs an explicit decision here.

## D21 — Follow camera during play, by the user's explicit decision

**Status:** Accepted (user decision, overrides guide sections 67–68)

The user asked twice for a camera directly behind the vehicle, following it, with the whole-arena view dropped. The conflict with the guide ("Fixed camera", "Do not dynamically follow one player and hide the rest") was raised and the user reaffirmed, so this is the design.

- `ANeonVehiclePawn` carries a spring arm (inherit yaw only, position and rotation lag, no collision test) and a camera. Tunables on the pawn: `CameraDistance` 650, `CameraHeight` 190, `CameraPitchDown` 12°, `CameraFieldOfView` 85, lag speeds 14 / 9.
- The camera is cosmetic and local. The simulation never reads it.
- `ANeonArena` keeps its fixed whole-arena camera as the main menu backdrop and a future spectator view (D24).
- Consequences accepted: a player cannot see the whole arena while driving; an eliminated player keeps watching from their parked vehicle until the round ends; on LAN each client views its own vehicle. If a spectator mode is wanted later, the arena camera is the obvious target.

## D22 — AI is a deterministic plan-sampler on the real collision data

**Status:** Accepted

- `ANeonAIController` derives from `AController`, not `AAIController`: no navmesh, perception or behavior tree is needed and the AI stays a plain input generator (D10). It still requests an `ANeonPlayerState` so scores and the HUD treat it like any player.
- A plan is "steer at a fixed value until the heading has changed by Δ, then straight". Every decision samples straight plus ±{0.5, 1.0} × {90, 45, 135, 30, 180}° (count by difficulty), predicts each with the vehicle's own turn-rate formula for `LookAheadTime`, and scores: survive bonus, free fraction, openness and escape probes at the plan end, a flood-fill **territory** fraction on a 250 cm occupancy grid (trails and vehicles, dilated one cell), cut-off alignment to the nearest opponent, minus steering cost and loop aversion, plus a commit bonus only for continuing the plan already committed. If no plan survives, full-steer 180° and 135° turns are also tried.
- Lethality of a predicted step uses the same collision system and own-trail grace as the real step. Opponents are extrapolated with their current steering (constant curvature) for at most `OpponentExtrapolationTime` and their future path counts as trail.
- Randomness is a seeded `FRandomStream` (`AISeed + slot`), so a seed replays identically given the same inputs. `MistakeChance` picks a random surviving plan instead of the best.
- Difficulty is data (`UNeonAIConfig`, four levels, class defaults until an asset is authored). Powerup use (`PowerupSkill`) is a parameter with no behavior until Milestone 4.
- Measured headlessly (sim ticks at 60 Hz, 14000 × 9000 arena, Normal): lone AI median ≈ 36–41 s before hitting its own trail; three AI in a match median ≈ 13 s. Both were ≈ 7 s in the 8000 × 5000 arena before the territory metric.

## D23 — The Grid is 14000 × 9000

**Status:** Accepted (user request: "too small for 4 bikes")

`ANeonArena::HalfExtents` is (7000, 4500), spawns inset 65 % / 55 %, so the head-on pairs start 9100 cm apart. At 2200 cm/s the long axis takes ≈ 6.4 s, which gives four vehicles room for long turns and the AI room to plan. The size is a single property; the arena camera, spawns, collision hash and AI grid derive from it. "Dynamic" arena behavior (anything that changes the playable space during a round) is **not** decided; it is a design change against the guide's "simple" first arena and is waiting on the user (see TODO).

## D24 — Menus live in the arena level

**Status:** Accepted

The main menu and solo menu are widgets shown over `L_TheGrid` viewed through the arena's fixed camera, not a separate menu map. No vehicles exist in the `MainMenu` phase; starting a match spawns them and blends to the follow camera. This avoids a second map, level travel, and settings hand-off for the solo flow; `UNeonGameInstance` carries `FNeonMatchSettings` anyway so the LAN lobby can reuse it. Guide section 60 entries that do not exist yet (LAN PLAY, OPTIONS, CREDITS) are shown disabled rather than hidden, per section 63's rule against exposing unimplemented settings. Widgets are built in C++ with `BindWidgetOptional` members so a Blueprint subclass can restyle them without touching the logic.

## D25 — The Grid is a 2.5D stepped pyramid; the simulation stays flat

**Status:** Accepted (user request: "add a bit of dimensionality to the arena or verticality… a 3D pyramid"; "make the map… dynamic")

The user asked for verticality and named a pyramid. Of the options (cosmetic height; 2.5D heightfield; interior obstacles; shrinking boundary; full 3D surface driving), the 2.5D heightfield is the only one that delivers a visible pyramid without touching movement, trails, collision, AI or the future network model.

- `ANeonArena` owns a height function `GetFloorHeight(XY)`: `TierCount` (3) concentric rectangular plateaus toward the center, each `TierHeight` (140 cm) higher, `TierSpacing` (1200 cm) apart, joined by `RampWidth` (450 cm) smoothstep bands. It is continuous, so Z is a pure function of XY. `GetFloorNormal` is finite differences of it.
- The floor is a `UProceduralMeshComponent` heightfield generated from that function at `TerrainResolution` (100 cm), plus a thin lit lip along every plateau edge so the tiers read from the low follow camera. No collision on any of it.
- `UNeonMovementComponent` still integrates X, Y and heading only. `SetArena` gives it the height source; `ApplyInterpolatedTransform` writes Z = floor + spawn offset and a cosmetic pitch/roll from the normal (`bAlignToFloor`). Nothing in `StepSimulation` or `CheckHazards` reads height.
- `UNeonTrailRendererComponent` draws each 2D segment as chunks no longer than `ChunkLength` (200 cm), each pitched between its two end heights. Chunks are only appended, so ISM indices stay stable. Collision segments are unchanged.
- The follow camera inherits yaw only, so ramps never tilt the view.
- Spawns sit on the first plateau (inset 65 % / 60 %).
- Consequences: a vehicle on a higher tier does not gain or lose anything mechanically; the terrain is readability and feel. "Dynamic" is interpreted as a shaped, non-flat arena, not as geometry that changes during a round. A shrinking boundary or moving terrain would be a new mechanic against the guide's "simple" first arena and needs its own decision. If true 3D surface driving is ever wanted, this is the wrong foundation and the movement/trail/collision stack would be rewritten.

## D26 — Powerup rules the guide leaves open

**Status:** Accepted (Milestone 4)

The guide names six powerups (D6), one slot (D7), manual activation, and EMP as a steering-rate cut (D5). It does not say what each one does at the edges. These are the calls made while implementing them; all are tunable on `UNeonPowerupConfig`.

- **Activation is latched on the simulation tick.** The player controller and the AI both write `bActivatePowerup` into `FNeonVehicleInput`; `UNeonMovementComponent::SetInput` latches the rising edge and `StepSimulation` consumes it in `UNeonPowerupComponent::PreStep`. Humans, AI and (later) LAN clients activate through one path, and effects are counted in ticks, so a replayed input stream reproduces them.
- **A refused activation keeps the powerup.** Trail Cutter with nothing in reach and EMP with nobody in range do nothing and stay in the slot. Nothing is spent on a miss.
- **Shield absorbs trail hits only** (one charge by default) and then grants `ShieldPassThroughTicks` of trail immunity so the vehicle clears the trail it just hit instead of dying on the next tick. The boundary and vehicle contact still kill. With `ShieldDuration = 0` it stays armed until used.
- **Phase ignores trails, not the boundary or vehicles.** The vehicle keeps laying its own trail while phased.
- **Trail Cutter cuts any owner's trail**, including the user's own, at the first trail along the heading within `TrailCutterReach`; the hole is `TrailCutterLength` wide, centered on the hit. Cut segments are flagged removed and the remaining pieces re-registered, so segment indices stay stable for the renderer and the AI grid.
- **EMP hits the nearest alive opponent within `EMPRange`**, multiplying their steering rate by `EMPSteeringMultiplier` for `EMPDuration`. Speed is untouched (D5). Only one target per use.
- **Boost / Overdrive multiply speed and reduce turn rate** so a faster vehicle has a wider turning circle; the AI's prediction ignores the multiplier (noted in TODO).
- **Pickups are 2D.** The spawner resolves collection by distance in the simulation plane (`PickupRadius + CollisionRadius`); the actor is presentation only. Spawn points come from a lattice filtered for trail, vehicle and pickup clearance; the type and point are drawn from a seeded `FRandomStream`.
- **A full slot refuses the pickup**, which stays on the floor for someone else.
- **AI**: an empty slot adds a `CollectWeight` alignment term toward the nearest pickup in `CollectRange`; a held powerup is used when the moment fits its type (defensive ones under threat, speed ones on an open straight, EMP with a target in range), taken with probability scaling on `PowerupSkill`; low-skill AI also wastes powerups at random.
- **Powerups can be turned off** in the Solo menu (`FNeonMatchSettings::bPowerupsEnabled`) for a classic match.
- The debug Boost on Left Shift is gone; Space / RT / gamepad A is the activate key.

## D27 — Bigger Grid, slower bikes, closing walls, death camera

**Status:** Accepted (playtest: map too small / not dynamic, rounds too fast, camera stuck on the wreck)

- The Grid is **24000 × 16000** (`HalfExtents` 12000 × 8000). At 1400 cm/s the long axis is about 17 s.
- Cruise / min speed is **1400 cm/s** (was 2200, then 1700). Turn rate stays 180°/s so the bike still corners.
- **Closing walls** are on by default. After 8 s of play the rectangle interpolates to 8000 × 5200 over 40 s. `GetBounds()` is live; a vehicle caught by the wall dies (`Boundary`). Solo menu can turn this off. Walls pulse orange while moving.
- **Spectator camera:** on death the local follow camera is disabled and the view blends onto a living opponent. A / D (or left / right) cycles survivors. With nobody left, the arena camera. HUD: "WATCHING P#".
- AI look-ahead and loop aversion were scaled with the slower speed. Easy now uses a territory term so it boxes itself less.

## D28 — LAN first slice is listen + IP, not discovery

**Status:** Accepted (Milestone 6 start)

The LAN menu Host reloads `L_TheGrid` with `?listen`. Join travels to a typed IPv4 (optional `:port`, default 7777). Session search, ready-up, and color pick land later. Same compiled project on every machine; Windows Firewall must allow the editor / packaged exe on a private network.
