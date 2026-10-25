"""Walk through the authored exit into procedural terrain using actual character collision."""
import re
from test_town_combat import run
log=run('norune-exit-walk','edit:0',520,'0\n40 128 0 128 128\n460\n',{'DC_TEST_SKIP_TERRAIN_DRAW':'1','DC_WORLD_PREVIEW_SITE':'-1000,-150,1.5707963','DC_WORLD_PREVIEW_GROUND_CAMERA':'-700,-150,-2000,16,-150','DC_TOWN_WALK_TRACE':'1'},timeout=120)
points=[tuple(map(float,p)) for p in re.findall(r'town walk: ([-\d.]+) ([-\d.]+) ([-\d.]+)',log)]
assert points and points[-1][0]<-1600,points
# Allow 10 cm for the rendered triangle interpolation at the join.
assert all(abs(y)<1 for x,y,z in points),points
print('PASS real walking collision: native exit to procedural canyon',points[-1])
