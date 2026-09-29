# es_fireflies.py — the firefly swarm, in Python.
#
# WHAT THE SCENE WAS MISSING.  gen_scene.py authors twelve ParticleEmitter
# entities named Firefly_0..Firefly_11 on a fixed ring, and the Lua director
# only ever calls jce.particle_set_emitting() on them (apply_state_discrete:
# night-only, all seasons).  Nothing moved them and nothing changed their
# colour, so the "swarm" was twelve motionless lamps at authored coordinates.
# This script owns exactly the two channels the director does not touch:
# TRANSFORM and PARTICLE COLOUR.  It never writes `emitting` — that stays the
# director's, so the two scripts cannot fight over one field.
#
# WHY PYTHON AND NOT LUA.  Nothing here is hot: twelve entities, two calls
# each.  It is the kind of low-rate ambience policy a gameplay programmer
# writes and re-writes, and it is the case the Python backend exists for.
#
# HOW IT IS SELECTED.  The Script component says
# "scripts/es_fireflies.py"; jce_script_vm_python_register() claims "py"
# (jce_script_vm.h, REGISTERING A LANGUAGE), so
# jce_script_vm_language_for_path() answers "python" and the runtime stands up
# a python VM beside the lua one.  JCE_SCRIPT_LANGUAGE is NOT set and must not
# be — it is a whole-process override, and this scene runs four languages.
#
# TWO EVIDENCE CHANNELS, AND THE SECOND IS THE ONE THAT COUNTS.  `jce.log`
# IS reachable from Python — jce_script/vm.py's _Jce.log wraps the VM's own
# JceScriptHost::log, which is one of the seven entries scripting/c_abi
# deliberately does NOT export (jce_script_api.h, "NOT in this ABI") and which
# the Python and Java backends put back through their own VM surface.  The C++
# backend does not, so it has no log at all; measured, not assumed.
#
# A log line proves on_start ran ONCE.  It cannot prove on_update is still
# running at frame 3000, and per-frame logging is not an option.  So the
# counters also go into EsPyProbe's TRANSFORM — x = on_start count,
# y = on_update count, z = how many fireflies were resolved — which
# src/es_script_probe.c reads back and prints, and which the editor's
# inspector shows live in Play.  A script that silently stopped and one that
# is still running look identical without it.

import math

# Ring drift: each firefly wanders a small ellipse around its AUTHORED spot,
# so the composition gen_scene.py placed is preserved and only softened.
DRIFT_R_XZ = 0.55      # world units
DRIFT_R_Y = 0.30
DRIFT_RATE = 0.42      # radians/second — one lap every ~15 s
BOB_RATE = 0.83        # different rate so x/z and y never re-sync

# Colour pulse: a firefly is a warm yellow-green that breathes.  These are the
# two ends; the pulse is a raised cosine between them.
DIM = (0.42, 0.55, 0.16)
BRIGHT = (1.00, 0.94, 0.42)
PULSE_RATE = 1.7       # radians/second

# Module-level defaults.  jce_script.vm.Instance falls through to the module
# on a read that misses the instance and writes land on the instance, so these
# are the initial values and `self.x = self.x + 1` is the increment.
starts = 0
frames = 0
found = 0
probe = None
swarm = ()
base = ()
phase = ()
t = 0.0


def on_start(self):
    self.starts = self.starts + 1
    self.probe = jce.find_by_name("EsPyProbe")[0]

    swarm_here = jce.find_by_prefix("Firefly_")
    base_here = []
    phase_here = []
    for i, e in enumerate(swarm_here):
        p = jce.get_position(e)
        # get_position is fallible_out: None when the entity has no
        # transform.  Tested for None and not for truth — (0,0,0) is a
        # legitimate position and `if p:` would discard it.
        base_here.append(p if p is not None else (0.0, 0.0, 0.0))
        # A stable per-firefly phase so twelve lamps never blink in unison.
        # Derived from the index, not from a clock: the same scene reload
        # produces the same swarm.
        phase_here.append((i * 2.399963) % 6.283185)

    self.swarm = swarm_here
    self.base = base_here
    self.phase = phase_here
    self.found = len(swarm_here)
    self.t = 0.0

    if self.probe:
        jce.set_position(self.probe, float(self.starts), 0.0,
                         float(self.found))

    jce.log("es_fireflies (python) online: " + str(self.found) +
            " fireflies, probe=" + ("yes" if self.probe else "MISSING"))


def on_update(self, dt):
    self.frames = self.frames + 1
    self.t = self.t + dt
    t = self.t

    swarm = self.swarm
    base = self.base
    phase = self.phase
    for i in range(len(swarm)):
        e = swarm[i]
        bx, by, bz = base[i]
        ph = phase[i]

        jce.set_position(
            e,
            bx + DRIFT_R_XZ * math.sin(t * DRIFT_RATE + ph),
            by + DRIFT_R_Y * math.sin(t * BOB_RATE + ph * 1.7),
            bz + DRIFT_R_XZ * math.cos(t * DRIFT_RATE + ph))

        # Raised cosine in [0,1]; each firefly on its own phase.
        k = 0.5 - 0.5 * math.cos(t * PULSE_RATE + ph)
        jce.particle_set_color(
            e,
            DIM[0] + (BRIGHT[0] - DIM[0]) * k,
            DIM[1] + (BRIGHT[1] - DIM[1]) * k,
            DIM[2] + (BRIGHT[2] - DIM[2]) * k)

    if self.probe:
        jce.set_position(self.probe, float(self.starts), float(self.frames),
                         float(self.found))
