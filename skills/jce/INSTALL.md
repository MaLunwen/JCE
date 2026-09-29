# JCE skill installation

The default en-US source is skills/jce; the zh-CN edition is skills/jce-zh-cn.
Cloud hosts can read either directly from this repository.
For local discovery, link the runtime's jce entry to the default source and
its jce-zh-cn entry to the translated source. Preserve an existing directory
before replacing it with a link. Windows uses junctions; Unix uses symlinks.
Repository edits immediately reach linked hosts. Private extensions are excluded.
