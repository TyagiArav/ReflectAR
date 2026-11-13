`reflectar_setup.py` writes the materials to `/Game/ReflectAR/` (Content/ReflectAR/*.uasset).
`init_unreal.py` runs it on the first editor start, and again when `VERSION` in the script differs from
the one the materials were built with (`Content/ReflectAR/materials.version`). A packaged app has to be
re-packaged to pick up new materials. To rebuild them after changing the script:
Output Log > Python: `import importlib, reflectar_setup; importlib.reload(reflectar_setup); reflectar_setup.build()`.
