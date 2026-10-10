# Neon Grid — TODO

Status of the v2 game. Specs live in [GAME_DESIGN.md](GAME_DESIGN.md) and the companion docs. The original text is [Reference/ORIGINAL_GAME_GUIDE.md](Reference/ORIGINAL_GAME_GUIDE.md).

Milestones 1–4, the offline round/match system (most of Milestone 5), the stepped-pyramid terrain (D25) and a Win64 package are compiled and smoke-tested headlessly. **Nothing has been played by a person yet.**

---

## DONE

- Original guide archived at `Docs/Reference/ORIGINAL_GAME_GUIDE.md`
- Design and implementation specs written (`GAME_DESIGN`, `TECHNICAL_SPEC`, `MULTIPLAYER_SPEC`, `AI_SPEC`, `VEHICLE_SPEC`, `DEVELOPMENT_PLAN`, `DECISIONS`)

### Milestone 1 — Movement (compiled, smoke-tested)

- [x] C++ module `NeonGrid` under `MyProject/Source`
- [x] `UNeonVehicleConfig` Primary Data Asset with the VEHICLE_SPEC section 7 fields as class defaults
- [x] `UNeonMovementComponent`: 60 Hz fixed step, heading/speed integration, min-radius clamp, boundary elimination, render interpolation, `SetFrozen` for countdown/results
- [x] `UNeonLeanComponent`: cosmetic roll on the visual root only
- [x] `ANeonVehiclePawn`: scene root + lean root + two engine-cube placeholders, color tint, follow camera (D21)
- [x] `ANeonArena`: The Grid bounds (14000 × 9000, D23), 4 inward-facing spawns, fixed whole-arena camera (menu backdrop), stepped-pyramid procedural terrain with plateau lips (D25), walls, dev light
- [x] `ANeonPlayerController`: Enhanced Input built in code (A/D, arrows, left stick; Esc/gamepad Menu; Space / RT / gamepad A activates a powerup), per-frame input snapshot
- [x] Config: split-screen off, default map `/Game/NeonGrid/Maps/L_TheGrid`, default mode `NeonGameMode`, game instance `NeonGameInstance`
- [x] Compile: `NeonGridEditor` builds clean with VS 2026 Community (MSVC 14.51). UBT warns that 14.51 is newer than its preferred 14.50.
- [x] Level `L_TheGrid` (empty; the game mode spawns the arena)

### Milestone 2 — Trails (compiled, smoke-tested)

- [x] `FNeonTrailSegment` / `FNeonTrailHit`, `UNeonCollisionSystem` world subsystem (segment registry, 400 cm spatial hash, thickened sweeps, own-trail grace by tick, vehicle registry)
- [x] `UNeonTrailComponent` (records the rear point each tick, extends/starts segments), `UNeonTrailRendererComponent` (ISM mirror, cosmetic only)
- [x] Movement: trail + vehicle sweeps after the boundary check; causes `Trail`, `OwnTrail`, `Vehicle`; vehicle-vehicle kills both
- [x] Headless smoke test: deterministic deaths at the predicted ticks; vehicle-vehicle path exercised in AI matches (both die on the same tick)

### Milestone 3 — Solo AI (compiled, measured headlessly; D22)

- [x] `UNeonAIConfig` (Easy / Normal / Hard / Expert parameter sets as class defaults)
- [x] `ANeonAIController`: plan sampler with path prediction on the real collision data, openness/escape probes, flood-fill territory grid, chase cut-off scoring, loop aversion, commit hysteresis, curvature-aware opponent extrapolation, panic 180/135 turns, deterministic seeded RNG, mistake chance, Avoid interrupt at reaction time
- [x] AI has an `ANeonPlayerState` like a human; freezes with the round
- [x] Measured (Normal, sim ticks): lone AI median ≈ 36–41 s (own trail); 3 AI in a match median ≈ 13 s. Head-on spawn pair no longer dies at 2.7 s every round
- [ ] Hard / Expert measured and tuned (parameters exist; only Normal has numbers)
- [ ] Easy confirmed beatable by a new player (needs a person)
- [x] Powerup use: `CollectWeight` / `CollectRange` steer toward pickups when the slot is empty; `PowerupSkill` gates activation moments per type (see D26)
- [ ] Guide states Collect / Attack / Escape as explicit modes — Escape is a probe, Chase and Collect are score terms; the state label does not show Collect

