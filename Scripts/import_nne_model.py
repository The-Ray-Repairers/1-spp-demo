"""Import an ONNX file as an NNE model data asset, replacing an existing asset of the same name.

Run with the editor closed:

  UnrealEditor-Cmd.exe SL_Test.uproject -run=pythonscript -EnablePlugins=PythonScriptPlugin
      -script="<project>/Scripts/import_nne_model.py C:/path/to/model.onnx [/Game/ML] [asset_name]" -unattended -nosplash

The destination folder defaults to /Game/ML and the asset name to the file name without .onnx.
Use forward slashes in the .onnx path: the commandlet unescapes backslashes.

Example: the recurrent motion-aware model for SL.Denoiser.Method 7
  .../import_nne_model.py C:/path/to/rayrepair_motion_s3area_1920x1080.onnx
creates /Game/ML/rayrepair_motion_s3area_1920x1080, the default of SL.Denoiser.RayRepairMotion.Model.
"""
import os
import sys

import unreal


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not args:
        raise RuntimeError("usage: import_nne_model.py <model.onnx> [/Game/Folder] [asset_name]")
    onnx_path = args[0]
    dest = args[1] if len(args) > 1 else "/Game/ML"
    name = args[2] if len(args) > 2 else os.path.splitext(os.path.basename(onnx_path))[0]
    # A directory here would make AssetImportTask import everything under it.
    if not onnx_path.lower().endswith(".onnx") or not os.path.isfile(onnx_path):
        raise RuntimeError(f"not an existing .onnx file: {onnx_path!r} (use forward slashes)")

    task = unreal.AssetImportTask()
    task.filename = onnx_path
    task.destination_path = dest
    task.destination_name = name
    task.replace_existing = True
    task.automated = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    paths = list(task.imported_object_paths)
    if not paths:
        raise RuntimeError(f"importing {onnx_path} produced no asset")
    asset = unreal.load_asset(paths[0])
    if type(asset).__name__ != "NNEModelData":
        raise RuntimeError(f"expected an NNEModelData asset, got {type(asset).__name__}")
    unreal.log(f"import_nne_model: imported {onnx_path} -> {paths[0]}")


main()
