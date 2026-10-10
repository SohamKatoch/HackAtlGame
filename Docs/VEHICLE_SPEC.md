# Neon Grid — Vehicle Specification

Neon Grid vehicles are arcade bodies with a locked heading, not wheeled simulations. Handling must feel responsive before any other system is judged finished.

Player-facing rules are summarized in [GAME_DESIGN.md](GAME_DESIGN.md). The integrator and collision primitive are specified in [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md). AI uses this same model ([AI_SPEC.md](AI_SPEC.md)).

---

## 1. Model

The vehicle is a kinematic pawn.

```text
heading (degrees, continuous)
speed (units per second)
velocity = direction(heading) * speed
```

There is no engine RPM, gear, suspension, tire slip, or brake pedal. Chaos wheeled vehicles are the wrong base. A custom movement component integrates heading and position.

Unreal units are centimeters. The guide's example `baseSpeed = 12` is a tuning token, not a literal 12 cm/s (that would be a crawl). The first drivable build picks a world scale where the arena can be crossed in a few seconds and then stores that speed in the vehicle Data Asset. The token 12 remains the config name's starting point only if the map is built in a matching abstract scale. Either way, designers edit the asset, not the integrator.

---

## 2. Steering

Input is a scalar from -1 (full left) to 1 (full right). Zero holds heading.

```text
turnRate = maxTurnRate * response(speed, powerupModifiers) 
heading += steeringInput * turnRate * dt
```

Starting `maxTurnRate` is **120 degrees per second** at cruise, before speed scaling. Playtests may change the asset.

The player can produce:

- A small correction (tap)
- A medium curve
- A sharp turn (held steer at lower speed)
- A wide arc (held steer, or any steer at high speed)
- A near-90-degree corner by holding steer long enough

90-degree grid turns are allowed as a result of holding steer. They are not a separate move, and the game does not snap heading to 90-degree increments.

### No instant reversal

A single input must not flip heading by 180 degrees. Enforce all three:

- A finite max turn rate
- A minimum speed while alive (MVP: always at cruise or powerup speed, never zero)
- A minimum turn radius derived from speed and turn rate

```text
radius = speed / turnRateRadians
```

Clamp turn rate so radius never drops below `minTurnRadius`. The clamp is what makes a U-turn cost space. If a trap is narrower than that diameter, the player dies. That is intentional.

### Speed and radius

| Condition | Radius |
| --- | --- |
| Cruise | Moderate. Starting point for tuning. |
| Higher speed | Larger radius (lower turn rate and/or the radius clamp). |
| Boost | Significantly larger radius than cruise. |
| Overdrive | Faster than Boost's intent and a stronger steering penalty. |

Tradeoff to protect: going faster is stronger and harder to steer.

MVP cruise speed is constant. There is no acceleration ramp and no brake in the movement milestone. A future curve may add minimum speed, acceleration, cruise, and boost. Powerups already need a temporary speed override, so the component should accept `speedMultiplier` and `turnRateMultiplier` even while the base speed is constant.

Overdrive's "increased acceleration" is, until that curve exists, the same kind of immediate speed multiplier plus a reduced turn-rate multiplier. Do not block Overdrive on a full acceleration model.

---

## 3. Input

One local player. Enhanced Input. Rebindable.

| Action | Default keyboard | Default gamepad | Network |
| --- | --- | --- | --- |
| Steer | A / D or Left / Right | Left stick X | float -1..1 |
| Activate powerup | Space | Right trigger | bool |

No throttle. No brake. No camera-look during a match. No second local player binding.

The guide's sample input packet included a `boost` field. Boost is not a held button. Activating the Boost powerup uses Activate Powerup. Do not add a third gameplay button for v2.

Steer and activate are sampled into the input snapshot on the simulation tick and, online, sent to the host. The host does not trust a client-supplied position.

---

## 4. Visual lean

Lean is cosmetic. It communicates an aggressive turn. It is not a gameplay stat.

- Rotate the visual mesh toward the steer direction.
- Small steer, about **5 degrees**.
- Medium steer, about **12 degrees**.
- Full steer, about **20 degrees**.
- Cap around **15–25 degrees**. Starting `maxLeanAngle` is **20**.
- Response is fast. Return to neutral is smooth when steer returns to zero.
- Do not lean so far that the vehicle looks like it is falling over.

Implementation: a child scene component or mesh. The collision primitive stays on the parent and does not roll. Animating lean must not change overlap tests.

Heading yaw of the pawn is the gameplay heading and should match the trail. Lean is an extra local roll or pitch on the mesh only.

---

## 5. Collision shape

- A small horizontal capsule or box, radius from the vehicle Data Asset.
- Independent of the render mesh and of lean.
- Hits that eliminate, once trails exist: arena boundary, any trail including own trail, another vehicle.
- Phase: ignore trails, still hit the boundary and still hit vehicles unless a later decision says otherwise. The guide only requires that Phase need not pass the boundary. Vehicle contact stays lethal in v2.
- Shield: the next lethal hit consumes one charge, the player stays alive, and feedback plays. The following hit kills.

The movement sweep uses this radius. The mesh can be larger visually if the glow reads better, but the tuned radius is what the player learns.

---

## 6. Presentation requirements

The mesh, even a placeholder, must read from a fixed high angle:

- Small
- Low profile
- Clear nose so heading is obvious
- Distinct color and a matching trail color

Placeholder: an Engine cone or wedge is enough for the movement milestone. Final mesh follows the Blender path in [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md). Imported collision is not the gameplay primitive.

---

## 7. Data Asset fields

`UNeonVehicleConfig` (name may change; fields should not):

| Field | Starting target |
| --- | --- |
| baseSpeed | Guide token 12, scaled to the arena so a crossing takes a few seconds |
| maxTurnRate | 120 degrees/second at the cruise reference |
| minTurnRadius | Tuned so a U-turn needs visible space |
| minSpeed | Equal to cruise in MVP |
| collisionRadius | Small relative to The Grid |
| maxLeanAngle | 20 degrees |
| leanAngles (low / mid / high) | 5 / 12 / 20 degrees |
| leanResponseSpeed | Fast |
| leanReturnSpeed | Smooth |
| boostSpeedMultiplier | About 22/12 if base speed is the guide's 12 |
| boostTurnRateMultiplier | Well below 1 |
| boostDuration | 1–2 seconds |
| overdriveSpeedMultiplier | At or above Boost |
| overdriveTurnRateMultiplier | Below Boost's turn multiplier |
| overdriveDuration | Short, data-driven |

Powerup durations can live on the powerup assets instead. The vehicle asset must still expose the multipliers the movement component reads so Boost and Overdrive do not hardcode math.

---

## 8. Feel tests (movement milestone)

Before trails, powerups, or network code:

- From a stop-at-countdown, the vehicle is moving on GO at cruise speed. It does not idle.
- Full steer draws a smooth arc, not a snapped 90.
- A flick of steer is a small correction.
- Holding steer long enough can reach a right angle.
- Releasing steer stops the heading change immediately and eases lean back.
- At Boost speeds (can be forced from a debug key in this milestone only), the arc is visibly wider.
- A attempted instant reverse still costs a wide loop.
- Leaning the mesh does not change whether the boundary test hits.
- Driving is fun enough to continue. If it is not, stop and retune this asset. Do not proceed to trails to "fix" the feel.

Debug boost input, if used, is removed or compiled out before the powerup milestone so the real Boost powerup is the only way to speed up.
