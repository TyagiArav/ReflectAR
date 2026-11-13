"""Runs at every editor start (Python Editor Script Plugin): builds the materials if they are
missing or were built by an older reflectar_setup.py."""
import unreal

_handle = None


def _build_once(_dt):
    global _handle
    unreal.unregister_slate_post_tick_callback(_handle)
    import reflectar_setup
    if unreal.EditorAssetLibrary.does_asset_exist("/Game/ReflectAR/M_Proxy") and reflectar_setup.is_current():
        return
    reflectar_setup.build()


# wait for the first editor tick: the asset registry and material editor are ready by then
_handle = unreal.register_slate_post_tick_callback(_build_once)
