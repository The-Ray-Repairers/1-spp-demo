"""Run inside Unreal Editor: py "<project>/Scripts/mrg/apply_capture_config.py".

Settings are in the neighboring capture_config.json. This updates only the named
graphs, with filesystem backups and readback, leaving maps and sequences intact.
"""
import datetime
import hashlib
import json
import runpy
import shutil
from pathlib import Path

import unreal

SETTINGS = json.loads(Path(__file__).with_name('capture_config.json').read_text())
PROJECT = Path(unreal.Paths.project_dir()).resolve()
STAMP = datetime.datetime.now().astimezone().strftime('%Y%m%d_%H%M%S')
OUT = PROJECT / 'Saved' / 'Diagnostics' / ('config_repair_' + STAMP)
OUT.mkdir(parents=True, exist_ok=False)


def override(node, property_name, value, override_name=None):
    node.set_editor_property(property_name, value)
    node.set_editor_property(override_name or ('override_' + property_name), True)


def edge(graph, first, second, target_pin=''):
    # UE 5.8 returns whether an OLD edge was broken, not success.
    graph.add_labeled_edge(first, '', second, target_pin)
    if not any(second in pin.get_connected_nodes() for pin in first.get_output_pins()):
        raise RuntimeError('Cannot connect ' + first.get_name() + ' to ' + second.get_name())


def describe(node):
    result = {'class': node.get_class().get_name(), 'path': node.get_path_name()}
    for key in dir(node):
        if key.startswith('_'):
            continue
        try:
            result[key] = str(node.get_editor_property(key))
        except Exception:
            pass
    return result


def snapshot(graph):
    return {str(branch): [describe(n) for n in graph.get_nodes_for_branch(unreal.MovieGraphNode, branch)]
            for branch in graph.get_branch_names()}


graphs = []
manifest = {'start': datetime.datetime.now().astimezone().isoformat(), 'settings': SETTINGS, 'assets': []}
for spec in SETTINGS['graphs']:
    graph = unreal.load_asset(spec['asset'])
    if not isinstance(graph, unreal.MovieGraphConfig):
        raise RuntimeError('Not a MovieGraphConfig: ' + spec['asset'])
    pt = graph.get_node_for_branch(unreal.MovieGraphPathTracerRenderPassNode, 'DefaultLayer')
    if not pt:
        raise RuntimeError('Missing existing Path Tracer in DefaultLayer')
    src = PROJECT / 'Content' / (spec['asset'].removeprefix('/Game/') + '.uasset')
    backup = OUT / src.relative_to(PROJECT)
    backup.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, backup)
    manifest['assets'].append({'asset': spec['asset'], 'backup': str(backup),
                               'sha256_before': hashlib.sha256(src.read_bytes()).hexdigest(),
                               'before': snapshot(graph)})
    graphs.append((spec, graph, pt))
(OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')

with unreal.ScopedEditorTransaction('Repair MRG dataset capture configuration'):
    for spec, graph, pt in graphs:
        graph.modify()
        pt.modify()
        geometry = graph.get_node_for_branch(unreal.MovieGraphDeferredRenderPassNode, SETTINGS['geometry_branch']) if SETTINGS['geometry_branch'] in [str(b) for b in graph.get_branch_names()] else None
        passes = list(pt.additional_post_process_materials)
        if not passes and geometry:
            passes = list(geometry.additional_post_process_materials)
        if not passes:
            raise RuntimeError('No source auxiliary materials; refusing to invent a mapping')
        for material_pass in passes:
            material_pass.set_editor_property('high_precision_output', True)
            material_pass.set_editor_property('use_lossless_compression', True)
        if geometry is None:
            if SETTINGS['geometry_branch'] not in [str(b) for b in graph.get_branch_names()]:
                graph.add_output(SETTINGS['geometry_branch'])
            # Resume an interrupted first application without adding orphan nodes.
            owned = [n for n in unreal.ObjectIterator(unreal.MovieGraphNode) if n.get_outer() == graph]
            candidates = [n for n in owned if isinstance(n, unreal.MovieGraphDeferredRenderPassNode)]
            geometry = candidates[0] if len(candidates) == 1 else graph.create_node_by_class(unreal.MovieGraphDeferredRenderPassNode)
            candidates = [n for n in owned if isinstance(n, unreal.MovieGraphImageSequenceOutputNode_EXR) and n.file_name_format == 'geometry_unused/{frame_number}']
            writer = candidates[0] if candidates else graph.create_node_by_class(unreal.MovieGraphImageSequenceOutputNode_EXR)
            candidates = [n for n in owned if isinstance(n, unreal.MovieGraphRenderLayerNode) and n.layer_name == 'geometry']
            layer = candidates[0] if candidates else graph.create_node_by_class(unreal.MovieGraphRenderLayerNode)
            override(layer, 'layer_name', 'geometry')
            override(writer, 'file_name_format', 'geometry_unused/{frame_number}')
            edge(graph, geometry, writer)
            edge(graph, writer, layer)
            edge(graph, layer, graph.get_output_node(), SETTINGS['geometry_branch'])
        override(geometry, 'spatial_sample_count', 1)
        override(geometry, 'anti_aliasing_method', unreal.AntiAliasingMethod.AAM_NONE)
        override(geometry, 'disable_tone_curve', True, 'override_b_disable_tone_curve')
        override(geometry, 'allow_ocio', False, 'override_b_allow_ocio')
        override(geometry, 'include_beauty_render_in_output', False, 'override_b_include_beauty_render_in_output')
        override(geometry, 'additional_post_process_materials', passes)
        override(geometry, 'ppm_file_name_format', SETTINGS['geometry_file_name'])
        override(pt, 'additional_post_process_materials', [])
        override(pt, 'spatial_sample_count', spec['spp'])
        override(pt, 'disable_tone_curve', True, 'override_b_disable_tone_curve')
        override(pt, 'allow_ocio', False, 'override_b_allow_ocio')
        override(pt, 'enable_denoiser', False, 'override_b_enable_denoiser')
        override(pt, 'enable_reference_motion_blur', False, 'override_b_enable_reference_motion_blur')
        for writer in graph.get_nodes_for_branch(unreal.MovieGraphImageSequenceOutputNode_EXR, 'DefaultLayer'):
            override(writer, 'file_name_format', SETTINGS['beauty_file_name'])
        # An HDR render cannot also produce a correctly tone-mapped browsing PNG
        # through the same branch. Disable PNG while retaining graph wiring.
        for writer in graph.get_nodes_for_branch(unreal.MovieGraphImageSequenceOutputNode_PNG, 'DefaultLayer'):
            writer.set_disabled(True)
        output = graph.get_node_for_branch(unreal.MovieGraphGlobalOutputSettingNode, 'Globals')
        override(output, 'output_directory', unreal.DirectoryPath(SETTINGS['output_directory'].replace('{spp}', str(spec['spp']))))
        sampling = graph.get_node_for_branch(unreal.MovieGraphSamplingMethodNode, 'Globals')
        override(sampling, 'temporal_sample_count', 1)
        for name, value in SETTINGS['console_variables'].items():
            existing = [n for n in graph.get_nodes_for_branch(unreal.MovieGraphSetCVarValueNode, 'Globals') if n.get_editor_property('name') == name]
            node = existing[0] if existing else graph.insert_before(graph.get_output_node(), unreal.MovieGraphSetCVarValueNode, 'Globals')
            override(node, 'name', name)
            override(node, 'value', float(value))

    for spec, graph, pt in graphs:
        assert pt.spatial_sample_count == spec['spp'] and pt.disable_tone_curve
        assert not pt.additional_post_process_materials
        assert graph.get_node_for_branch(unreal.MovieGraphPathTracerRenderPassNode, 'DefaultLayer') == pt
        assert graph.get_node_for_branch(unreal.MovieGraphImageSequenceOutputNode_EXR, 'DefaultLayer')
        assert graph.get_node_for_branch(unreal.MovieGraphDeferredRenderPassNode, SETTINGS['geometry_branch'])
        if not unreal.EditorAssetLibrary.save_loaded_asset(graph, only_if_is_dirty=False):
            raise RuntimeError('Failed to save ' + spec['asset'])

runpy.run_path(str(Path(__file__).with_name('refresh_graph_editor.py')))

for record, (spec, graph, _) in zip(manifest['assets'], graphs):
    record['after'] = snapshot(graph)
    src = PROJECT / 'Content' / (spec['asset'].removeprefix('/Game/') + '.uasset')
    record['sha256_after'] = hashlib.sha256(src.read_bytes()).hexdigest()
manifest['end'] = datetime.datetime.now().astimezone().isoformat()
manifest['status'] = 'saved; GPU validation pending'
(OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
(PROJECT / 'Saved/Diagnostics/latest_config_repair.txt').write_text(str(OUT), encoding='utf-8')
unreal.log('RR_CONFIG_REPAIR_SAVED ' + str(OUT))
