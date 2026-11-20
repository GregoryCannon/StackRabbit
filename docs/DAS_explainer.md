# DAS Explained

The DAS playstyle involves holding down the Dpad and interacting with the game's auto-shifter. 

## DAS Charge
The auto-shifter keeps a counter that ranges from 0 to 16, and it ticks up each frame that the button is held, until it reaches >= 15 at which point it performs a shift. After a successful shift, it lowers the counter to 10, and after a manual tap, it lowers it to 0. Human players have named this counter value the "DAS charge" since there are many techniques that preserve a higher value for this charge between pieces. Maintaining a high DAS charge allows for better piece movement.

Some additional properties of the counter:
- Losing DAS: any down-press on the left or right dpad arrows while a piece is spawned into the screen sets the DAS charge instantly to 0.
- DAS charge only changes on frames where a dpad arrow is pressed down; rotation buttons do not affect the DAS charge in any way.
- DAS is frozen during ARE: during piece-spawn-delay (named ARE), dpad inputs don't modify the DAS charge like they usually would. The value cannot change during ARE.
- 15 vs 16: there is no functional difference between a DAS charge of 15 and 16. The only distinction is that 16 comes from wall charges whereas 15 comes from manually charging.

## DAS Techniques

Here are some gameplay maneuvers players use to get the most out of DAS:

- **Wall charge**: if a player attempts to shift a piece in any direction where it would collide with the stack or with the edges of the playing field, the DAS charge is instantly set to 16. This is highly desirable, since 16 is the maximum possible DAS charge and sets up the player well for the next placement.
- **Quicktap**: if a player wants to move a piece higher than standard DAS would reach, they can expedite the last shift by letting go of the arrow right after the 2nd to last shift, and manually tapping the last shift quicker than the auto-shifter would have. This has a tradeoff of leaving the player with a poor DAS charge for the next piece, unless they can use other mechanics to increase the DAS charge after performing the quicktap.
- **Holding through ARE**: since the DAS charge is frozen during ARE (see above), players take advantage of the piece-spawn delay to start holding the arrow in the desired direction for the next piece, without losing their DAS charge.
- **Slow tap**: this isn't technically a technique, but more so a constraint. When playing DAS, the player shouldn't be allowed to manually tap the piece faster than a preset "slow tap" speed limit. This might be set to, for example, 8 Hz.

## The Engine's Frame-Exact Charge Model

The engine simulates DAS frame-by-frame (see `src/backend/full_move_search.cpp`,
verified by the move-search fuzz harness). The table below restates
the charge rules above at frame granularity, plus transitions that only the engine
model tracks:

| Event | Charge after | Notes |
|---|---|---|
| Piece spawn | incoming charge | preserved from the previous piece via its `dasChargeAfter` |
| Shift while "tapping" (spawn charge <= 5) | **0** | the down-press that starts the shift resets charge |
| Normal DAS shift (charge >= 15) | **10** | standard post-shift charge |
| Held frame, no shift | **+1** | only while the arrow is held down |
| No inputs this frame | unchanged | the charge only changes on frames where a dpad arrow is down |
| Wall charge (shift blocked by stack/edge) | **16** | via a deliberate re-press |
| Tuck tap (manual, non-wall) | **0** | fresh down-press |
| Wall-charge tuck | **10**, then +1 per held fall frame | |
| During ARE / line clears | unchanged | charge is frozen; no input modifies it |
| Next piece | `dasChargeAfter` of the chosen placement | |

Consequences worth internalizing:

- **Shift cadence**: The effective cadence of shifting with DAS is a shift every 6
  frames, or about 10 Hz. Starting with a DAS charge of <15 reduces the effective
  range. However, using quicktaps (described above) can expand the effective range.
- **Holding past the goal**: the agent intentionally keeps holding the arrows after
  reaching its goal until it either reaches a full charge (15) or the piece locks into
  the stack. Once the piece is at the goal and the DAS charge is full, it lets the
  piece drop with no further changes to DAS — from that point the only thing that can
  change the charge is the deliberate wall-charge re-press.

### Input sequence alphabet (one character per simulated frame)

| Char(s) | Meaning |
|---|---|
| `.` | no input |
| `A` / `B` | rotate CW / CCW (tap) |
| `L` / `R` | shift left / right |
| `E` `F` / `I` `G` | shift+rotate combos (L+A, L+B / R+A, R+B) |
| `l` `e` `f` / `r` `i` `g` | lowercase: DAS button held, no shift executed this frame (visual only) |
| `_` | sequence prefix: means a dpad arrow should be held during ARE — whichever arrow the player will hold on the first frame |
| `l`/`r` from wall-charge attempts | the shift would collide; sets charge to 16 |
| `*`, `^` | ARE/entry-delay frames / line-clear frames; appended at assembly, never mid-simulation |