"""Create the Neural Post-Processing assets that run candy-9-clean.onnx over the screen.

Produces, next to the imported model in /Game/ML:
  NP_Candy   - NeuralProfile: model + runtime (NNERuntimeORTDml) + tiling
  PPM_Candy  - post process material: scene color -> candy -> emissive

Candy expects RGB in [0,255] (NCHW 1x3x224x224), so the material scales by 255 on the
way in and divides on the way out. It runs after tonemapping because the model was
trained on display-referred sRGB images.

Requires the NeuralRendering plugin. Re-runnable: existing assets are reused and rebuilt.

Headless:
  UnrealEditor-Cmd.exe SL_Test.uproject -run=pythonscript -script="<abs path to this file>"
"""
import unreal

MODEL_PATH = "/Game/ML/candy-9-clean"
ASSET_DIR = "/Game/ML"
PROFILE_NAME = "NP_Candy"
MATERIAL_NAME = "PPM_Candy"

# OneByOne scales the screen down to the model's fixed 224x224 and back up: blurry but one
# dispatch per frame, which is what we want for a first proof. AUTO tiles at full resolution
# (~45 dispatches at 1080p with this fixed batch-1 model).
TILE_TYPE = unreal.NeuralModelTileType.ONE_BY_ONE

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary


def log(msg):
    unreal.log("[setup_candy] " + msg)


def load_or_create(name, asset_class, factory):
    path = f"{ASSET_DIR}/{name}"
    if eal.does_asset_exist(path):
        log(f"reusing {path}")
        return eal.load_asset(path)
    log(f"creating {path}")
    return asset_tools.create_asset(name, ASSET_DIR, asset_class, factory)


def connect(src, src_out, dst, dst_in_candidates):
    for dst_in in dst_in_candidates:
        if mel.connect_material_expressions(src, src_out, dst, dst_in):
            return
    raise RuntimeError(f"could not connect {src.get_name()} -> {dst.get_name()} via {dst_in_candidates}")


model = eal.load_asset(MODEL_PATH)
if not model:
    raise RuntimeError(f"{MODEL_PATH} not found - import candy-9-clean.onnx there first")

# --- Neural profile ---------------------------------------------------------------
profile = load_or_create(PROFILE_NAME, unreal.NeuralProfile, unreal.NeuralProfileFactory())
settings = profile.get_editor_property("settings")
settings.set_editor_property("nne_model_data", model)
settings.set_editor_property("tile_size", TILE_TYPE)
# Setting the whole struct fires PostEditChangeProperty, which registers the profile and
# loads the model on the render thread.
profile.set_editor_property("settings", settings)

# --- Post process material ----------------------------------------------------------
material = load_or_create(MATERIAL_NAME, unreal.Material, unreal.MaterialFactoryNew())
mel.delete_all_material_expressions(material)
material.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
material.set_editor_property("blendable_location", unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING)
material.set_editor_property("neural_profile", profile)
mel.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_NEURAL_NETWORKS)

scene = mel.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -900, 0)
scene.set_editor_property("scene_texture_id", unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)

in_rgb = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -650, 0)
for c in ("r", "g", "b"):
    in_rgb.set_editor_property(c, True)

to_255 = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -450, 0)
to_255.set_editor_property("const_b", 255.0)

nn_in = mel.create_material_expression(material, unreal.MaterialExpressionNeuralNetworkInput, -250, 0)

nn_out = mel.create_material_expression(material, unreal.MaterialExpressionNeuralNetworkOutput, -900, 300)

out_rgb = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -650, 300)
for c in ("r", "g", "b"):
    out_rgb.set_editor_property(c, True)

from_255 = mel.create_material_expression(material, unreal.MaterialExpressionDivide, -450, 300)
from_255.set_editor_property("const_b", 255.0)

clamp = mel.create_material_expression(material, unreal.MaterialExpressionSaturate, -250, 300)

connect(scene, "Color", in_rgb, [""])
connect(in_rgb, "", to_255, ["A"])
connect(to_255, "", nn_in, ["Input0", "Input 0", "Input"])
connect(nn_out, "", out_rgb, [""])
connect(out_rgb, "", from_255, ["A"])
connect(from_255, "", clamp, [""])
if not mel.connect_material_property(clamp, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
    raise RuntimeError("could not connect to Emissive Color")

mel.layout_material_expressions(material)
mel.recompile_material(material)

for asset in (profile, material):
    eal.save_loaded_asset(asset, only_if_is_dirty=False)

log(f"done: {ASSET_DIR}/{PROFILE_NAME}, {ASSET_DIR}/{MATERIAL_NAME}")
