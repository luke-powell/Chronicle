#pragma once
#include <algorithm>
#include <cmath>
namespace flightcamera {
constexpr double Pi=3.141592653589793;
constexpr double CruisePitch=Pi*10/180;
struct Orbit {
    double yaw=0,pitch=CruisePitch,quiet=0,vertical=0;
    void Reset(double heading){yaw=heading;pitch=CruisePitch;quiet=0;vertical=0;}
    void Step(double dt,double heading,double speed,double verticalSpeed,double inputYaw,double inputPitch){
        bool manual=std::abs(inputYaw)>1e-6 || std::abs(inputPitch)>1e-6;
        quiet=manual?0:quiet+dt;
        yaw=std::remainder(yaw+inputYaw,2*Pi);
        pitch=std::clamp(pitch+inputPitch,-Pi*89/180,Pi*89/180);
        vertical+=(verticalSpeed-vertical)*(1-std::exp(-dt/1.2));
        // Keep the horizon level; ignore banking and short altitude corrections.
        // Manual orbit owns both axes until the grace period expires. Hovering
        // preserves the player's chosen view instead of pulling it home.
        if(manual || quiet<.85 || speed<.5)return;
        double ramp=std::clamp((quiet-.85)/.8,0.,1.);
        double yawError=std::remainder(heading-yaw,2*Pi);
        yaw=std::remainder(yaw+std::clamp(yawError*(1-std::exp(-dt/.65))*ramp,-1.4*dt,1.4*dt),2*Pi);
        // Altitude changes should suggest the flight path without pulling the
        // view down into the ground. Manual orbit retains its full range.
        double target=CruisePitch-std::clamp(.15*std::atan2(vertical,std::max(12.,speed)),-Pi/36,Pi/36);
        double delta=(target-pitch)*(1-std::exp(-dt/1.6))*ramp;
        pitch+=std::clamp(delta,-.45*dt,.45*dt);
    }
};
}
