from test_town_combat import run,OUT
import shutil
log=run('norune-house-regression','edit:0',240,'0\n',{'DC_GEORAMA_TEST':'1','DC_TEST_SKIP_TERRAIN_DRAW':'1'},timeout=120)
assert 'georama TEST PASS: original fixed platform and stairs, movable mayor house, door, rotation and save/load' in log
print('house/platform regression PASS',flush=True)
run('norune-style','edit:0',120,'0\n',{'DC_WORLD_PREVIEW_SITE':'-1750,-150,0','DC_WORLD_PREVIEW_GROUND_CAMERA':'-1750,-150,-2400,35,0'},timeout=600)
shutil.copyfile(OUT/'norune-style.png',OUT/'norune-style-canyon.png')
print('canyon view ready',flush=True)
