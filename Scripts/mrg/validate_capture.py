"""Render a short, isolated queue using separate validation copies of the graphs."""
import datetime
import json
from pathlib import Path

import unreal

settings = json.loads(Path(__file__).with_name('capture_config.json').read_text())
test = settings['validation']
project = Path(unreal.Paths.project_dir()).resolve()
stamp = datetime.datetime.now().astimezone().strftime('%Y%m%d_%H%M%S')
out = project / 'Saved' / 'Diagnostics' / ('render_validation_' + stamp)
out.mkdir(parents=True)
subsystem = unreal.get_editor_subsystem(unreal.MoviePipelineQueueSubsystem)
if subsystem.is_rendering():
    raise RuntimeError('An existing render is running; leaving it untouched')
queue = unreal.MoviePipelineQueue()
graphs = []
result = {'start': datetime.datetime.now().astimezone().isoformat(), 'settings': test,
          'status': 'preparing', 'errors': [], 'jobs': []}


def override(node, name, value):
    node.set_editor_property(name, value)
    node.set_editor_property('override_' + name, True)


def write_result():
    (out / 'validation_run.json').write_text(json.dumps(result, indent=2), encoding='utf-8')


for spec in settings['graphs']:
    if spec['spp'] not in test['graph_spp']:
        continue
    source = unreal.load_asset(spec['asset'])
    graph = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(
        source.get_name() + '_Validation', test['asset_directory'] + '/' + stamp, source)
    if graph is None:
        raise RuntimeError('Could not create isolated validation graph')
    graphs.append(graph)
    output = graph.get_node_for_branch(unreal.MovieGraphGlobalOutputSettingNode, 'Globals')
    override(output, 'output_directory', unreal.DirectoryPath(str(out / (str(spec['spp']) + 'spp'))))
    resolution = unreal.MovieGraphLibrary.named_resolution_from_size(test['width'], test['height'])
    override(output, 'output_resolution', resolution)
    start = output.custom_playback_range_start
    start.set_editor_property('type', unreal.MovieGraphSequenceRangeType.CUSTOM)
    start.set_editor_property('value', test['start_frame'])
    end = output.custom_playback_range_end
    end.set_editor_property('type', unreal.MovieGraphSequenceRangeType.CUSTOM)
    end.set_editor_property('value', test['start_frame'] + test['frame_count'])
    override(output, 'custom_playback_range_start', start)
    override(output, 'custom_playback_range_end', end)
    if not unreal.EditorAssetLibrary.save_loaded_asset(graph, only_if_is_dirty=False):
        raise RuntimeError('Could not save validation graph')
    job = queue.allocate_new_job(unreal.MoviePipelineExecutorJob)
    job.job_name = 'CaptureValidation_' + str(spec['spp']) + 'spp'
    job.map = unreal.SoftObjectPath(test['map'])
    job.sequence = unreal.SoftObjectPath(test['sequence'])
    job.set_graph_preset(graph)
    result['jobs'].append({'source': spec['asset'], 'spp': spec['spp']})


def on_error(executor, pipeline, fatal, error_text):
    result['errors'].append({'fatal': bool(fatal), 'message': str(error_text)})
    write_result()


def on_finished(executor, success):
    result['success'] = bool(success)
    result['status'] = 'finished'
    result['end'] = datetime.datetime.now().astimezone().isoformat()
    write_result()
    unreal.log('RR_VALIDATION_FINISHED ' + str(success) + ' ' + str(out))


executor = unreal.MoviePipelinePIEExecutor()
executor.on_executor_errored_delegate.add_callable_unique(on_error)
executor.on_executor_finished_delegate.add_callable_unique(on_finished)
result['status'] = 'rendering'
write_result()
(project / 'Saved/Diagnostics/latest_validation.txt').write_text(str(out), encoding='utf-8')
subsystem.render_queue_instance_with_executor_instance(queue, executor)
