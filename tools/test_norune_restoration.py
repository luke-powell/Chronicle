from test_town_combat import run, OUT
import shutil
for label,site,camera,skip in [('native','0,0,0','0,-450,0,100,900',True),('town','0,0,0','0,-450,0,100,900',False),('canyon','-2000,-150,0','-2000,-150,-2700,60,900',False),('cave','750,-500,0','750,-500,930,20,-500',False)]:
 env={'DC_WORLD_PREVIEW_SITE':site,'DC_WORLD_PREVIEW_GROUND_CAMERA':camera}
 if skip:env['DC_TEST_SKIP_TERRAIN_DRAW']='1'
 run('norune-style','edit:0',120,'0\n',env,timeout=600)
 shutil.copyfile(OUT/'norune-style.png',OUT/('norune-style-'+label+'.png'))
 print(label+' ready',flush=True)
