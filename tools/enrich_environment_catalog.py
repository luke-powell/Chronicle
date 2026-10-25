"""Add standalone TIM2 images, scene-part provenance and reviewed asset tags.
Run after catalog_environment.py. This never modifies the retail game data.
"""
import csv,hashlib,json,re,struct,sys
from pathlib import Path
from PIL import Image,ImageDraw
sys.path.insert(0,str(Path(__file__).parent/'kazarov/scripts'))
import kazarov_textures as tex
out=Path('port/run/asset-catalog');root=Path('port/run/data')
c=json.loads((out/'catalog.json').read_text())
snow='dun/item/main_data/icebig.img'
if (root/snow).exists() and not any(f['path']==snow for f in c['files']):
    c['files'].append({'path':snow,'bytes':(root/snow).stat().st_size})
seen={(t['source'],t['offset']) for t in c['textures']}
parts={}
for record in c['files']:
    path=root/record['path'];b=path.read_bytes();source=record['path'];ranges=[]
    for match in re.finditer(b'SCN\x00',b):
        base=match.start()
        if base+16>len(b):continue
        n=tex.u32(b,base+4)
        if n>10000 or base+16+n*48>len(b):continue
        for i in range(n):
            p=base+16+i*48;name=b[p:p+16].split(b'\0')[0].decode('ascii','replace');offset,size=struct.unpack_from('<II',b,p+16)
            if offset and size and base+offset+size<=len(b):ranges.append((base+offset,base+offset+size,name))
    parts[source]=ranges
    for match in re.finditer(b'TIM2',b):
        offset=match.start()
        if (source,offset) in seen:continue
        try:
            p=tex.parse_tim2(b[offset:])[0]
            if not p.levels:continue
            w,h,raw,ch=p.levels[0];rgba=tex.colorize(raw,p.palette) if ch==1 else raw
            digest=hashlib.sha256(rgba+struct.pack('<II',w,h)).hexdigest()[:20];image='images/'+digest+'.png'
            if not (out/image).exists():Image.frombytes('RGBA',(w,h),rgba).save(out/image)
            c['textures'].append(dict(source=source,offset=offset,name=path.stem+'@'+str(offset),kind='standalone TIM2',width=w,height=h,image=image,hash=digest,status='decoded',category='unclassified',geometry_usage={}))
            seen.add((source,offset))
        except (ValueError,IndexError,struct.error):pass
for m in c['models']:
    m['part']=next((name for lo,hi,name in parts.get(m['source'],[]) if lo<=m['mds_offset']<hi),'')
    name=m['name'].lower()
    if name.startswith('kusari'):m['category']='chain (not grass)'
    if m['part']=='e01t02':m['category']='dead branches / ground decal'
    if m['part'] in ['e01t01','e02t01']:m['category']='tree / canopy / ground decal'
    if m['part'] in ['e03t01','e03t02','e04t01']:m['category']='palm / ground vegetation'
    if name.startswith('rock') or name.startswith('iwa'):m['category']='rock candidate'
    if name=='hana__c':m['category']='flower arrangement candidate (interior)'
    if 'hanabira' in name:m['category']='petal effect (not rooted flora)'
    if name.startswith('tuta'):m['category']='vine'
    if name.startswith('kusa__') or name.startswith('kkusa__'):m['category']='grass candidate'
curated={
 'e01b06':'Norune natural rock / cliff', 'e02b02':'Matataki mossy grass ground',
 'e02b03':'Matataki patchy grass ground','e02b05':'Matataki grass-to-cliff transition atlas',
 'e02b06':'Matataki rough cliff rock','e02b21':'Matataki pale rock wall',
 'e02b16':'Matataki vines / vegetation atlas','e02b18':'Matataki foliage atlas',
 'e03b03':'Queens pale sand ground','e03b04':'Queens palm foliage / bark atlas',
 'e03b05':'Queens rough stone wall','e03b07':'Queens water / shoreline atlas',
 'e03b09':'Queens grass-to-sand transition','e03b10':'Queens grass decal',
 'e04b01':'Muska Lacka sand ground','e04b02':'Muska Lacka sand variation',
 'e04b04':'Muska Lacka foliage / bark atlas','e04b05':'Muska Lacka sparse dry grass ground',
 'e04b10':'Muska Lacka palm fronds','e04b11':'Muska Lacka radial foliage crown',
 'e04b12':'Muska Lacka pale stone cliff'}
for t in c['textures']:
    if t['name'] in curated:t['category']=curated[t['name']];t['review']='visually reviewed'
    elif re.fullmatch(r'[eds]\d\d[bft]\d+.*',t['name']):t['category']='environment surface / atlas candidate'
c['review_notes']=['Geometry previews are untextured shape previews, not proof of botanical identity.',
 'Surface usage is a broad whole-object association; mixed-material objects need UV review before tiling.',
 'Standalone TIM2 images use file/offset identifiers; some embedded swizzled payloads require container metadata.',
 'Excluded character/monster/menu directories are listed in scope; this is an environment audit, not a claim to identify every file in the game.']
(out/'catalog.json').write_text(json.dumps(c,indent=2),encoding='utf8')
for name,rows in [('textures',c['textures']),('models',c['models'])]:
    keys=sorted(set().union(*(r.keys() for r in rows)))
    with (out/(name+'.csv')).open('w',newline='',encoding='utf8') as f:
        w=csv.DictWriter(f,keys);w.writeheader();w.writerows(rows)
data=json.dumps([dict(r,type='texture') for r in c['textures']]+[dict(r,type='model') for r in c['models']]).replace('</','<\\/')
page=(out/'index.html').read_text(encoding='utf8');start=page.index('const data=')+len('const data=');end=page.index(';let page=0;',start)
page=page[:start]+data+page[end:];(out/'index.html').write_text(page,encoding='utf8')
# Named contact sheets keep natural surfaces easy to review beside the full search.
rows=[];seen=set()
for t in c['textures']:
    if t['name'] in curated and t.get('image') and t['name'] not in seen:rows.append(t);seen.add(t['name'])
canvas=Image.new('RGB',(1000,150*((len(rows)+4)//5)),(230,234,238));draw=ImageDraw.Draw(canvas)
for i,t in enumerate(rows):
    x=i%5*200;y=i//5*150;im=Image.open(out/t['image']).convert('RGBA');im.thumbnail((128,110));canvas.paste(im,(x,y),im);draw.text((x,y+112),t['name'],fill='black');draw.text((x,y+127),t['category'][:28],fill='black')
canvas.save(out/'reviewed-surfaces.png')
print(json.dumps({'textures':len(c['textures']),'unique_images':len({t.get('hash') for t in c['textures']}),'models':len(c['models']),'unique_meshes':len({m['hash'] for m in c['models']}),'reviewed_surface_names':len(curated)}))
