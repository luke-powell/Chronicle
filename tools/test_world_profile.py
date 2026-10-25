import unittest
import tempfile
from pathlib import Path
from contextlib import redirect_stdout
import io
from analyze_world_profile import analyze
from analyze_world_profile import summarize
class ProfileTests(unittest.TestCase):
    def test_nested_threads_and_waits(self):
        def span(name,t,d,tid=1,cat='cpu'):
            return dict(name=name,ph='X',ts=t,dur=d,tid=tid,cat=cat,args={'lod':0})
        data=summarize([span('parent',0,100),span('child',10,40),span('grandchild',20,10),
                        span('sibling',60,20),span('other_thread',0,100,2),span('wait',5,80,1,'wait')])
        rows={r['name']:r for r in data['scopes']}
        self.assertAlmostEqual(rows['parent']['exclusive']['total_ms'],.04)
        self.assertAlmostEqual(rows['child']['exclusive']['total_ms'],.03)
        self.assertAlmostEqual(rows['other_thread']['exclusive']['total_ms'],.1)
        self.assertEqual(rows['wait']['category'],'wait')
    def test_interrupted_capture_exports(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'capture.jsonl'
            path.write_text('{"name":"gpu.draw_frame_us","ph":"C","ts":1,"tid":1,"args":{"value":2000}}\n{"partial":')
            with redirect_stdout(io.StringIO()):result=analyze(path)
            self.assertEqual(result['invalid_lines'],1)
            self.assertEqual(result['gpu_draw']['mean_ms'],2)
            self.assertTrue(path.with_suffix('.trace.json').exists())
if __name__=='__main__':unittest.main()
