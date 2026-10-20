"""Author the LVL_RT_Room flythrough scenario: a camera, a camera-only Level Sequence, and the scenario actor.

The route reproduces the former hard-coded room loop (10 s at 30 fps, 80 degree FOV)
so existing room results keep the same framing. Run from the editor command line:

  UnrealEditor-Cmd.exe SL_Test.uproject -run=pythonscript -EnablePlugins=PythonScriptPlugin
      -script="<project>/Scripts/author_room_flythrough.py" -unattended -nosplash

Re-running replaces the sequence asset and the actors this script created.
"""
import math

import unreal

LEVEL = "/Game/Levels/LVL_RT_Room"
SEQUENCE_DIR = "/Game/Benchmark"
SEQUENCE_NAME = "LS_RT_Room_Flythrough"
CAMERA_LABEL = "BenchmarkCamera"
SCENARIO_LABEL = "FlythroughScenario"
SCENARIO_ID = "RT_Room"
BINDING_TAG = "BenchmarkCamera"
FPS = 30
FRAMES = 300
FOV = 80.0


def room_transform(t):
    """Same loop as the former SLFlythroughCameraPath::EvaluateRoom (cm)."""
    angle = t * 2.0 * math.pi
    pos = unreal.Vector(-150.0 + 450.0 * math.sin(angle),
                        200.0 + 200.0 * math.cos(angle),
                        300.0 + 25.0 * math.sin(angle))
    subject = unreal.Vector(-225.0, -450.0, 220.0)
    d = subject - pos
    yaw = math.degrees(math.atan2(d.y, d.x))
    pitch = math.degrees(math.atan2(d.z, math.hypot(d.x, d.y)))
    return pos, pitch, yaw


def main():
    unreal.EditorLoadingAndSavingUtils.load_map(LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for actor in actors.get_all_level_actors():
        if actor.get_actor_label() in (CAMERA_LABEL, SCENARIO_LABEL):
            actors.destroy_actor(actor)

    pos, pitch, yaw = room_transform(0.0)
    camera = actors.spawn_actor_from_class(unreal.CameraActor, pos, unreal.Rotator(roll=0.0, pitch=pitch, yaw=yaw))
    camera.set_actor_label(CAMERA_LABEL)
    camera_component = camera.camera_component
    camera_component.set_editor_property("field_of_view", FOV)
    camera_component.set_editor_property("constrain_aspect_ratio", False)

    path = f"{SEQUENCE_DIR}/{SEQUENCE_NAME}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    sequence = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        SEQUENCE_NAME, SEQUENCE_DIR, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    sequence.set_display_rate(unreal.FrameRate(FPS, 1))
    sequence.set_playback_start(0)
    sequence.set_playback_end(FRAMES)

    binding = sequence.add_possessable(camera)
    unreal.MovieSceneBindingTagExtensions.tag_binding(binding, BINDING_TAG)
    track = binding.add_track(unreal.MovieScene3DTransformTrack)
    section = track.add_section()
    section.set_range(0, FRAMES)
    channels = section.get_all_channels()  # loc xyz, rot roll/pitch/yaw, scale xyz
    previous_yaw = None
    for frame in range(FRAMES + 1):
        pos, pitch, yaw = room_transform(frame / FRAMES)
        # Keep yaw continuous so keys never interpolate the long way round.
        if previous_yaw is not None:
            yaw += 360.0 * round((previous_yaw - yaw) / 360.0)
        previous_yaw = yaw
        time = unreal.FrameNumber(frame)
        for channel, value in zip(channels[:6], (pos.x, pos.y, pos.z, 0.0, pitch, yaw)):
            channel.add_key(time, value, interpolation=unreal.MovieSceneKeyInterpolation.LINEAR)
    for channel in channels[6:9]:
        channel.set_default(1.0)

    scenario_class = unreal.load_class(None, "/Script/SL_Test.SLFlythroughScenario")
    scenario = actors.spawn_actor_from_class(scenario_class, unreal.Vector(0, 0, 0))
    scenario.set_actor_label(SCENARIO_LABEL)
    scenario.set_editor_property("scenario_id", SCENARIO_ID)
    scenario.set_editor_property("sequence", sequence)
    scenario.set_editor_property("camera", camera)
    scenario.set_editor_property("camera_binding_tag", BINDING_TAG)

    unreal.EditorAssetLibrary.save_loaded_asset(sequence)
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log(f"Authored {path} with {FRAMES} frames at {FPS} fps in {LEVEL}.")


main()
