# Tower Defense Web consumer

The public SDK C99 controller is linked into the Web executable. Normal launch
uses one canvas; `?qa` collects actual runtime logs and registry receipts.
`fixture` selects frozen reference states; realtime gameplay remains separate.
Normal motion, firing and waves share the bounded fixed 60 Hz simulation clock.
`?legacy-frame` is an explicit historical diagnostic only.

Build from the repository root using the supported driver:

```powershell
python scripts/jce.py sdk --arch wasm --variant dist --no-debug --profiling off
python scripts/jce.py app tests/consumers/td_lab --arch wasm --variant dist --sdk dist/sdk/wasm-dist
```

A native C scene class requires an actual browser registry execution receipt.
The first build writes `build/native-registry-proof/web-request.json`; a pending
execution is not a successful admission. After running that exact HTML and
collecting the request-bound receipt, use the same command with
`--registry-proof <receipt.json>`. This audits the existing artifact without
repacking or relinking. Changed artifacts or stale receipts are rejected.

The shared SDK helper embeds the archive into MEMFS and inlines wasm/bootstrap
with `SINGLE_FILE=1`. Deployment contains only `TowerDefense.html`; companion
wasm/JS/data files do not satisfy the single-file delivery contract. HTTP is the
browser transport. Verify actual relocation, network dependencies, input,
rendering, cleanup and timing separately from a structural HTML audit.

`timing&frames=N` exports the real engine phase CSV. RAF evidence measures
presentation scheduling and callback CPU time; it cannot substitute for GPU
execution measurements. Collect timing without screenshots, readbacks, GL
queries, other rendering applications or build/test activity. Preserve every
repeat and name the browser, viewport, GPU adapter and artifact hash.

New final output belongs in `dist/apps/td_lab/wasm32-dist`; native GUI output
belongs in `dist/apps/td_lab/windows-x86_64-dist`. Build intermediates and new
measurement files stay under `tests/consumers/td_lab/build`, SDKs under
`dist/sdk`. Earlier raw evidence remains at its original ignored
`examples/td_lab/build` paths with unchanged artifact identities.
