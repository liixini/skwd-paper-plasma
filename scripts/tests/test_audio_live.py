import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('audio_live', Path(__file__).parents[1] / 'test-audio-live.py')
audio_live = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audio_live)


class AudioLiveDirectorySafety(unittest.TestCase):
    def test_existing_paths_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            marker = root / 'preserve.txt'
            marker.write_text('keep')
            previous_mode = root.stat().st_mode
            for results, runtime in [(root, root / 'new'), (root / 'new', root)]:
                with self.assertRaises(ValueError):
                    audio_live.create_directories(results, runtime)
            self.assertEqual(marker.read_text(), 'keep')
            self.assertEqual(root.stat().st_mode, previous_mode)
            self.assertFalse((root / 'new').exists())

    def test_overlapping_paths_are_rejected_before_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination = root / 'new'
            for results, runtime in [(destination, destination),
                                     (destination, destination / 'runtime'),
                                     (destination / 'results', destination)]:
                with self.assertRaises(ValueError):
                    audio_live.create_directories(results, runtime)
            self.assertFalse(destination.exists())
