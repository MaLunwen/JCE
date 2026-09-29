# Build and gates

Use python scripts/jce.py --help and the supported target/variant options.
The entry point forwards to tools/build/jce.py, which owns prerequisite order.

- python scripts/jce.py lint: public source gates and architecture audit.
- python scripts/jce.py test: build the suite, then execute it.
- python scripts/jce.py editor: editor build.
- python scripts/jce.py sdk: refresh installed SDK after public-header edits.
- python scripts/jce.py smoke: installed-SDK and scripting consumers.
- python scripts/jce.py accept --project <dir>: shipped consumer acceptance.

Source pins come from contracts/vendor-sources.json. Fetch only new cache trees;
never repair original dependencies or Conan packages. A public clone does not
need docs, private workflows, user assets or a locally installed skill. Private
checks run only when their separate installation exists and must be reported
separately. Check staged paths before delivery. No commit or push if reserved
by the user. Run HEAD-based ABI validation again after the user's final commit.
