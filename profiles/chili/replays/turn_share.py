"""turn_share.py <trace.csv> [first sample]

In how many controller samples that carried a mouse angle did the player's heading change by exactly that
angle? Reads the CHILI_INPUT_TRACE of a dual_analog run (replays/README.md, "mouse replays") and prints the
count by move, then the totals with and without the samples the port leaves to the game (a stunt on a wall,
on the ground after a blow, dead). CONTROLS.md, "Where the turn is applied", says what each move is.
"""
import csv
import sys

MOVES = {27: 'dive left', 28: 'dive right', 29: 'dive back', 30: 'dive forward', 52: 'on the ground after a flight', 8: 'jump', 9: 'jump',
         11: 'landing a jump', 21: 'shooting', 36: 'wall flip', 57: 'wall run', 34: 'state 34', 19: 'state 19'}


def share(path, first=0, tolerance=0.05):
    rows = {}
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            try: rows[int(row['sample'])] = row
            except ValueError: pass
    counts = {}
    thrown = flying_before = False
    model_before = '0'
    for sample in sorted(rows):
        row, before = rows[sample], rows.get(sample - 1)
        if sample < first or before is None: continue
        try: change = float(row['player_heading']) - float(before['player_heading'])
        except ValueError: continue
        change = (change + 180.0) % 360.0 - 180.0
        state, model = int(row['state'] or 0), row.get('physics_model', '')
        # Whose flight: a dive starts on the ground or off a wall, a blow throws the player into the air at once.
        flying = 27 <= state <= 30
        if flying and not flying_before: thrown = model == '1' and model_before != '3'
        elif not flying and state != 52: thrown = False
        flying_before, model_before = flying, model
        angle = float(row['mouse_turn'] or 0)
        if angle == 0.0: continue
        left = 'wall stunt' if model == '3' else 'knocked down' if state == 52 and thrown else 'dead' if state in (34, 19) else ''
        move = MOVES.get(state, 'on foot')
        if flying: move += ', thrown' if thrown else ', take-off' if model == '0' else ', in the air'
        count = counts.setdefault((left, move), [0, 0])
        count[0] += 1
        count[1] += abs(change - angle) < tolerance
    return counts


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__)
    counts = share(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 0)
    total, play = [0, 0], [0, 0]
    for (left, move), (samples, exact) in sorted(counts.items()):
        print('%-13s %-36s %5d of %5d' % (left or 'play', move, exact, samples))
        total[0] += samples; total[1] += exact
        if not left: play[0] += samples; play[1] += exact
    print('all samples with a mouse angle: %d of %d exact (%.1f%%); without wall stunts, knocked down, dead: %d of %d (%.1f%%)' % (
        total[1], total[0], 100.0 * total[1] / max(total[0], 1), play[1], play[0], 100.0 * play[1] / max(play[0], 1)))
