# Neon Grid — AI Specification

AI opponents are players. They use the same pawn, movement component, trail rules, collision, and powerups as humans. The AI controller only produces the input a human would produce.

See [VEHICLE_SPEC.md](VEHICLE_SPEC.md) for the movement step and [GAME_DESIGN.md](GAME_DESIGN.md) for match rules. Do not build a second movement system.

This is deterministic gameplay AI. Machine learning is not required and is not part of v2.

---

## 1. Architecture

```text
Human: input device -> player controller -> input snapshot -> movement
AI:    AI controller -> input snapshot -> movement
```

The input snapshot is the same struct used online: steering in -1..1, and activate-powerup. There is no AI-only speed, radius, or collision exception.

`ANeonAIController` owns sensing and decisions. `UNeonMovementComponent` owns integration. The trail registry is read-only from the AI's point of view. The server (or the offline authority) runs AI. Remote clients do not simulate opponent decisions; they render replicated pawns.

The structure should allow a later controller to replace the decision function without replacing movement. Personalities, if added after v2, are different decision parameters or state weights, not new physics.

---

## 2. What the AI can know

Each decision reads:

- Own heading, position, and speed
- Arena bounds
- Nearby trail segments (spatial hash from the collision system, not a full scan if the hash exists)
- Other players' positions and headings
- Active powerups and what this AI is holding
- A simple measure of open space in a few sample directions
- Immediate collision risk along the current heading and along left and right arcs

The AI does not get hidden information that a human cannot see: future spawns, other players' held powerups, or a god-view path that ignores trails. Expert play comes from better use of visible space, not from extra data.

Difficulty may delay how often this snapshot is acted on. It may not change the vehicle data asset.

---

## 3. States

Start with these states. A later version may add states. It should not replace the movement component.

| State | Intent |
| --- | --- |
| Cruise | Hold a heading while the forward space is clear. |
| Turn | Commit to a left or right arc to take space or set up a cut. |
| Avoid | Immediate collision risk. Steer toward the sampled direction that stays clear longest. |
| Chase | Close distance or mirror an opponent to force them into a trail. This is positioning, not a ram script. |
| Collect Powerup | Detour to a pickup when the path is safe enough for this difficulty. |
| Escape | Already boxed in or about to be. Prefer a legal arc, Phase, Shield, or Trail Cutter over a reversal the vehicle cannot perform. |
| Attack | Use the held powerup or a cut-off line when it creates an opening. Never an instant-kill action. |

Suggested priority when several match:

1. Avoid, if impact is inside the reaction window
2. Escape, if the only clear arcs are closing
3. Attack or Collect, if the difficulty allows and the risk is acceptable
4. Chase or Turn, if an opponent or a gap is the best space play
5. Cruise

Avoid can interrupt any state. That is the survival rule.

---

## 4. Driving outputs

The controller writes steering each decision, then holds or blends it until the next decision so the vehicle does not flicker.

- **Steering.** Choose a target heading that is reachable at the current turn rate. Sample a few arcs (gentle, medium, sharp) on each side plus straight. Reject arcs that intersect a trail, a vehicle, or the boundary within the look-ahead. Pick the survivor that best matches the state.
- **Throttle.** There is no throttle axis in v2. Speed is the cruise speed unless a powerup changes it. The AI does not invent a brake or a throttle. "Going faster" means activating Boost or Overdrive.
- **Braking.** Not a mechanic. Do not add brake input.
- **Overtaking.** There are no laps to pass. The equivalent is cutting off space: Turn or Chase so the opponent meets a trail. Do not steer into another vehicle on purpose as a default tactic. Vehicle-vehicle contact kills both parties under the current rules, so ramming is usually self-destructive. Attack means trail placement and powerup timing.
- **Recovery.** If the AI hits nothing but is trapped, Escape tries a legal curve. If it holds Trail Cutter, it may open a gap. If it holds Phase, it may cross a trail. If it holds Shield, it may spend the charge only when a hit is otherwise certain. An instant 180 is still impossible. The AI lives under the same radius limit as the player.
- **Collision avoidance.** Avoid is the short-horizon version of the same sweep the game uses for death. Use the collision system. Do not raycast against visual trail meshes.
- **Powerup use.** Activate only in state Attack or Escape, and only if the held type matches the situation (Phase or Cutter to cross or open, Shield when impact is imminent, Boost or Overdrive only when the wider radius still fits the chosen arc, EMP when a target is in range and the AI wants them less able to turn).
- **Difficulty scaling.** See section 5.
- **Personality.** Not a v2 behavior matrix. Section 7 is the extension point.

