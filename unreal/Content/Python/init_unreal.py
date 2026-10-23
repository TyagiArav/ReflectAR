"""Runs at every editor start (Python Editor Script Plugin): builds the materials once if missing."""
import unreal

_handle = None


def _build_once(_dt):
    global _handle
    unreal.unregister_slate_post_tick_callback(_handle)
    if unreal.EditorAssetLibrary.does_asset_exist("/Game/ReflectAR/M_Proxy"):
        return
    import reflectar_setup
    reflectar_setup.build()


# wait for the first editor tick: the asset registry and material editor are ready by then
_handle = unreal.register_slate_post_tick_callback(_build_once)
