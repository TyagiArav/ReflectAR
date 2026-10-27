`reflectar_setup.py` writes the materials to `/Game/ReflectAR/` (Content/ReflectAR/*.uasset).
`init_unreal.py` runs it on the first editor start. To rebuild them after changing the script:
Output Log > Python: `import importlib, reflectar_setup; importlib.reload(reflectar_setup); reflectar_setup.build()`.
