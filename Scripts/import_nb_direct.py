"""Import the nb_direct denoiser model and expose it in the GM tool's denoiser list.

1. Imports the rewrapped ONNX (Scripts/rewrap_rayrepair_onnx.py --encoding noisebase)
   as the NNE model data asset /Game/ML/nb_direct, replacing an existing one.
2. Adds "NB Direct" (SL.Denoiser.Method 6) to the SL.Denoiser.Method radio group in
   /Game/Debug/GMTools/DA_RenderGMTools if it is not there yet.

Run with the editor closed (or from a separate checkout):

  UnrealEditor-Cmd.exe SL_Test.uproject -run=pythonscript -EnablePlugins=PythonScriptPlugin
      -script="<project>/Scripts/import_nb_direct.py C:/path/to/nb_direct_noisebase.onnx" -unattended -nosplash

Use forward slashes in the .onnx path: the commandlet unescapes backslashes, so
"...\\Downloads\\nb_direct..." turns "\\n" into a newline.

Re-running is safe: the asset is reimported and the radio option is not duplicated.
"""
import os
import sys

import unreal

MODEL_DIR = "/Game/ML"
MODEL_NAME = "nb_direct"
GM_CONFIG = "/Game/Debug/GMTools/DA_RenderGMTools"
METHOD_CVAR = "SL.Denoiser.Method"
OPTION_LABEL = "6 - NB Direct (NoiseBase U-Net, locks auto-exposure)"
OPTION_VALUE = 6


def import_model(onnx_path):
    # A directory here would make AssetImportTask import everything under it.
    if not onnx_path.lower().endswith(".onnx") or not os.path.isfile(onnx_path):
        raise RuntimeError(f"not an existing .onnx file: {onnx_path!r} (use forward slashes)")
    task = unreal.AssetImportTask()
    task.filename = onnx_path
    task.destination_path = MODEL_DIR
    task.destination_name = MODEL_NAME
    task.replace_existing = True
    task.automated = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    paths = list(task.imported_object_paths)
    if not paths:
        raise RuntimeError(f"importing {onnx_path} produced no asset")
    asset = unreal.load_asset(paths[0])
    unreal.log(f"import_nb_direct: imported {onnx_path} -> {paths[0]} ({type(asset).__name__})")
    if type(asset).__name__ != "NNEModelData":
        raise RuntimeError(f"expected an NNEModelData asset, got {type(asset).__name__}")


def add_radio_option():
    config = unreal.load_asset(GM_CONFIG)
    if config is None:
        raise RuntimeError(f"could not load {GM_CONFIG}")
    entries = config.get_editor_property("entries")
    for index, entry in enumerate(entries):
        if (entry.get_editor_property("console_variable") != METHOD_CVAR
                or entry.get_editor_property("control") != unreal.SLGMControlType.RADIO_GROUP):
            continue
        options = entry.get_editor_property("radio_options")
        listed = [(str(o.get_editor_property("label")), o.get_editor_property("value")) for o in options]
        unreal.log(f"import_nb_direct: {METHOD_CVAR} options before: {listed}")
        existing = [o for o in options if o.get_editor_property("value") == OPTION_VALUE]
        if existing and all(str(o.get_editor_property("label")) == OPTION_LABEL for o in existing):
            unreal.log(f"import_nb_direct: value {OPTION_VALUE} already listed; leaving the GM config unchanged")
            return
        if existing:
            for o in existing:
                o.set_editor_property("label", unreal.Text(OPTION_LABEL))
        else:
            option = unreal.SLGMRadioOption()
            option.set_editor_property("label", unreal.Text(OPTION_LABEL))
            option.set_editor_property("value", OPTION_VALUE)
            options.append(option)
        entry.set_editor_property("radio_options", options)
        entries[index] = entry
        config.set_editor_property("entries", entries)
        if not unreal.EditorAssetLibrary.save_loaded_asset(config, only_if_is_dirty=False):
            raise RuntimeError(f"failed to save {GM_CONFIG}")
        unreal.log(f"import_nb_direct: {'relabelled' if existing else 'added'} '{OPTION_LABEL}' = {OPTION_VALUE} "
                   f"in {METHOD_CVAR} of {GM_CONFIG}")
        return
    raise RuntimeError(f"no {METHOD_CVAR} radio group in {GM_CONFIG}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not args:
        raise RuntimeError("usage: import_nb_direct.py <nb_direct_noisebase.onnx>")
    import_model(args[0])
    add_radio_option()
    unreal.log("import_nb_direct: done")


main()
