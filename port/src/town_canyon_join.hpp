#pragma once

// Native geometry owns the inner edge. The collar owns the area from that
// exact edge to a terrain-grid-aligned square; no overlapping radial skirts.
namespace canyon_join {
using Vertex=gfx::Vertex3D;
struct Face {std::array<Vertex,3> v;std::string material;};
struct Edge {Vertex a,b;std::array<Vertex,3> source;std::string material;};
inline std::vector<Face> faces;
inline Vertex Mix(const Vertex&a,const Vertex&b,double t){
    Vertex v=a;
    for(int k=0;k<3;k++){v.position[k]=float(std::lerp(double(a.position[k]),double(b.position[k]),t));v.normal[k]=float(std::lerp(double(a.normal[k]),double(b.normal[k]),t));}
    for(int k=0;k<2;k++)v.uv[k]=float(std::lerp(double(a.uv[k]),double(b.uv[k]),t));
    for(int k=0;k<4;k++)v.color[k]=uint8_t(std::lerp(double(a.color[k]),double(b.color[k]),t));
    return v;
}
inline double Cut(const Vertex &v){
    double x=v.position[0],z=v.position[2];
    double throat=std::min({x+1138.,-850.-x,z+260.,-40.-z});
    return std::min(std::hypot(x,z)-(canyon::RidgeRadius(x,z)-8),-throat);
}
inline double Angle(const Vertex &v){double a=std::atan2(v.position[2],v.position[0]);return a<0?a+6.283185307179586:a;}
inline Vertex Normal(Vertex v){
    double x=v.position[0],z=v.position[2],d=canyon::WallDistance(x,z);
    double g=canyon::ProfileGradient(d),nx=(canyon::WallDistance(x-24,z)-canyon::WallDistance(x+24,z))*g,nz=(canyon::WallDistance(x,z-24)-canyon::WallDistance(x,z+24))*g;
    double weight=canyon::Smooth(105,180,canyon::Profile(d));nx+=(canyon::CrownRelief(x-24,z)-canyon::CrownRelief(x+24,z))*weight;nz+=(canyon::CrownRelief(x,z-24)-canyon::CrownRelief(x,z+24))*weight;
    double len=std::sqrt(nx*nx+48*48+nz*nz);v.normal[0]=float(nx/len);v.normal[1]=float(48/len);v.normal[2]=float(nz/len);
    if(d<=65&&x<-1000&&x>-1500&&z>-250&&z<-50){auto n=canyon::FloorNormal(x,z);std::copy_n(n.data(),3,v.normal);}
    return v;
}
inline void Build(const std::vector<Draw3DVisual*> &sources){
    std::vector<Edge> edges;
    const bool build=!canyon::TownJoin().ready;
    for(auto *source:sources){
        if(source->native_terrain)continue;
        Draw3DVisual out;out.vertex_colour=source->vertex_colour;out.native_terrain=true;
        for(auto strip:source->strips){
            auto first=unsigned(out.indices.size());
            for(unsigned i=strip.first_index;i+2<strip.first_index+strip.index_count;i+=3){
                std::array<Vertex,3> tri{source->vertices[source->indices[i]],source->vertices[source->indices[i+1]],source->vertices[source->indices[i+2]]};
                std::vector<Vertex> polygon,crossings;
                for(int k=0;k<3;k++){
                    auto a=tri[k],b=tri[(k+1)%3];double da=Cut(a),db=Cut(b);
                    if(da<=0)polygon.push_back(a);
                    if((da<=0)!=(db<=0)){auto v=Mix(a,b,da/(da-db));polygon.push_back(v);crossings.push_back(v);}
                }
                if(build&&crossings.size()==2&&!strip.ground_transition)
                    edges.push_back({crossings[0],crossings[1],tri,strip.texture_name});
                unsigned start=unsigned(out.vertices.size());out.vertices.insert(out.vertices.end(),polygon.begin(),polygon.end());
                for(unsigned k=1;k+1<polygon.size();k++)out.indices.insert(out.indices.end(),{start,start+k,start+k+1});
            }
            strip.first_index=first;strip.index_count=unsigned(out.indices.size())-first;
            if(strip.index_count)out.strips.push_back(strip);
        }
        gfx::DestroyMesh(source->mesh);out.mesh=gfx::CreateMesh(out.vertices,out.indices);*source=std::move(out);
    }
    if(!build)return;
    if(edges.empty())return;
    // Every cut endpoint must have exactly one partner, including the outlet.
    using Key=std::array<int,3>;std::map<Key,int> degree;
    auto key=[](const Vertex&v){return Key{int(std::round(v.position[0]*100)),int(std::round(v.position[1]*100)),int(std::round(v.position[2]*100))};};
    for(const auto&e:edges){degree[key(e.a)]++;degree[key(e.b)]++;}
    int open=0;for(auto [k,n]:degree)open+=n!=2;
    std::fprintf(stderr,"canyon join: %zu native edges, %d unpaired endpoints\n",edges.size(),open);
    if(open)std::abort();

    // Square perimeter is subdivided at all terrain vertices and wall-profile
    // intersections. Extra native-edge rays are interpolated on those edges.
    std::vector<Vertex> perimeter;
    constexpr double extent=canyon::JoinExtent;
    auto add=[&](double x,double z){Vertex v{};v.position[0]=float(x);v.position[2]=float(z);v.position[1]=float(terrain::LandHeight(x,z));perimeter.push_back(Normal(v));};
    for(int side=0;side<4;side++)for(double q=-extent;q<extent;q+=32){
        double x=side==0?extent:side==1?-q:side==2?-extent:q;
        double z=side==0?q:side==1?extent:side==2?-q:-extent;
        double bx=side==0?x:side==1?x-32:side==2?x:x+32;
        double bz=side==0?z+32:side==1?z:side==2?z-32:z;
        add(x,z);double da=canyon::WallDistance(x,z),db=canyon::WallDistance(bx,bz);
        for(double cut:canyon::ProfileCuts)if((da<cut)!=(db<cut)){
            double t=(cut-da)/(db-da);add(std::lerp(x,bx,t),std::lerp(z,bz,t));
        }
    }
    std::sort(perimeter.begin(),perimeter.end(),[](const Vertex&a,const Vertex&b){return Angle(a)<Angle(b);});
    // Follow mesh connectivity, not polar angle: the retail outlet and a few
    // crown notches fold back in angle. Sorting those vertices discards real
    // native edges and would recreate the very gap this collar must eliminate.
    std::vector<Edge> ordered;std::vector<bool> used(edges.size());
    ordered.push_back(edges[0]);used[0]=true;
    while(ordered.size()<edges.size()){
        auto end=key(ordered.back().b);bool found=false;
        for(size_t i=0;i<edges.size();i++)if(!used[i]){
            Edge e=edges[i];if(key(e.b)==end)std::swap(e.a,e.b);
            if(key(e.a)==end){ordered.push_back(e);used[i]=true;found=true;break;}
        }
        if(!found)std::abort();
    }
    double area=0;for(auto &e:ordered)area+=double(e.a.position[0])*e.b.position[2]-double(e.a.position[2])*e.b.position[0];
    if(area<0){std::reverse(ordered.begin(),ordered.end());for(auto &e:ordered)std::swap(e.a,e.b);}
    auto start=std::min_element(ordered.begin(),ordered.end(),[](const Edge&a,const Edge&b){return Angle(a.a)<Angle(b.a);});std::rotate(ordered.begin(),start,ordered.end());
    std::vector<double> parameter{0};
    for(const auto&e:ordered)parameter.push_back(parameter.back()+std::hypot(e.b.position[0]-e.a.position[0],e.b.position[2]-e.a.position[2]));
    const double tau=6.283185307179586,offset=Angle(ordered.front().a),length=parameter.back();
    for(auto &p:parameter)p*=tau/length;
    std::vector<double> angles(parameter.begin(),parameter.end()-1);
    for(const auto &v:perimeter)angles.push_back(std::fmod(Angle(v)-offset+tau,tau));
    std::sort(angles.begin(),angles.end());angles.erase(std::unique(angles.begin(),angles.end(),[](double a,double b){return std::abs(a-b)<1e-7;}),angles.end());
    auto hit=[](const Vertex&a,const Vertex&b,double angle,double &t,double &r){
        double x=std::cos(angle),z=std::sin(angle),dx=b.position[0]-a.position[0],dz=b.position[2]-a.position[2];
        double den=x*dz-z*dx;if(std::abs(den)<1e-10)return false;
        t=(z*a.position[0]-x*a.position[2])/den;
        r=(a.position[0]*dz-a.position[2]*dx)/den;
        return t>=-1e-5&&t<=1.00001&&r>0;
    };
    struct Ray {Vertex inner,outer;const Edge*edge;};std::vector<Ray> rays;
    for(double a:angles){
        size_t i=std::min(size_t(std::upper_bound(parameter.begin(),parameter.end(),a)-parameter.begin()-1),ordered.size()-1);
        Ray ray{};ray.edge=&ordered[i];ray.inner=Mix(ordered[i].a,ordered[i].b,(a-parameter[i])/(parameter[i+1]-parameter[i]));
        for(size_t k=0;k<perimeter.size();k++){double t,r;if(hit(perimeter[k],perimeter[(k+1)%perimeter.size()],a+offset,t,r)){ray.outer=Mix(perimeter[k],perimeter[(k+1)%perimeter.size()],std::clamp(t,0.,1.));break;}}
        rays.push_back(ray);
    }
    double boundaryError=0;
    for(const auto &e:edges)for(double f:{0.,.25,.5,.75,1.}){
        auto p=Mix(e.a,e.b,f);double nearest=1e30;
        for(size_t i=0;i<rays.size();i++){
            const auto &a=rays[i].inner,&b=rays[(i+1)%rays.size()].inner;
            double dot=0,len=0;for(int k=0;k<3;k++){double d=b.position[k]-a.position[k];dot+=(p.position[k]-a.position[k])*d;len+=d*d;}
            auto q=Mix(a,b,len?std::clamp(dot/len,0.,1.):0);double err=0;for(int k=0;k<3;k++)err+=std::pow(p.position[k]-q.position[k],2);
            nearest=std::min(nearest,std::sqrt(err));
        }boundaryError=std::max(boundaryError,nearest);
    }
    std::fprintf(stderr,"canyon join: native boundary maximum gap %.9f world units\n",boundaryError);
    if(boundaryError>.02)std::abort();
    // Broad bands preserve the native silhouette and roll into the shared
    // terrain. The authored UVs extend tangentially and fade into the same
    // material's world mapping; they never switch abruptly at the native edge.
    constexpr int bands=16;
    auto vertex=[&](const Ray&r,double t,const Edge &material){
        Vertex v=Mix(r.inner,r.outer,t);double x=v.position[0],z=v.position[2];
        if(t>0&&t<1){
            double distance=std::hypot(x-r.inner.position[0],z-r.inner.position[2]);
            double native=r.inner.position[1];
            const auto &pa=r.edge->source[0],&pb=r.edge->source[1],&pc=r.edge->source[2];
            double ax=pb.position[0]-pa.position[0],ay=pb.position[1]-pa.position[1],az=pb.position[2]-pa.position[2];
            double bx=pc.position[0]-pa.position[0],by=pc.position[1]-pa.position[1],bz=pc.position[2]-pa.position[2];
            double nx=ay*bz-az*by,ny=az*bx-ax*bz,nz=ax*by-ay*bx;
            double slope=std::abs(ny)>1e-5?-(nx*(x-r.inner.position[0])+nz*(z-r.inner.position[2]))/ny:0;
            native+=std::clamp(slope,-30.,30.)*std::exp(-distance/60);
            v.position[1]=float(std::lerp(native,terrain::LandHeight(x,z),canyon::Smooth(0,220,distance)));
            auto n=Normal(v);for(int k=0;k<3;k++)v.normal[k]=float(std::lerp(double(r.inner.normal[k]),double(n.normal[k]),canyon::Smooth(0,180,distance)));
        }
        // Extend the source triangle's UV chart in its tangent plane. This is
        // exact at the shared edge even when the native strip resets its UVs.
        const auto &a=material.source[0],&b=material.source[1],&c=material.source[2];
        double ab[3],ac[3],ap[3],aa=0,bb=0,cc=0,pa=0,pc=0;
        for(int k=0;k<3;k++){ab[k]=b.position[k]-a.position[k];ac[k]=c.position[k]-a.position[k];ap[k]=v.position[k]-a.position[k];aa+=ab[k]*ab[k];bb+=ab[k]*ac[k];cc+=ac[k]*ac[k];pa+=ap[k]*ab[k];pc+=ap[k]*ac[k];}
        double den=aa*cc-bb*bb,u=den?(pa*cc-pc*bb)/den:0,w=den?(pc*aa-pa*bb)/den:0;
        for(int k=0;k<2;k++)v.uv[k]=float(a.uv[k]+u*(b.uv[k]-a.uv[k])+w*(c.uv[k]-a.uv[k]));
        // A native UV/material boundary may also duplicate a normal. Use
        // this face's interpolated normal at the shared edge, not the next
        // edge's attributes merely because its position is identical.
        pa=pc=0;for(int k=0;k<3;k++){double d=r.inner.position[k]-a.position[k];pa+=d*ab[k];pc+=d*ac[k];}
        u=den?(pa*cc-pc*bb)/den:0;w=den?(pc*aa-pa*bb)/den:0;
        double distance=std::hypot(x-r.inner.position[0],z-r.inner.position[2]);
        if(t<1){auto n=Normal(v);for(int k=0;k<3;k++)v.normal[k]=float(std::lerp(a.normal[k]+u*(b.normal[k]-a.normal[k])+w*(c.normal[k]-a.normal[k]),double(n.normal[k]),canyon::Smooth(0,180,distance)));}
        v.color[0]=v.color[1]=v.color[2]=128;
        v.color[3]=uint8_t(128*(1-canyon::Smooth(0,180,distance)));
        return v;
    };
    double uvError=0,normalError=0;
    for(size_t i=0;i<rays.size();i++){
        auto &a=rays[i],&b=rays[(i+1)%rays.size()];
        const Edge *material=a.edge;
        for(const Ray *ray:{&a,&b}){
            double dot=0,len=0;for(int k=0;k<3;k++){double d=material->b.position[k]-material->a.position[k];dot+=(ray->inner.position[k]-material->a.position[k])*d;len+=d*d;}
            auto expected=Mix(material->a,material->b,len?std::clamp(dot/len,0.,1.):0),actual=vertex(*ray,0,*material);
            for(int k=0;k<2;k++)uvError=std::max(uvError,double(std::abs(expected.uv[k]-actual.uv[k])));
            for(int k=0;k<3;k++)normalError=std::max(normalError,double(std::abs(expected.normal[k]-actual.normal[k])));
        }
        for(int band=0;band<bands;band++){
            auto p=vertex(a,double(band)/bands,*material),q=vertex(b,double(band)/bands,*material);
            auto r=vertex(a,double(band+1)/bands,*material),s=vertex(b,double(band+1)/bands,*material);
            faces.push_back({{p,q,r},material->material});faces.push_back({{r,q,s},material->material});
        }
    }
    std::fprintf(stderr,"canyon join: shared-edge UV error %.9f, normal error %.9f\n",uvError,normalError);
    if(uvError>.0001||normalError>.0001)std::abort();
    auto &surface=canyon::TownJoin();
    for(const auto &f:faces){
        canyon::JoinTriangle tri;for(int k=0;k<3;k++)std::copy_n(f.v[k].position,3,tri.p[k].begin());
        unsigned index=unsigned(surface.triangles.size());surface.triangles.push_back(tri);
        auto cell=[](double v){return std::clamp(int((v+extent)/128),0,31);};
        for(int z=cell(std::min({tri.p[0][2],tri.p[1][2],tri.p[2][2]}));z<=cell(std::max({tri.p[0][2],tri.p[1][2],tri.p[2][2]}));z++)
        for(int x=cell(std::min({tri.p[0][0],tri.p[1][0],tri.p[2][0]}));x<=cell(std::max({tri.p[0][0],tri.p[1][0],tri.p[2][0]}));x++)surface.bins[z*32+x].push_back(index);
    }
    surface.ready=true;
    std::fprintf(stderr,"canyon join: %zu shared rays, %zu collar triangles, exact native position/UV/normal boundary\n",rays.size(),faces.size());
}
} // namespace canyon_join
