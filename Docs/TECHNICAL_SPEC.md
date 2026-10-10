# Neon Grid — Technical Specification

This document describes how Neon Grid will be implemented in Unreal Engine 5. Player-facing rules are in [GAME_DESIGN.md](GAME_DESIGN.md). Networking detail is in [MULTIPLAYER_SPEC.md](MULTIPLAYER_SPEC.md). Opponent behavior is in [AI_SPEC.md](AI_SPEC.md). Handling and tuning fields are in [VEHICLE_SPEC.md](VEHICLE_SPEC.md).

Do not build future systems before the current milestone in [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) works.

---

## 1. Current project

| Item | State |
| --- | --- |
| Project | `MyProject/MyProject.uproject` |
| Engine | Unreal Engine 5.8 |
| Code | Blueprint-only. No game C++ module yet. |
| Default map | `/Game/DemoTemplate/_Core/Lvl_IntroRoom` |
| Default game mode | `BP_FirstPersonGameMode` |
| Split-screen | `bUseSplitscreen=True` in `DefaultEngine.ini` |
| Gameplay | None. The first-person template is leftover content. |

The first implementation milestone must add a C++ game module, point the default map and game mode at Neon Grid, and turn split-screen off. Those Editor and project-file steps are manual work to call out when that milestone starts. This document does not perform them.

Leave template maps and the first-person character in place until a milestone needs the space. Do not build Neon Grid on `CharacterMovement` or the first-person pawn.

---

## 2. Language split

Use C++ for reusable gameplay:

- Fixed-step movement
- Trail storage, spatial lookup, and swept collision
- Match, round, and score state
- Powerup rules and effects
- Replication of authoritative state and input RPCs

Use Blueprints for:

- Tuning defaults wired to Data Assets
- UI widgets and menu flow
- Materials, Niagara, audio cues, and camera presentation
- One-off event glue that does not own simulation

Use Primary Data Assets (or equivalent data-driven assets) for vehicle, AI difficulty, arena, powerup, and match parameters. Designers change numbers without code edits. Code reads the assets. Do not scatter the guide's starting numbers through gameplay code.

Do not add plugins or libraries for problems Unreal already solves (sessions, replication, UI, Niagara). Do not add a custom socket stack for v2.

---

## 3. Simulation

Simulation runs at a fixed **60 Hz**. Rendering runs at the display frame rate and interpolates visual state.

Each simulation tick:

1. Consume input for that tick (local player, AI, or replicated client input).
2. Integrate heading and position.
3. Append trail segments.
4. Test the movement sweep against bounds, trails, and vehicles.
5. Resolve powerup timers, pickups, and activations.
6. Apply eliminations and round transitions when the rules say so.

Do not run trail collision or death logic only in a variable `Tick`. Visual lean, VFX, and UI may use the render tick.

---

## 4. Unreal mapping

The guide's responsibility split stays. Names may follow Unreal types. Suggested ownership:

| Guide area | Unreal owner | Notes |
| --- | --- | --- |
| GameManager / GameState | `ANeonGameState` | Replicated match phase, scores, round index |
| MatchManager / RoundManager | `ANeonGameMode` (server only) | Flow, spawn, win checks. Not replicated as an actor clients call for rules. |
| PlayerState | `ANeonPlayerState` | Id, name, colors, score, alive, powerup id and state |
| PlayerController | `ANeonPlayerController` | Local input, HUD owner, ready-up. One local player. |
| PlayerMovement | `UNeonMovementComponent` on the pawn | Kinematic heading integration. Shared by humans and AI. |
| PlayerInput | Enhanced Input assets, applied by the controller | Steering and activate. See [VEHICLE_SPEC.md](VEHICLE_SPEC.md). |
| PlayerTrail | `UNeonTrailComponent` plus a match-owned trail registry | Segments are data, not actors. |
| PlayerCollision | Movement tick calls `UNeonCollisionSystem` | No per-segment rigidbodies. |
| AIController / AIDecision / AIDifficulty | `ANeonAIController` | Writes the same input struct the human writes. |
| PowerupBase and six types | `UNeonPowerupDefinition` Data Asset plus a small effect object | Spawned pickups are lightweight actors or a single spawner with replicated instances. |
| SpawnSystem / ArenaSystem | Arena Data Asset plus server spawn code | The Grid first. |
| ScoreSystem | Game mode + player state | First to 5. |
| Network managers | Listen server, GameMode, RPCs, replicated properties | See [MULTIPLAYER_SPEC.md](MULTIPLAYER_SPEC.md). Do not invent a parallel `HostManager` process. |
| UI | UMG widgets | Main, Solo, LAN, lobbies, HUD, powerup, countdown, results. |
| Audio / effects | MetaSound or cues, Niagara, a cosmetic lean component | No simulation authority. |
| GameConfig and related | Data Assets | Section 6. |

Keep networking code out of visual systems. Keep AI decisions out of the movement integrator. The movement component accepts an input snapshot. It does not know why the steering value exists.

