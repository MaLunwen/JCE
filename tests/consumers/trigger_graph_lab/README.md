# Trigger Graph Lab

Open this directory's `jce_project.json` in JCE Editor. Select
`WestGateTrigger` in the scene hierarchy to inspect the trigger and its
`target` link to `SignalBeacon`. The AABB center is world (0, 1, 8).
The parent's (4, 0, 8) and the child's (-4, 1, 0) make incorrect viewport
placement visible.

Press Play and focus Game View. Use the top-row number keys in order, pausing
briefly at station 2.

| Key | Observer station | Expected event | Beacon |
| --- | --- | --- | --- |
| 1 | Outside left | None at startup | On the pedestal |
| 2 | Inside | Enter, then Stay | Rises 2 units and stays raised |
| 3 | Outside right | Exit | Returns to the pedestal |

The engine delivers the event to the zone's scene-bound Lua script. The rule
checks the authored `west_gate` tag before moving the remapped `target` or
reporting the first Stay for that visit. The log prefix is
`TRIGGER_GRAPH_LAB event=`. A disabled Stay setting suppresses only Stay.
A different tag suppresses the fixture's actions without changing overlap.
Stop resets the Play scene. Repeat the same keys in a standalone build and
in a reopened Bundle to exercise the same scene and scripts.

This fixture uses one runtime viewer (the primary camera), procedural meshes
and the public script API. The observer payload is the runtime's opaque viewer
identifier. The Lua rule table illustrates Event, Condition and Action
authoring. Edit it in the script asset, with the trigger volume and target link
authored in the scene.

From the repository root, verify the source scene with the public scene tool.
Build and package separately with the matching installed SDK.

```text
python tools/jce_scene_kit.py check tests/consumers/trigger_graph_lab/resources/scenes/trigger_graph_lab.scene.json
lua tests/consumers/trigger_graph_lab/test_rules.lua tests/consumers/trigger_graph_lab
python scripts/jce.py app tests/consumers/trigger_graph_lab --variant dist --sdk <sdk>
python scripts/jce.py bundle --project tests/consumers/trigger_graph_lab --scene tests/consumers/trigger_graph_lab/resources/scenes/trigger_graph_lab.scene.json --single-file --sdk <sdk> --out <build-output-directory>
```

### Open the packaged scene

In JCE Editor 0.13.4, use **File → Open Bundle**, select the single
`.jbundle` in the native file picker. You can also paste its absolute path
into the Bundle / catalog path field and press Enter or choose **Open**.
A path copied with surrounding quotation marks works too. The bundle preview is
read-only. Open `jce_project.json` separately to edit the authoring project.

The command above preserves portable content (`target_profile: auto`).
Platform-specific cooking is optional. The Open tab's compatibility target
only applies to package inspection. Actual opening uses the current editor
platform and its supported content version.

The project requires Lua only. All three procedural meshes share one original
1×1 white texture. Native and visual acceptance require a built Editor and
application, plus the actual packaged Bundle.

The Lua test uses a fake SDK host to exercise a remapped target, a wrong tag,
repeat visits and physical key edges. Verify native trigger dispatch and the
scene view separately through Play and the packaged application.
