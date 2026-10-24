"""Read-only retail environment audit. Generated previews stay in ignored port/run.

Indexes every IMG and MDS signature in environment containers, retaining offsets,
object/material provenance and geometry-based floor/wall hints. Names are evidence,
not authoritative botanical identification. Re-run after changing the asset set.
"""
import argparse, csv, hashlib, html, json, math, re, struct, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, str(Path(__file__).parent/'kazarov/scripts'))
import kazarov_textures as tex

def text(b): return b.split(b'\0')[0].decode('ascii', 'replace')
def u32(b,p): return struct.unpack_from('<I',b,p)[0]
def category(name):
    for label,pattern in [('flora','tree|leaf|leav|flower|bush|grass|kusa|hana|ha__|palm|fern|sabo'),('rock','rock|stone|iwa|seki'),('wood','log|stump|trunk|kabu|kare'),('ground','ground|jimen|yuka|floor|sand'),('wall','wall|kabe|cliff')]:
        if re.search(pattern,name,re.I): return label+' (name hint)'
    return 'unclassified'

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--data',default='port/run/data');ap.add_argument('--out',default='port/run/asset-catalog');a=ap.parse_args()
    root=Path(a.data);out=Path(a.out);out.mkdir(parents=True,exist_ok=True);(out/'images').mkdir(exist_ok=True)
    textures=[];models=[];errors=[];files=[];pixels={};seenimg=set();seenmodel=set()
    # All environment-bearing roots, including interior, dungeon and unused town scenes.
    roots=['gedit','rmdat','noda_w','img','img_1','img_2','img_3','img_4','img_5','img_6','opdat','dun']
    excluded={'monstor','mainchara','message','menu','item','sound','cloth','chara'}
    for folder in roots:
        for path in sorted((root/folder).rglob('*')):
            if not path.is_file() or any(p in excluded for p in path.relative_to(root).parts):continue
            if path.suffix.lower() in {'.mes','.cfg','.txt','.stb','.snd','.wav'}:continue
            b=path.read_bytes();rel=path.relative_to(root).as_posix();files.append({'path':rel,'bytes':len(b)})
            for match in re.finditer(b'(?:IMG|IM2|IM3)\x00',b):
                base=match.start()
                try:kind,entries=tex.parse_img(b[base:])
                except (ValueError,struct.error):continue
                for e in entries:
                    if e['name'].startswith('#'):continue
                    key=(rel,base+e['offset'])
                    if key in seenimg:continue
                    seenimg.add(key)
                    row={'source':rel,'archive_offset':base,'offset':key[1],'name':e['name'],'kind':kind}
                    try:
                        p=tex.parse_tim2(b[key[1]:],e['swizzled'],e['no_image'])[0]
                        if not p.levels:continue
                        w,h,raw,ch=p.levels[0];rgba=tex.colorize(raw,p.palette) if ch==1 else raw
                        digest=hashlib.sha256(rgba+struct.pack('<II',w,h)).hexdigest()[:20]
                        image='images/'+digest+'.png'
                        if digest not in pixels:
                            im=Image.frombytes('RGBA',(w,h),rgba);im.save(out/image);pixels[digest]=1
                        row.update(width=w,height=h,image=image,hash=digest,status='decoded')
                    except (ValueError,IndexError,struct.error) as ex:
                        row.update(status='decode failed',error=str(ex));errors.append(row.copy())
                    textures.append(row)
            for match in re.finditer(b'MDS\x00',b):
                base=match.start();d=b[base:]
                if len(d)<16:continue
                count=u32(d,8)
                if not 0<count<20000 or 16+112*count>len(d):continue
                matrices=[]
                for i in range(count):
                    pos=16+i*112;name=text(d[pos+8:pos+40]);off,parent=struct.unpack_from('<ii',d,pos+40)
                    matrix=np.array(struct.unpack_from('<16f',d,pos+48)).reshape(4,4).T
                    if 0<=parent<i:matrix=matrices[parent]@matrix
                    matrices.append(matrix)
                    if off<=0 or off+60>len(d):continue
                    h=struct.unpack_from('<15i',d,off);size,n,vo=h[2:5];mesh,info=h[10],h[14]
                    if size<60 or off+size>len(d) or not 0<n<1000000 or vo<60 or vo+n*16>size or mesh<0 or mesh+16>size:continue
                    ident=(rel,base,i)
                    if ident in seenmodel:continue
                    seenmodel.add(ident)
                    v=np.frombuffer(d,dtype='<f4',count=n*4,offset=off+vo).reshape(n,4).copy();v[:,3]=1;v=(matrix@v.T).T[:,:3]
                    if not np.isfinite(v).all():continue
                    mats=set();triangle_ids=[];horizontal=vertical=0;cursor=off+mesh+16
                    for s in range(min(u32(d,off+mesh+8),100000)):
                        if cursor+12>off+size:break
                        prim,num,mat=struct.unpack_from('<IIi',d,cursor);cursor+=12;stride=16 if h[8]>0 else 12
                        if cursor+num*stride>off+size:break
                        if mat>=0 and info>=0 and info+(mat+1)*96<=size:mats.add(text(d[off+info+mat*96+52:off+info+(mat+1)*96]))
                        ids=[u32(d,cursor+k*stride) for k in range(num)];cursor+=num*stride
                        if any(k>=n for k in ids):continue
                        for k in range(0,num-2,1 if prim==4 else 3):
                            triangle_ids.append(ids[k:k+3])
                    if not triangle_ids:continue
                    all_tris=v[np.array(triangle_ids)]
                    normals=np.cross(all_tris[:,1]-all_tris[:,0],all_tris[:,2]-all_tris[:,0]);areas=np.linalg.norm(normals,axis=1)
                    ny=np.abs(normals[:,1])/np.maximum(areas,1e-8)
                    horizontal=float(areas[ny>.85].sum());vertical=float(areas[ny<.3].sum());tris=all_tris[:8000]
                    digest=hashlib.sha256(d[off:off+size]).hexdigest()[:20];image='images/model-'+digest+'.png'
                    if not (out/image).exists():
                        # Orthographic shape preview; no invented material identification.
                        triangles=np.array(tris);basis=np.array([[.707,0,-.707],[-.35,.87,-.35],[.61,.50,.61]])
                        q=triangles@basis.T;lo=q[:,:,:2].min(axis=(0,1));hi=q[:,:,:2].max(axis=(0,1));scale=145/max(hi-lo).clip(.001)
                        im=Image.new('RGB',(176,176),(27,32,39));draw=ImageDraw.Draw(im)
                        for j in np.argsort(q[:,:,2].mean(axis=1)):
                            p=(q[j,:,:2]-(lo+hi)/2)*scale+[88,88];p[:,1]=176-p[:,1]
                            normal=np.cross(triangles[j,1]-triangles[j,0],triangles[j,2]-triangles[j,0]);light=.35+.65*abs(normal@np.array([.3,.85,.43]))/max(1e-5,np.linalg.norm(normal));c=int(180*light)
                            draw.polygon([tuple(t) for t in p],fill=(c,int(c*.95),int(c*.8)))
                        im.save(out/image)
                    models.append({'source':rel,'mds_offset':base,'object_index':i,'name':name,'category':category(name),'vertices':n,'triangles':len(tris),'textures':sorted(mats),'bounds_min':v.min(axis=0).tolist(),'bounds_max':v.max(axis=0).tolist(),'horizontal_area':round(horizontal,1),'vertical_area':round(vertical,1),'image':image,'hash':digest})
            print(rel,len(textures),len(models),flush=True)
    usage={}
    for m in models:
        for name in m['textures']:
            u=usage.setdefault(name,{'objects':0,'horizontal_area':0,'vertical_area':0});u['objects']+=1
            for k in ['horizontal_area','vertical_area']:u[k]+=m[k]
    for t in textures:
        t['geometry_usage']=usage.get(t['name'],{})
        t['category']='surface candidate' if t['geometry_usage'] else 'unclassified'
    manifest={'scope':roots,'excluded_subdirectories':sorted(excluded),'files':files,'textures':textures,'models':models,'errors':errors}
    (out/'catalog.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
    for name,rows in [('textures',textures),('models',models)]:
        keys=sorted(set().union(*(r.keys() for r in rows)))
        with (out/(name+'.csv')).open('w',newline='',encoding='utf8') as f:
            writer=csv.DictWriter(f,keys);writer.writeheader();writer.writerows(rows)
    # Paginate in the browser: thousands of images remain lazy-loaded.
    data=json.dumps([dict(r,type='texture') for r in textures]+[dict(r,type='model') for r in models]).replace('</','<\\/')
    page='''<!doctype html><meta charset="utf-8"><title>Dark Cloud environment asset catalogue</title>
<style>body{background:#171d24;color:#eee;font:15px system-ui;margin:30px}input,select,button{padding:10px;margin:5px}#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:12px}article{background:#26303b;padding:12px;overflow-wrap:anywhere}img{width:176px;height:176px;object-fit:contain;image-rendering:pixelated;background:repeating-conic-gradient(#777 0% 25%,#aaa 0% 50%) 0/16px 16px}small{color:#bdcbd8}h1{margin-bottom:8px}</style>
<h1>Dark Cloud environment asset catalogue</h1><p>Retail assets from your local game. Search names, source paths and material references. Model previews show geometry; name-based flora/rock labels are provisional. Surface candidates include architecture as well as natural ground and walls. Identical image hashes are deduplicated on disk; every source occurrence remains indexed.</p>
<input id="query" placeholder="Search: e02, kusa, rock, snow..." size="40"><select id="type"><option value="">All</option><option>texture</option><option>model</option></select><button id="prev">Previous</button><button id="next">Next</button><p id="count"></p><div id="grid"></div>
<script>const data=DATA;let page=0;const esc=s=>String(s??'').replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('"','&quot;');function show(){let q=document.querySelector('#query').value.toLowerCase(),type=document.querySelector('#type').value;let rows=data.filter(r=>(!type||r.type===type)&&JSON.stringify(r).toLowerCase().includes(q));page=Math.min(page,Math.max(0,Math.ceil(rows.length/80)-1));document.querySelector('#count').textContent=rows.length+' matches; page '+(page+1)+' / '+Math.max(1,Math.ceil(rows.length/80));document.querySelector('#grid').innerHTML=rows.slice(page*80,page*80+80).map(r=>'<article>'+(r.image?'<a href="'+esc(r.image)+'"><img loading="lazy" src="'+esc(r.image)+'"></a>':'Decode unavailable')+'<b>'+esc(r.name)+'</b><p>'+esc(r.category)+'</p><small>'+esc(r.source)+'<br>'+esc(r.type==='model'?'MDS '+r.mds_offset+' object '+r.object_index+' | '+r.triangles+' triangles | '+r.textures.join(', '):r.width+' x '+r.height+' | offset '+r.offset)+'</small></article>').join('')}query.oninput=type.onchange=()=>{page=0;show()};prev.onclick=()=>{page=Math.max(0,page-1);show()};next.onclick=()=>{page++;show()};show();</script>'''.replace('DATA',data)
    (out/'index.html').write_text(page,encoding='utf8')
    print(json.dumps({'files':len(files),'textures':len(textures),'unique_texture_images':len(pixels),'models':len(models),'errors':len(errors)}))
if __name__=='__main__':main()
