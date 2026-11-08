"""Compatibility entry for the maintained 104-entity, lazy-prefab authoring tool.

Keep one generator: the old maximum-capacity actor scene must not be rebuilt.
"""
if __package__:
    from .author_reference_scene import main
else:
    from author_reference_scene import main


if __name__ == "__main__":
    main()