---

## 5. Movement implementation

Authoritative rule:

```text
velocity = direction(heading) * speed
heading += steeringInput * turnRate(speed, powerupState) * dt
```

`steeringInput` is in the range -1 to 1.

Turn rate drops as speed rises, including Boost and Overdrive, so the turn radius grows. Combine max turn rate, a minimum speed, and the resulting radius so a single tick cannot reverse heading. Exact curve is data. Starting targets are in [VEHICLE_SPEC.md](VEHICLE_SPEC.md).

The collision primitive is a fixed shape on the pawn (a small box or capsule in the horizontal plane). The visible mesh is a child that rolls toward the steer direction and returns to neutral. Animating that child must not move the collision primitive.

MVP speed is constant `baseSpeed` except when Boost or Overdrive overrides it. Do not build a full acceleration model in the movement milestone. Leave a speed override hook so those powerups can set speed and turn-rate multipliers later.

Phase 1 collision is the arena boundary only. Trail and vehicle hits arrive with the trail milestone.

---

## 6. Trails and collision

### Segment

```text
start
end
ownerId
creationTick
```

Optional later fields: `energyState`, `powerupEffect`, `expirationTime`. Do not add them until a mechanic needs them.

Segments live in one registry on the server (and in the predicted local copy for the owning client). They are not `UStaticMeshComponent`s and not Chaos bodies.

### Query

Vehicle motion each tick is a swept segment (previous position to new position), thickened by the vehicle radius.

```text
movement segment
  -> spatial hash lookup of nearby trail segments
  -> segment-segment intersection (plus radius)
  -> hit
```

Also test the sweep against arena bounds and other vehicle sweeps or capsules.

Start with a uniform grid spatial hash. A quadtree is unnecessary until a grid hash is measurably too slow. Four players with long trails should stay cheap if each cell stores segment indices, not meshes.

### Rendering

A separate trail renderer builds a ribbon or spline mesh from the same points. Glow, bloom, particles, animation, and visual thickness belong here. Gameplay ignores visual width.

### Death

On a confirmed hit, the server marks the player dead, stops their trail, plays the elimination event, and checks for a round winner. Shield consumes a charge and suppresses that death once. Phase skips trail hits and still hits the boundary. Clients do not decide the death.

### Trail Cutter

The server deletes or splits a short run of segments and replicates the edit (removed ids, or a replacement segment list for that span). Do not rebuild and send the entire trail mesh.

---

## 7. Powerups

Server (or offline game mode, which is the same authority locally):

- Maintains the active pickup set, capped by `maxActivePowerups` (start at 3, allowed range 2–4).
- Spawns on a timer at authored points that are not inside trail geometry and not immediately beside a player.
- Assigns type, spawn time, and lifetime.
- Rejects a pickup if the player already holds a powerup.
- Applies activation only from a validated input bit.
- Expires effects by simulation tick.

Each type is a Data Asset plus a code effect. Suggested fields:

| Type | Fields |
| --- | --- |
| Boost | speed multiplier or target speed, duration, cooldown |
| Shield | charges (1), duration if the charge can expire unused |
| Phase | duration. Boundary collision stays on. |
| EMP | range, duration, steering-rate multiplier. Movement stays on. |
| Trail Cutter | length of trail removed, cooldown |
| Overdrive | speed multiplier, turn-rate multiplier, duration, cooldown |

Solo uses the same code path as the host with no remote clients. Do not write a second powerup implementation for offline play.

Balance goal: openings, not a comeback weapon that deletes the leader. There is no instant-kill powerup.

---

## 8. Match flow in code

`ANeonGameMode` owns the phase enum matching the guide:

`Boot`, `MainMenu`, `SoloLobby`, `LanMenu`, `LanLobby`, `Countdown`, `Playing`, `RoundResults`, `MatchResults`.

Countdown duration starts at 3 seconds. Round win goes to the last living player. First to `scoreLimit` (default 5) ends the match. Rematch returns to the lobby with scores reset and the same settings.

Implemented (offline): `ENeonMatchPhase` {MainMenu, Countdown, Playing, RoundResults, MatchResults} on `ANeonGameState` with a server-time phase deadline; `ANeonGameMode::StartSoloMatch / Rematch / ReturnToMainMenu`; `UNeonMovementComponent::SetFrozen` holds every vehicle outside Playing; `FNeonMatchSettings` on `UNeonGameInstance`. Menus are widgets over the arena level (D24). LAN menu/lobby phases are added with Milestone 6.

Disconnected humans are removed. They are not replaced by AI in v2.

---

## 9. Data Assets

Starting `GameConfig` values from the guide:

```text
maxPlayers = 4
scoreLimit = 5
baseSpeed = 12
maxTurnRate = 120
maxLeanAngle = 20
powerupsEnabled = true
maxActivePowerups = 3
countdownDuration = 3
```

