"""Rebuild the saved MRG editor representation after runtime graph edits.

UE documents PipelineEdGraph reset for scripted graph mutations. The Kismet
property setters update editor-only fields on the configured graph objects.
"""
import json
from pathlib import Path
import unreal

settings = json.loads(Path(__file__).with_name('capture_config.json').read_text())
subsystem = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem)
property_library = unreal.get_default_object(unreal.SystemLibrary)


def set_internal(obj, name, value):
    if value == 'None':
        property_library.call_method('SetObjectPropertyByName', (obj, name, None))
    else:
        property_library.call_method('SetIntPropertyByName', (obj, name, value))


for spec in settings['graphs']:
    graph = unreal.load_asset(spec['asset'])
    before = {str(b): [n.get_path_name() for n in graph.get_nodes_for_branch(unreal.MovieGraphNode, b)] for b in graph.get_branch_names()}
    subsystem.close_all_editors_for_asset(graph)
    graph.modify()
    nodes = [n for n in unreal.ObjectIterator(unreal.MovieGraphNode) if n.get_outer() == graph]
    for node in nodes:
        set_internal(node, 'GraphNode', 'None')
    for row, branch in enumerate(['Globals', 'DefaultLayer', settings['geometry_branch']]):
        chain = list(reversed(graph.get_nodes_for_branch(unreal.MovieGraphNode, branch)))
        for i, node in enumerate(chain):
            # Wrap the global settings into two rows, then place render branches below.
            x = (i % 7) * 380
            y = (i // 7) * 200 if row == 0 else (row + 1) * 220
            set_internal(node, 'NodePosX', x)
            set_internal(node, 'NodePosY', y)
    set_internal(graph.get_input_node(), 'NodePosX', -350)
    set_internal(graph.get_input_node(), 'NodePosY', 0)
    set_internal(graph.get_output_node(), 'NodePosX', 2800)
    set_internal(graph.get_output_node(), 'NodePosY', 440)
    set_internal(graph, 'PipelineEdGraph', 'None')
    subsystem.open_editor_for_assets([graph])
    after = {str(b): [n.get_path_name() for n in graph.get_nodes_for_branch(unreal.MovieGraphNode, b)] for b in graph.get_branch_names()}
    if before != after:
        raise RuntimeError('Editor graph reconstruction changed runtime traversal')
    if not unreal.EditorAssetLibrary.save_loaded_asset(graph, only_if_is_dirty=False):
        raise RuntimeError('Could not save editor graph ' + spec['asset'])
unreal.log('RR_GRAPH_EDITOR_REFRESHED')
