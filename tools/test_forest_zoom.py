"""Rendered negative control: distant trees must remain meshes after zooming."""
import re
from PIL import Image, ImageChops, ImageStat
from test_town_combat import run, OUT

for name, extra in [('lod-final-430',{}),('lod-negative-430',{'DC_DISABLE_FOREST_MESHES':'1'})]:
    log=run(name,'edit:0',510,'0\n220 select\n221\n250 l2\n430\n',
            {'DC_TERRAIN_TEST_SPAWN':'16050,0,0','DC_FOREST_TRACE':'1',**extra})
    rows=re.findall(r'terrain forest ready: (-?\d+) (-?\d+) level (\d+) trees (\d+) ids (\d+) mask (\d+)',log)
    assert {4,5}.issubset({int(r[2]) for r in rows})
    assert all(int(r[5])==(7 if int(r[2])<=2 else 4) for r in rows)
a=Image.open(OUT/'lod-final-430.png').convert('RGB').crop((0,150,1280,580))
b=Image.open(OUT/'lod-negative-430.png').convert('RGB').crop((0,150,1280,580))
difference=ImageStat.Stat(ImageChops.difference(a,b)).mean
assert sum(difference)/3>5,difference
print('PASS visible far tree geometry versus mesh-disabled negative control:',difference)