Also store simulation rate (60), vehicle radius, minimum turn radius, lean response and return speeds, lean angles at small/medium/max steer (about 5, 12, and 20 degrees, with a cap around 15–25), powerup frequency, and per-type tables from section 7.

Arena config: bounds, spawn transforms and initial headings, pickup points.

AI difficulty config: reaction delay, look-ahead, powerup use chance, mistake rate. See [AI_SPEC.md](AI_SPEC.md).

---

## 10. Replication (summary)

Listen-server host authority. Clients send input, not positions or kills. Detail, prediction, interpolation, discovery, and failure messages are in [MULTIPLAYER_SPEC.md](MULTIPLAYER_SPEC.md).

Gameplay systems ask "am I the authority?" They do not branch on a custom transport type. That is the seam for a later online backend.

---

## 11. Presentation systems

Cosmetic only:

- Lean component on the visual mesh
- Trail ribbon, bloom, haze, neon materials
- Niagara listed in [GAME_DESIGN.md](GAME_DESIGN.md)
- Small camera shake on the events listed there
- Audio and, later, music intensity driven by alive-player count

Camera: the guide specified a fixed whole-arena view; by the user's explicit decision ([DECISIONS.md](DECISIONS.md) D21) play uses a follow camera on the vehicle pawn (spring arm + camera, cosmetic, local only). The arena's fixed camera remains as the menu backdrop and a future spectator view.

---

## 12. Blender pipeline

Blender is the tool for custom 3D assets. Gameplay milestones may use Engine primitives or template meshes until the visual milestone.

Pipeline:

1. Author in Blender.
2. Export (FBX or glTF; pick one and keep it).
3. Import into Unreal.
4. Assign materials.
5. Set collision (or, for vehicles, confirm the gameplay primitive is separate from the render mesh).
6. Hook the asset into the pawn, trail renderer, or pickup.
7. Verify in a match: silhouette, heading readability, collision unchanged by lean, frame time.

Source files live outside Unreal content, for example `ArtSource/Blender/`. Do not put `.blend` files under `MyProject/Content`.

Add a Blender Python script only when a repeated export or naming step is wasting time. Do not write a general asset framework up front.

Vehicle collision stays the gameplay primitive even if the imported mesh has its own collision. Disable or ignore mesh collision so a leaning mesh cannot widen the hit volume.

---

## 13. Performance

Target **60 FPS or higher** on a modern gaming PC with:

- Four vehicles
- Long trails
- A few powerups
- Particles and bloom
- Network updates

Rules that protect that target:

- No rigidbody per trail segment
- Replicate compact segments and events, not meshes
- Do not require Lumen hardware ray tracing or the template's ray-tracing settings for readability
- Bloom and a dark grid are enough for the neon read
- Particles stay off the gameplay collision path

The current template enables Lumen, virtual shadows, and ray tracing. Neon Grid should stay playable if those are lowered. Do not tune gameplay around them.

---

## 14. Known UE difficulties

These are real, and the smallest approach that keeps the player experience is listed with each.

| Difficulty | Smallest approach that preserves the design |
| --- | --- |
| Trail collision is not a built-in UE feature | Custom sweep plus spatial hash. Not Chaos bodies per segment. |
| `UCharacterMovementComponent` prediction does not model heading-lock arcade motion | Custom integrator. Predict locally by running the same step on the owner's inputs. Correct toward server state. Keep correction simple; LAN RTT is low. |
| Trail bandwidth | Replicate new segments and rare edits (cutter, death stop). Never send the ribbon mesh. |
| LAN browse can fail on a given network | UE session search for the common case, plus a direct IP and port connect path. Both are required. |
| No C++ module in the project yet | Add a game module and compile from the Editor or Unreal Build Tool before gameplay code. Manual Editor step. |
| Phase, Shield, and Trail Cutter change hits | Only the authority resolves them. Predicted clients may show the local effect, then accept the server result. |
| Fixed 60 Hz versus variable frame time | Accumulate render delta and run zero or more sim steps. Cap steps per frame so a hitch cannot simulate unbounded time. |
| Split-screen default | Set `bUseSplitscreen` false and use one local player controller. |
| EMP's three possible effects | Implement steering-rate reduction only. See [DECISIONS.md](DECISIONS.md). |

---

## 15. Implementation rules

From the guide, applied to this repo:

1. Do not rewrite a system that already works.
2. Inspect the project and the relevant spec before a major feature.
3. One feature at a time. Smallest coherent version.
4. Build and test after major changes. Do not claim success from reading code.
5. No unused dependencies.
6. No networking logic inside VFX or UI beyond display of replicated state.
7. No second movement implementation for AI.
8. No physics object per trail segment.
9. No future milestone work inside the current one.
10. The guide plus these docs stay the source of truth. A design change is written down in [DECISIONS.md](DECISIONS.md) first.

When a task needs the Unreal Editor (module generation, default map, input assets, widgets, import), say so explicitly. Do not pretend those steps happened because C++ compiled.
