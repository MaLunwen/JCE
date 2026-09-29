# tools/lint — public source gates

run_all.py aggregates independent public gates. Private workflow suites are
optional local additions and must be reported as unavailable when absent.
Checks inspect the real staged tree, enforce maintained contracts and need a
negative control when introduced. Never weaken a failing check or refresh a
baseline merely to make it green. No build recipe or user-project generation
belongs here. Source layout is declared by contracts/source-layout.json.