---

## 5. Difficulty

Four levels. They change reaction time, look-ahead, how often a bad arc is chosen, and how wisely powerups are used. They do not change speed, hit radius, or trail collision.

| Level | Behavior |
| --- | --- |
| Easy | Long delay between decisions. Short look-ahead. Often misses a closing trail. Rarely collects or uses powerups well. |
| Normal | Moderate delay. Can follow open space and sometimes take a pickup. |
| Hard | Shorter delay. Better awareness of nearby trails and opponents. Uses powerups in obvious traps. |
| Expert | Fast decisions. Plans cuts and escapes. Uses powerups with timing. Still the same physics. |

Solo menu choices are exactly these four. LAN v2 does not spawn AI to fill slots (see [MULTIPLAYER_SPEC.md](MULTIPLAYER_SPEC.md)).

Suggested config fields, per difficulty asset:

- Decision interval (seconds)
- Look-ahead distance
- Number of sample arcs
- Chance to pick a worse safe arc (mistakes)
- Minimum risk tolerated before Collect
- Powerup activation skill (0 to 1)

Easy uses a large interval and a high mistake chance. Expert uses a short interval and a low mistake chance. Exact numbers are tuned in play, not hardcoded in the decision function.

---

## 6. Fairness and failure

- AI dies to bounds, trails, own trail, and vehicles, with the same Shield and Phase exceptions.
- AI may drive poorly. It may not tunnel through trails unless Phase is active on the server.
- If the spatial query is too expensive, sample fewer arcs on Easy and Normal before simplifying the hash. Do not give AI a cheaper collision rule.
- When the last human or last AI dies, the round system resolves the winner. AI does not award score itself.

---

## 7. Extension point

After the Survival match is fun, personalities can be extra difficulty-style assets: weights for Chase versus Cruise, willingness to pick up Overdrive, or how early they cut. They still emit the same input snapshot.

Do not add a racing line, a waypoint lap, or a learned policy as a requirement for the first AI milestone. A few authored steer biases are enough if a personality asset appears later.

---

## 8. First playable AI

The smallest version that still matches this spec:

1. Shared movement and input snapshot.
2. Avoid plus Cruise, using trail and boundary sweeps.
3. Easy and Normal intervals only if time is short, but the difficulty asset must already have four entries so Hard and Expert are data changes, not a rewrite.
4. Then Collect, Escape, and powerup activation once powerups exist.
5. Chase, Turn, and Attack once a full offline round exists.

A match against 1, 2, and 3 AI at each shipped difficulty is the test. "The code has states" is not a passed test.

---

## 9. Implementation notes (Milestone 3 as built)

`ANeonAIController` (`Source/NeonGrid/Public/AI`) with `UNeonAIConfig` parameters. Full rationale in [DECISIONS.md](DECISIONS.md) D22.

- **Plan** = (steering, target heading change). Executed faithfully between decisions; a new decision every `DecisionInterval` (±20 % jitter) or immediately when the committed plan has become lethal and `ReactionTime` has passed (Avoid).
- **Candidates**: continuation of the committed plan (the only one that receives `CommitBias`), straight, ±(1/ArcsPerSide … 1.0) steering × the first `TurnAnglesPerArc` of {90, 45, 135, 30, 180}°. If none survives, full-steer 180° and 135° both ways are added.
- **Prediction**: `LookAheadTime` in `PredictionSteps`, the vehicle's own turn-rate clamp, lethal step = boundary (inset by radius + `SafetyMargin`), any trail (same own-trail grace as the sim), the predicted path's own earlier points, an alive vehicle now, or its constant-curvature extrapolation for ≤ `OpponentExtrapolationTime` treated as trail.
- **Score** = 2·survives + 2·freeFraction + `OpennessWeight`·(½ openness + ½ escape) + `SpaceWeight`·territory + `ChaseWeight`·cutOff − `StraightBias`·|steer| − loopAversion + commit. Openness = 5 weighted rays from the plan end; escape = can a full-steer 90° turn either way still be made; territory = flood-fill reachable fraction of a `SpaceCellSize` grid (trails + vehicles, dilated one cell, plan path marked).
- **States** are labels on the chosen plan (Cruise, Turn, Avoid, Chase, Doomed) for logs and a later HUD; the decision is the scoring.
- **Not yet**: Collect and powerup activation (Milestone 4), explicit Attack beyond the cut-off term, personalities.