### Milestone 4 — Powerups (compiled, smoke-tested headlessly; D26)

- [x] `ENeonPowerupType` (Boost, Shield, Phase, TrailCutter, EMP, Overdrive), `UNeonPowerupConfig` data asset with every tunable as class defaults
- [x] `UNeonPowerupComponent` on the vehicle: one slot, tick-counted effects, `PreStep` activation from the latched input edge, Shield absorb + pass-through, Phase, EMP victim state, speed/turn multipliers read by the movement step
- [x] `UNeonCollisionSystem`: `RemoveSegment`, `CutTrailsInCircle` (removed flag + re-added outside pieces), `FindFirstTrailAlongRay`; the trail renderer hides removed chunks, the AI grid skips them
- [x] `ANeonPowerupPickup` (tinted sphere + beacon pillar at floor height) and `UNeonPowerupSpawner` on the game mode: seeded lattice spawns with trail / vehicle / pickup clearance, lifetime expiry, 2D collection, active only in Playing, cleared on round reset
- [x] HUD slot bottom center: held name in its color + PRESS SPACE, ACTIVE / ARMED with a time bar, STEERING JAMMED while EMP'd
- [x] Vehicle feedback: shield disc, Phase blink, nose glow on Boost / Overdrive, body flicker while EMP'd
- [x] Solo menu POWERUPS ON / OFF; `-NeonNoPowerups` for headless runs
- [x] Headless: pickups spawn, an Expert AI detoured to collect a Trail Cutter and fired it; no crashes over a 3-round match
- [ ] Balance pass with a person: spawn cadence vs. round length (rounds between four AI last 10–25 s; first pickup at 3 s, then every 5 s), effect durations, EMP range
- [ ] Pickup / activation / expiry audio and VFX (Milestones 8–9)

### Milestone 5 — Round / match system (offline; compiled, flow-tested headlessly)

- [x] `ENeonMatchPhase`: MainMenu → Countdown (3 s, frozen) → Playing → RoundResults (3 s) → Countdown … → MatchResults
- [x] `ANeonGameState` (phase, round, score limit, phase timer, round/match winner; replicated fields), `ANeonPlayerState` (slot, color, round wins, alive, AI flag; replicated)
- [x] `ANeonGameMode`: `StartSoloMatch`, `Rematch`, `ReturnToMainMenu`; last vehicle alive wins the round; simultaneous wipe awards nothing; first to `ScoreLimit` (default 5)
- [x] `UNeonGameInstance::MatchSettings` (AI count, difficulty, score limit, powerups on/off, arena id)
- [x] Headless: 10-round match to 5 ran end to end with correct scores and "Match over"; menu boot spawns nothing until START
- [ ] Countdown / round-result audio and visual punch (Milestones 8–9)

### Presentation (first pass, code-built UMG; D24)

- [x] Main menu: SOLO / LAN PLAY / OPTIONS / CREDITS / QUIT (LAN, options, credits disabled)
- [x] Solo menu: AI players 1–3, difficulty, powerups on/off, arena (The Grid), START, BACK; settings persist in the game instance
- [x] HUD: per-slot scores in the corners in player color (dimmed when dead, "(YOU)" marker), ROUND N / FIRST TO N, center countdown 3-2-1-GO, "P# WINS THE ROUND", "NO SURVIVORS", "ELIMINATED", match result with REMATCH / MAIN MENU
- [x] Esc / gamepad Menu returns to the main menu from any phase

### Terrain (D25)

- [x] `ANeonArena::GetFloorHeight / GetFloorNormal` (3 tiers × 140 cm, 1200 cm apart, 450 cm smoothstep ramps), `UProceduralMeshComponent` heightfield + lips, no collision
- [x] Vehicles ride the height function (Z, cosmetic pitch / roll); trails are drawn as pitched chunks; spawns on the first plateau
- [x] Headless: identical elimination ticks before and after (the simulation never reads height)

### Packaging (shareable build)

