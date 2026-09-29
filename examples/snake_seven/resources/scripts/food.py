# food.py -- MODULE 3 of 7: FOOD, in Python.
#
# Owns where the food is, and the two things that happen when it is eaten:
# the score goes up by 10 and the snake gets one segment longer (spec 14).
#
# SPEC 12 IS THE WHOLE POINT OF THIS FILE: a new food position must never
# land on a cell the snake already occupies.  The previous version of this
# game picked a cell with a hash and did not check, which is wrong in a way
# that is almost invisible -- on a 30x20 board with a short snake it
# misbehaves perhaps one spawn in eighty, and it looks like the food "did
# not appear" rather than like a bug with a cause.  As the snake grows it
# gets steadily more common, so the game degrades exactly as the player
# gets better at it.
#
# The method is the one the spec recommends: enumerate every FREE cell and
# choose uniformly among them.  600 cells is nothing to walk once per food.
# It is also the only method that answers the endgame correctly -- when
# there are no free cells left the player has filled the board and won, and
# an enumerate-then-choose knows that, while a guess-and-retry spins.

COLS, ROWS = 30, 20          # spec 11
FOOD_SCORE = 10              # spec 14
BODY_POOL = 64               # must match tools/gen_scene.py

HX = (COLS - 1) / 2.0
HZ = (ROWS - 1) / 2.0


def _to_world(cx, cz):
    return (cx - HX, cz - HZ)


def _to_cell(x, z):
    return (int(x + HX + 0.5), int(z + HZ + 0.5))


def _find(name):
    # find_by_name's shape is "first_and_count": it returns a TUPLE of
    # (first-or-None, match count), not an entity.  Passing the tuple
    # straight to a binding fails deep inside ctypes with
    # "argument 2: 'tuple' object cannot be interpreted as an integer",
    # which names neither this call nor this file.
    first, _count = jce.find_by_name(name)
    return first


def on_start(self):
    self.state_e = _find("GameState")
    self.meta_e = _find("GameMeta")
    self.head_e = _find("SnakeHead")
    self.verdict_e = _find("Verdict")
    self.body_e = [_find("Body%02d" % i) for i in range(BODY_POOL)]
    self.run = -1
    self.seed = 12345
    jce.log("SNAKE python: food up; occupancy-checked spawn over %d cells"
            % (COLS * ROWS))


def _rand(self, n):
    # A small LCG rather than the random module: the seed is re-derived from
    # the run counter on every new game, so a replayed run eats the same
    # sequence of cells.  Determinism is what lets verify.py compare two
    # runs byte for byte.
    self.seed = (self.seed * 1103515245 + 12345) & 0x7FFFFFFF
    return self.seed % n if n > 0 else 0


def _occupied(self):
    """Every cell the snake is standing on, head included."""
    cells = set()
    hp = jce.get_position(self.head_e)
    if hp:
        cells.add(_to_cell(hp[0], hp[2]))
    for e in self.body_e:
        if not e:
            continue
        p = jce.get_position(e)
        # Parked segments sit far below the board; they are not on a cell.
        if p and p[1] > -50.0:
            cells.add(_to_cell(p[0], p[2]))
    return cells


def _respawn(self):
    taken = _occupied(self)
    free = [(cx, cz)
            for cz in range(ROWS)
            for cx in range(COLS)
            if (cx, cz) not in taken]
    if not free:
        # The board is full: the player has won.  Leaving the food where it
        # is would be a silent lie, so say so and let the manager end it.
        jce.log("SNAKE python: no free cell left -- the board is full")
        return False
    cx, cz = free[_rand(self, len(free))]
    wx, wz = _to_world(cx, cz)
    fp = jce.get_position(self.entity)
    jce.set_position(self.entity, float(wx), fp[1] if fp else 0.0, float(wz))
    return True


def on_update(self, dt):
    st, score, _tick = jce.get_position(self.state_e)
    best, length, run = jce.get_position(self.meta_e)

    if run != self.run:
        self.run = run
        self.seed = 12345 + int(run) * 7919
        _respawn(self)
        return

    if int(st) != 1:                       # only PLAYING can eat
        return

    hp = jce.get_position(self.head_e)
    fp = jce.get_position(self.entity)
    if not hp or not fp:
        return

    if _to_cell(hp[0], hp[2]) != _to_cell(fp[0], fp[2]):
        return

    # Eaten.  Score and length are this module's slots; read-modify-write so
    # the components this module does not own are put back unchanged.
    st2, sc2, tk2 = jce.get_position(self.state_e)
    jce.set_position(self.state_e, st2, sc2 + FOOD_SCORE, tk2)
    b2, l2, r2 = jce.get_position(self.meta_e)
    jce.set_position(self.meta_e, b2, l2 + 1, r2)

    # Tell the body module not to drop its tail this tick.
    w, s, _a = jce.get_position(self.verdict_e)
    jce.set_position(self.verdict_e, w, s, 1.0)

    _respawn(self)
