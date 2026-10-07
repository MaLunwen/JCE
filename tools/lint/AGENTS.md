# tools/lint — public source gates

run_all.py aggregates independent public gates. Private workflow suites are
optional local additions and must be reported as unavailable when absent.
Checks inspect the real staged tree, enforce maintained contracts and need a
negative control when introduced. Never weaken a failing check or refresh a
baseline merely to make it green. No build recipe or user-project generation
belongs here. Source layout is declared by contracts/source-layout.json.
The shipped-main declaration gate recognizes static and JCE_INLINE definitions
without treating bare calls as declarations; negative callee controls live in
tools/tests/test_shipped_main_declarations.py.

The shader varying gate requires the complete declared IO interface for every
linked pair, including shared vertex stages. Unused fragment varyings remain
part of the bgfx link hash. Shared-stage mismatches have a failing control.
