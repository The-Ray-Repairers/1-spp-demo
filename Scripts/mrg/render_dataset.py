"""Render a dataset sequence with one MRG preset into an arbitrary output folder.

Usage (editor console):
    py "<project>/Scripts/mrg/render_dataset.py" --spp 512 --out F:/Datasets/LS_GothicCastle_V02/512spp
    py "<project>/Scripts/mrg/render_dataset.py" --spp 512 --out <dir> --start 240 --count 3

The source preset is left untouched: a copy with the output directory and frame
range overridden is saved under /Game/RayRepair/Cinematics/MRGConfig/Runs/.
Progress and the final status go to <out>/render_run.json.
"""
import argparse
import datetime
import json
import shlex
import sys
from pathlib import Path

import unreal

SETTINGS = json.loads(Path(__file__).with_name('capture_config.json').read_text())
DEFAULT_SEQUENCE = '/Game/RayRepair/Cinematics/LevelSequences/LS_GothicCastle_V02.LS_GothicCastle_V02'
DEFAULT_MAP = '/Game/Ft_Gothic_castle/Map/L_Map.L_Map'

parser = argparse.ArgumentParser()
parser.add_argument('--spp', type=int, required=True)
parser.add_argument('--out', required=True)
parser.add_argument('--sequence', default=DEFAULT_SEQUENCE)
parser.add_argument('--map', default=DEFAULT_MAP)
parser.add_argument('--start', type=int, default=None, help='first frame (default: whole sequence)')
parser.add_argument('--count', type=int, default=None, help='number of frames')
argv = sys.argv[1:]
if len(argv) == 1 and ' ' in argv[0]:
    argv = shlex.split(argv[0])
args = parser.parse_args(argv)

spec = next((g for g in SETTINGS['graphs'] if g['spp'] == args.spp), None)
if spec is None:
    raise RuntimeError('No preset for %d spp in capture_config.json' % args.spp)

subsystem = unreal.get_editor_subsystem(unreal.MoviePipelineQueueSubsystem)
if subsystem.is_rendering():
    raise RuntimeError('A render is already running; leaving it untouched')

out = Path(args.out)
out.mkdir(parents=True, exist_ok=True)
stamp = datetime.datetime.now().astimezone().strftime('%Y%m%d_%H%M%S')
result = {'start': datetime.datetime.now().astimezone().isoformat(), 'args': vars(args),
          'preset': spec['asset'], 'status': 'preparing', 'errors': []}


def write_result():
    (out / 'render_run.json').write_text(json.dumps(result, indent=2), encoding='utf-8')


def override(node, name, value):
    node.set_editor_property(name, value)
    node.set_editor_property('override_' + name, True)


source = unreal.load_asset(spec['asset'])
graph = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(
    '%s_%s' % (source.get_name(), stamp), '/Game/RayRepair/Cinematics/MRGConfig/Runs', source)
if graph is None:
    raise RuntimeError('Could not duplicate preset')
output = graph.get_node_for_branch(unreal.MovieGraphGlobalOutputSettingNode, 'Globals')
override(output, 'output_directory', unreal.DirectoryPath(str(out)))
if args.start is not None:
    start = output.custom_playback_range_start
    start.set_editor_property('type', unreal.MovieGraphSequenceRangeType.CUSTOM)
    start.set_editor_property('value', args.start)
    end = output.custom_playback_range_end
    end.set_editor_property('type', unreal.MovieGraphSequenceRangeType.CUSTOM)
    end.set_editor_property('value', args.start + (args.count or 1))
    override(output, 'custom_playback_range_start', start)
    override(output, 'custom_playback_range_end', end)
if not unreal.EditorAssetLibrary.save_loaded_asset(graph, only_if_is_dirty=False):
    raise RuntimeError('Could not save run preset')
result['run_preset'] = graph.get_path_name()

queue = unreal.MoviePipelineQueue()
job = queue.allocate_new_job(unreal.MoviePipelineExecutorJob)
job.job_name = 'Dataset_%dspp' % args.spp
job.map = unreal.SoftObjectPath(args.map)
job.sequence = unreal.SoftObjectPath(args.sequence)
job.set_graph_preset(graph)


def on_error(executor, pipeline, fatal, error_text):
    result['errors'].append({'fatal': bool(fatal), 'message': str(error_text)})
    write_result()


def on_finished(executor, success):
    result['success'] = bool(success)
    result['status'] = 'finished'
    result['end'] = datetime.datetime.now().astimezone().isoformat()
    write_result()
    unreal.log('RR_DATASET_RENDER_FINISHED %s %s' % (success, out))


executor = unreal.MoviePipelinePIEExecutor()
executor.on_executor_errored_delegate.add_callable_unique(on_error)
executor.on_executor_finished_delegate.add_callable_unique(on_finished)
result['status'] = 'rendering'
write_result()
subsystem.render_queue_instance_with_executor_instance(queue, executor)
