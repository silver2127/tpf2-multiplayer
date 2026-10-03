// Loading policy only; does not inspect or modify engine-owned state.
#pragma once
#include <cstdint>
struct TerrainWarmup {
    uint64_t lastActivity=0, seenBulk=0;
    bool pending=false;
    bool Update(uint64_t now,bool generating,uint64_t bulk,uint64_t uiTick,bool hasUiSignal) {
        if(generating || bulk>seenBulk) {
            lastActivity=now;pending=true;
        }
        if(bulk>seenBulk)seenBulk=bulk;
        if(!pending || generating)return pending;
        const uint64_t elapsed=now-lastActivity;
        // Require a NEW frame after loading activity, never an old world's
        // retained heartbeat. Five seconds absorbs the final allocation burst.
        if(elapsed>=5000 && uiTick>lastActivity && uiTick<=now && now-uiTick<2000)
            pending=false;
        // Old menu DLLs have no heartbeat: keep a bounded three-minute tail.
        // With telemetry, a cancelled/uncompleted load has a 15-minute cap.
        if(elapsed>=(hasUiSignal?900000ull:180000ull))pending=false;
        return pending;
    }
};
