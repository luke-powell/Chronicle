#include "flight_camera.hpp"
#include <cassert>
#include <cstdio>
int main(){using namespace flightcamera;Orbit c;c.Reset(0);
 c.Step(1./60,0,28,0,1.,-.5);double yaw=c.yaw,pitch=c.pitch;
 for(int i=0;i<40;++i)c.Step(1./60,0,28,0,0,0);
 assert(c.yaw==yaw&&c.pitch==pitch);
 for(int i=0;i<600;++i)c.Step(1./60,0,28,0,0,0);
 assert(std::abs(c.yaw)<.001&&std::abs(c.pitch-CruisePitch)<.01);
 c.Step(1./60,0,0,0,1.,.3);yaw=c.yaw;pitch=c.pitch;
 for(int i=0;i<300;++i)c.Step(1./60,2,0,12,0,0);
 assert(c.yaw==yaw&&c.pitch==pitch);
 c.Reset(Pi-.01);c.quiet=2;c.Step(1./60,-Pi+.01,28,0,0,0);
 assert(std::abs(std::remainder(c.yaw-(Pi-.01),2*Pi))<.01);
 for(double dt:{1./30,1./60,1./120}){c.Reset(0);for(int i=0;i<int(12/dt);++i)c.Step(dt,1,28,12,0,0);assert(std::abs(c.yaw-1)<.001);assert(c.pitch<.35&&c.pitch>-.12);}
 c.Reset(0);for(int i=0;i<200;++i)c.Step(1./60,0,28,0,0,.1);assert(c.pitch<Pi/2&&c.pitch>1.55);
 for(double climb:{-1000.,1000.}){c.Reset(0);for(int i=0;i<1200;++i)c.Step(1./60,0,28,climb,0,0);assert(c.pitch>=Pi/36-.001&&c.pitch<=Pi/12+.001);}
 std::puts("PASS orbit grace, moving recenter, hover hold, yaw wrap, vertical response and timestep stability");}
