"""Project parity parser for actual CPU TAA submissions, scoped to host views."""
import re

# contracts/view-reservations.txt: Game120 + postfx30; native100/default20.
RESOLVE = re.compile(r"taa resolve: (on|off)\b([^\n]*)")
VIEW = re.compile(r"\bview_base=(\d+)\b")

def resolve_taa(log, host):
    if host not in ("editor", "native"):
        raise ValueError("explicit editor/native host required")
    records = []
    for match in RESOLVE.finditer(log):
        state, suffix = match.groups()
        view = VIEW.search(suffix)
        if "view_base=" in suffix and view is None:
            raise ValueError("malformed TAA view identity")
        records.append((state, int(view.group(1)) if view else None))
    if not records:
        raise ValueError("actual TAA resolve submission marker unavailable")
    annotated = [record for record in records if record[1] is not None]
    legacy = [record for record in records if record[1] is None]
    if annotated and legacy:
        raise ValueError("mixed annotated/legacy TAA logs cannot select a reliable viewport")
    if legacy:
        return dict(state=legacy[-1][0], host=host, selected_view=None,
            source="LEGACY_UNSCOPED_LAST_RESOLVE", legacy=True,
            scope="CPU submission marker only; selected viewport cannot be proven",
            limitations=["entire log lacks view annotations; last pipeline may be another viewport",
                         "not GPU completion or presentation"])
    preferred = (150,) if host == "editor" else (100, 20)
    for view in preferred:
        selected = [state for state, identity in annotated if identity == view]
        if selected:
            return dict(state=selected[-1], host=host, selected_view=view,
                source="ANNOTATED_VIEW_RESOLVE", legacy=False,
                scope="CPU resolve submission state for selected viewport only",
                limitations=["not GPU completion or presentation"])
    raise ValueError("actual TAA resolve marker missing for selected " + host + " viewport")
