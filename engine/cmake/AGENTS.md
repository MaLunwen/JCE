# engine/cmake/ — engine build and SDK installation

JCESDKInstall.cmake owns SDK archive closure, merge and installation. Shared
installed consumer contracts belong to cmake/ and follow cmake/AGENTS.md.
Original third-party packages and cached sources remain immutable; acquisition
uses the existing verified vendor owner rather than copied source patches.

Archive target metadata is emitted by cmake/JCESDKTarget.cmake from the actual
compiler and owned library properties. Publish it beside the selected
lib/<CONFIG> archives after the generated CRT/schema check. Additive installs
must not stamp another configuration on behalf of old archives. Include the
shared metadata module in the installed CMake package; CLI and editor readers
consume those same files without deriving architecture from prefix names.

A declared compatible target is early admission, not compiler/link/runtime or
single-file shipment evidence. Refresh and consume the installed SDK with the
real selected target before making those claims.

The private Web query JS bridge installs beside JCESDKHelpers.cmake. Both the
repo and relocated SDK resolve the same owned bridge; a missing file refuses
configuration rather than silently losing the query adapter. It is embedded
by Emscripten into the single HTML, not a runtime sidecar or external VM.
Native single-executable publication and archive merging retain their owners.
