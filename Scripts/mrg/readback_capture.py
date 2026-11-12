"""Read saved graph settings in an independent Unreal Python commandlet."""
import json
from pathlib import Path
import unreal

settings = json.loads(Path(__file__).with_name('capture_config.json').read_text())
records = []
for spec in settings['graphs']:
    graph = unreal.load_asset(spec['asset'])
    pt = graph.get_node_for_branch(unreal.MovieGraphPathTracerRenderPassNode, 'DefaultLayer')
    geo = graph.get_node_for_branch(unreal.MovieGraphDeferredRenderPassNode, settings['geometry_branch'])
    output = graph.get_node_for_branch(unreal.MovieGraphGlobalOutputSettingNode, 'Globals')
    record = {'asset': spec['asset'], 'spp': pt.spatial_sample_count,
              'tone_curve_disabled': pt.disable_tone_curve,
              'pt_aux_count': len(pt.additional_post_process_materials),
              'geometry_spp': geo.spatial_sample_count, 'geometry_aa': str(geo.anti_aliasing_method),
              'aux': [str(p) for p in geo.additional_post_process_materials],
              'output': str(output.output_directory),
              'branches': {str(b): [n.get_class().get_name() for n in graph.get_nodes_for_branch(unreal.MovieGraphNode, b)] for b in graph.get_branch_names()}}
    record['cvars'] = {n.get_editor_property('name'): n.value for n in graph.get_nodes_for_branch(unreal.MovieGraphSetCVarValueNode, 'Globals')}
    record['beauty_filename'] = graph.get_node_for_branch(unreal.MovieGraphImageSequenceOutputNode_EXR, 'DefaultLayer').file_name_format
    record['aux_filename'] = geo.ppm_file_name_format
    record['resolution'] = str(output.output_resolution)
    record['range_start'] = str(output.custom_playback_range_start)
    record['range_end'] = str(output.custom_playback_range_end)
    export = unreal.AssetExportTask()
    export.object = graph
    export.filename = str(Path(unreal.Paths.project_dir()).resolve() / 'Saved/Diagnostics' / (graph.get_name() + '_final.copy'))
    export.automated = True
    export.prompt = False
    export.replace_identical = True
    record['text_export'] = unreal.Exporter.run_asset_export_task(export)
    assert pt.spatial_sample_count == spec['spp']
    assert pt.disable_tone_curve and not pt.additional_post_process_materials
    assert geo.spatial_sample_count == 1 and len(geo.additional_post_process_materials) == 5
    records.append(record)
destination = Path(unreal.Paths.project_dir()).resolve() / 'Saved/Diagnostics/config_disk_readback.json'
destination.write_text(json.dumps(records, indent=2), encoding='utf-8')
unreal.log('RR_DISK_READBACK_OK ' + str(destination))