- [x] `DefaultGame.ini` packaging settings: Development, pak + IoStore, cook `L_TheGrid` only, always cook `/Engine/BasicShapes`
- [x] `RunUAT BuildCookRun` → `Packaged\Windows\NeonGrid.exe` (BUILD SUCCESSFUL). Procedure in HEADLESS TEST HOOKS below.
- [ ] Launch the packaged exe on a machine without the engine installed (needs a second PC)
- [ ] Shipping configuration build (removes the headless hooks and the debug symbols; needs a quick play test first)

## IN PROGRESS

Nothing mid-edit. Everything above compiles and the headless tests pass.

## NEXT

- **Play it.** Open the project, press Play: menu → SOLO → START. Check camera feel (distance / height / pitch / lag on the pawn), vehicle speed and turn, lean direction, terrain readability from the low camera, trail readability, HUD legibility, pickup visibility, AI difficulty. Nothing has been seen on screen yet.
- Powerup balance pass from that play test (spawn cadence, durations, EMP range, pickup size).
- Tune Hard / Expert from a human baseline.
- Milestone 6 (LAN) per DEVELOPMENT_PLAN. The powerup component, spawner and pickup already have replication flags on the state that clients need to see; the activation RPC and pickup replication are not written.

## BACKLOG

Ordered by [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md).

### Trails

- [ ] Trail visual quality (emissive material, glow, optional fade-in on new segments) — Milestone 8
- [ ] Trail replication (segment events to clients, including removals from Trail Cutter) — Milestone 6
- [x] Trail Cutter / Phase hooks in `CheckHazards`

### Powerups

- [x] Powerup spawning
- [x] Pickup
- [x] One-slot inventory
- [x] Manual activation
- [x] Boost
- [x] Shield
- [x] Phase
- [x] Trail Cutter
- [x] EMP (steering-rate reduction)
- [x] Overdrive
- [x] Powerup UI slot on the HUD
- [ ] Pickup models / materials (engine sphere + cube now)
- [ ] Trail Cutter hole feedback (the chunks simply vanish)

### Match

- [x] Countdown
- [x] Rounds
- [x] Score
- [x] First-to-5
- [x] Match victory
- [x] Rematch
- [ ] Results screen with full standings (the center message shows the winner only)

### LAN

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
- [ ] Score synchronization (fields are already replicated; untested)
- [ ] Round synchronization (phase/timer fields are already replicated; untested)
- [ ] Disconnect handling
- [ ] Client view target on possession (`OnPossess` is server-only; clients need `OnRep_Pawn` / `ClientRestart` handling)

### Presentation

- [x] Main menu
- [x] Solo menu
- [ ] LAN menu
- [ ] Lobby
- [x] HUD (first pass)
- [x] Powerup UI (first pass)
- [ ] Results (standings)
- [ ] Gamepad / keyboard navigation of menus (mouse only now)
- [ ] Neon visuals
- [ ] Basic audio
- [ ] Particles

### Movement checklist (milestone 1, needs a person)

- [ ] Continuous movement
- [ ] Continuous steering
- [ ] Speed system
- [ ] Turn radius
- [ ] Vehicle rotation
- [ ] Vehicle leaning (verify lean direction reads as into the turn; flip `LeanDirection` if not)
- [ ] Boundary collision
- [ ] Follow camera framing (D21 tunables on the pawn)

### After v2

- Movement polish and powerup balancing
- Arenas: The Cross, The Ring, The Maze, The Void, The Split
- AI personalities
- Territory, Team Battle, Time Attack, Elimination, Tournament
- Vehicle customization and cosmetics
- Advanced arena mechanics
- Dynamic music intensity if not finished in the audio milestone
- Spectator / whole-arena view for eliminated players (the arena camera exists)
- Two powerup slots, only if a later decision allows it
- Host migration
- Online transport using the same authority interface
- Trail segment fields for energy state, powerup effect, and expiration
- Spawn scoring by distance, nearby trails, and accessibility

## HEADLESS TEST HOOKS (non-Shipping only)

`UnrealEditor-Cmd.exe <uproject> -game -nullrhi -nosound -unattended -nopause -nosplash -NoShaderCompile` plus:

- `-NeonAutoStart` — skip the menu and start a match with the game instance settings
- `-NeonAICount=N`, `-NeonAIDifficulty=0..3`, `-NeonScoreLimit=N`, `-NeonNoPowerups` — override settings for that run
- `-NeonRoundEndsAtZero` — the round ends only when nobody is alive (lone-AI survival runs)
- `-LogCmds="LogNeonGrid VeryVerbose"` — every AI decision with scores, position, heading, tick

Output goes to `MyProject/Saved/Logs/MyProject.log`. Do not pass `-log=Name.log` from PowerShell: UE parses the `.log` token as a map URL. The process does not exit after "Match over"; stop it with `Stop-Process -Name UnrealEditor-Cmd`.

Measure in sim ticks (60 Hz) from the "out (...) after N ticks" lines, not wall clock: under `-nullrhi` the sim runs faster than real time.

## BUILD AND PACKAGE

Compile the editor module (about 30 s incremental):

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" NeonGridEditor Win64 Development -Project="<path>\MyProject\MyProject.uproject" -WaitMutex
```

Package a shareable Windows build (about 10 minutes the first time; log in `Packaged\uat.log`):

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun -project="<path>\MyProject\MyProject.uproject" -noP4 -platform=Win64 -clientconfig=Development -build -cook -map=/Game/NeonGrid/Maps/L_TheGrid -stage -pak -iostore -archive -archivedirectory="<path>\Packaged" -nocompileeditor -nodebuginfo -unattended -utf8output
```

Share the `Packaged\Windows` folder (zip it, about 650 MB). No engine install is needed. The build packs only `L_TheGrid` and the engine basic shapes; the template content under `Content/` is not cooked.

Two executables are in the folder: the 170 KB bootstrapper `NeonGrid.exe` at the root and the real game `MyProject\Binaries\Win64\NeonGrid.exe`. On the dev machine, Windows Application Control (Smart App Control) blocked the unsigned bootstrapper but ran the real exe; tell players to use the inner one if the root one is blocked, and expect a SmartScreen warning either way. Verified: the packaged exe ran a full auto-started match headlessly (`-nullrhi -NeonAutoStart`).

## KNOWN ISSUES

- `git` is not installed on the dev machine. Nothing has been committed.
- MSVC 14.51 is newer than UE 5.8's preferred 14.50 range. UBT accepts it with a warning.
- The game mode logs `FindPlayerStart: NO PLAYERSTART` on spawn. Harmless: spawn transforms come from the arena.
- An eliminated human keeps viewing from their parked vehicle until the round ends (consequence of D21). No spectator camera yet.
- Menus are mouse-only. Keyboard/gamepad navigation is backlog.
- HUD and menus are code-built with engine fonts and default button brushes. Styling waits for the visual milestone; a Blueprint subclass can replace the tree.
- The AI's territory grid is topological (dilated one cell). It still occasionally seals itself in; lone-AI deaths are almost all `OwnTrail`.
- The trail renderer rebuilds its ISM render state every tick while a segment is being extended. Fine at four vehicles.
- The trail renderer is a cube ISM with a flat tint. No emissive/glow yet.
- The AI's path prediction uses the base turn rate; a Boosted / Overdriven / EMP'd AI plans with the wrong turning circle for the effect's duration.
- The powerup spawner picks points from a 1500 cm lattice; on a busy arena late in a round it can find no clear point and skips that spawn (logged at Verbose).
- Pickups are plain actors found with `TActorIterator` by the AI each decision. Fine at three pickups; replace with the spawner's list if more are ever allowed.
- The terrain mesh is rebuilt in `OnConstruction`; changing tier settings on a placed arena regenerates it in the editor but there is no LOD.
- `ANeonArena` carries a directional light so an empty level is visible. Turn `bEnableDevLight` off when the level gets real lighting.
- `SimulationHz` lives on the movement component. It should move to a shared game config asset when one exists.
- Template renderer settings enable Lumen and ray tracing. Gameplay must not depend on them for trail readability.
- LAN cannot be called done until host + 1, host + 2, and host + 3 are tested on a real network path. That test has not been run.
- Guide `baseSpeed = 12` is not a literal Unreal cm/s value. See [DECISIONS.md](DECISIONS.md) D4.
