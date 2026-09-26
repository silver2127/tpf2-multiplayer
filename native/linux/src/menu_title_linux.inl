// Native title and in-game session presentation; actions remain in panel_linux.cpp.
static int g_titleTab=0, g_serverPage=0, g_serverPerPage=4, g_playerPage=0;
static bool MenuPanelMode() { return g_uiState>=1 && g_uiState<=3; }
static bool TitleMode() { return !P().view.inGame && (g_uiState>=1 && g_uiState<=3); }
static int TitleNextFocus(int focus,bool backwards) {
    const int fields[]={3,2,g_titleTab?4:1};
    for(int i=0;i<3;++i)if(fields[i]==focus)return fields[(i+(backwards?2:1))%3];
    return fields[backwards?2:0];
}
static void TitleText(int x,int y,int w,int h,const std::string& s,int size=14,Rgb color=MW_TEXT,unsigned align=layer::kLeft) {
    layer::Text(x,y,w,h,s.c_str(),S(size),color,align|layer::kVCenter|layer::kEndEllipsis);
}
static void TitleAction(int x,int y,int w,const char* s,int id,bool enabled=true) {
    TitleText(x,y,w,S(32),s,13,enabled?MW_TEXT:MW_DIM,layer::kCenter);
    if(enabled)AddHit(x,y,w,S(32),id,true);
}
static void RenderTitleLocked(int w,int h) {
    const int pad=S(25),width=w-2*pad;
    const auto& v=P().view;
    const bool world=v.inGame;
    const bool recovery=g_uiState==3 || (g_uiState==2 && v.recoveryPresent);
    MwTitle(world?(g_uiState==1?"MULTIPLAYER - HOST SESSION":"MULTIPLAYER - SESSION"):g_uiState==1?"MULTIPLAYER":P().savePicker?"CHOOSE A SAVEGAME":"MULTIPLAYER - LOBBY");
    if(world && !recovery) MwClose(w,4);
    else if(world && recovery && !v.worldIo) MwClose(w,87); // 83 remains Back to lobby
    if(g_uiState==1 && world) {
        const int fx=pad+S(270),fw=width-S(270);
        TitleText(pad,S(63),width,S(28),"Invite players to the world you are currently playing.",14,MW_DIM);
        TitleText(pad,S(117),S(255),S(28),"Player name");
        MwField(fx,S(117),fw,S(28),P().username,g_focus==3,"Your Steam name",13);
        TitleText(pad,S(159),S(255),S(28),"Game name");
        MwField(fx,S(159),fw,S(28),P().lobbyName,g_focus==4,"Your multiplayer game",14);
        TitleText(pad,S(201),S(255),S(28),"Password (optional)");
        MwField(fx,S(201),fw,S(28),std::string(P().passCode.size(),'*'),g_focus==2,"",10);
        TitleText(pad,S(262),width,S(26),"Session settings",18);
        MwCheck(pad,S(303),"Separate companies",g_separateCompanies,50);
        MwCheck(pad+S(300),S(303),"Cross-play",g_crossplay,51);
        if(!P().flagMaster.empty())MwCheck(pad,S(345),"Show in the public game browser",g_public,11);
        TitleText(pad,S(391),width,S(28),"New players receive a snapshot of the current world.",14,MW_DIM);
        TitleAction(pad,h-S(68),S(90),"CLOSE",4);
        TitleAction(pad+S(120),h-S(68),S(120),"OPEN LOGS",15);
        TitleAction(w-pad-S(170),h-S(68),S(170),"HOST SESSION",2);
    } else if(g_uiState==1) {
        TitleAction(w-pad-S(110),S(10),S(110),"OPEN LOGS",15);
        for(int i=0;i<2;++i) {
            TitleAction(pad+i*S(160),S(51),S(150),i?"CREATE GAME":"JOIN GAME",110+i);
            if(g_titleTab==i)layer::Rect(pad+i*S(160),S(84),S(150),S(2),MW_TEXT,220);
        }
        const int fx=pad+S(270),fw=width-S(270);
        TitleText(pad,S(105),S(255),S(28),"Player name");
        MwField(fx,S(105),fw,S(28),P().username,g_focus==3,"Your Steam name",13);
        TitleText(pad,S(141),S(255),S(28),g_titleTab?"Game name":"Invitation code");
        MwField(fx,S(141),fw,S(28),g_titleTab?P().lobbyName:P().joinCode,g_focus==(g_titleTab?4:1),g_titleTab?"Your multiplayer game":"Paste invitation code",g_titleTab?14:8);
        TitleText(pad,S(177),S(255),S(28),"Password (optional)");
        MwField(fx,S(177),fw,S(28),std::string(P().passCode.size(),'*'),g_focus==2,"",10);
        if(g_titleTab) {
            TitleText(pad,S(241),width,S(28),"Lobby settings",18);
            if(!P().flagMaster.empty())MwCheck(pad,S(283),"Show in the public game browser",g_public,11);
            MwCheck(pad,S(325),"Separate companies",g_separateCompanies,50);
            MwCheck(pad,S(367),"Cross-play (players without Steam)",g_crossplay,51);
            TitleText(pad,S(409),width,S(28),"Choose a savegame and invite players after creating the lobby.",13,MW_DIM);
        } else {
            MwCheck(pad,S(221),"Auto-accept mod downloads",g_flagShareMods==1,19);
            const int count=int(P().pubRows.size());
            TitleText(pad,S(278),width-S(250),S(30),count ? "Public games ("+std::to_string(count)+")" : "Public games",16);
            TitleAction(w-pad-S(250),S(278),S(80),"REFRESH",12);
            const int per=std::max(1,std::min(8,(h-S(425))/S(28)));
            g_serverPerPage=per;
            g_serverPage=std::min(g_serverPage,std::max(0,(count-1)/per));
            TitleAction(w-pad-S(170),S(278),S(85),"PREVIOUS",112,g_serverPage>0);
            TitleAction(w-pad-S(85),S(278),S(85),"NEXT",113,(g_serverPage+1)*per<count);
            layer::Rect(pad,S(310),width,S(35)+per*S(28),rgb(0,0,0),50);
            TitleText(pad+S(10),S(313),S(330),S(24),"Game",13,MW_DIM);
            TitleText(pad+S(350),S(313),S(130),S(24),"Type",13,MW_DIM);
            TitleText(pad+S(485),S(313),S(75),S(24),"Players",13,MW_DIM);
            TitleText(pad+S(570),S(313),S(75),S(24),"Version",13,MW_DIM);
            TitleText(pad+S(650),S(313),S(75),S(24),"Seen",13,MW_DIM);
            for(int row=0;row<per;++row) {
                const int i=g_serverPage*per+row;if(i>=count)break;
                const auto& r=P().pubRows[i];const int y=S(341)+row*S(28);
                if(P().joinCode==r.code)layer::Rect(pad,y,width,S(26),MW_TEXT,85);
                TitleText(pad+S(10),y,S(320),S(26),r.name+(r.locked?" [password]":""),13);
                TitleText(pad+S(350),y,S(130),S(26),r.type=="relay"||r.type=="dedicated"?"Dedicated":"Player hosted",13,MW_DIM);
                TitleText(pad+S(485),y,S(75),S(26),std::to_string(r.players)+" / "+std::to_string(r.max),13);
                TitleText(pad+S(570),y,S(75),S(26),r.version,13,MW_DIM);
                TitleText(pad+S(650),y,S(75),S(26),r.age<60?"Just now":std::to_string(r.age/60)+" min ago",12,MW_DIM);
                AddHit(pad,y,width,S(26),40+row,true);
            }
            if(!count)TitleText(pad+S(10),S(349),width-S(20),S(28),P().pubNote.empty()?"No public games":P().pubNote,14,MW_DIM);
        }
        TitleAction(pad,h-S(68),S(80),"BACK",4);
        TitleAction(w-pad-S(150),h-S(68),S(150),g_titleTab?"CREATE GAME":"JOIN GAME",g_titleTab?2:3,g_titleTab || !P().joinCode.empty());
    } else if(!recovery && !world && P().savePicker && v.isHost && !v.lobbyDone) {
        TitleText(pad,S(57),width,S(28),"Choose the world to share with all players.",14,MW_DIM);
        layer::Rect(pad,S(88),width,S(340),rgb(0,0,0),50);
        TitleText(pad+S(10),S(88),width-S(195),S(22),"Savegame",13,MW_DIM);
        TitleText(w-pad-S(175),S(88),S(165),S(22),"Last saved",13,MW_DIM,layer::kRight);
        const int count=int(v.saves.size());P().savePage=std::min(P().savePage,std::max(0,(count-1)/8));
        for(int row=0;row<8;++row) {
            const int i=P().savePage*8+row;if(i>=count)break;
            const auto& save=v.saves[i];const int y=S(114+row*39);
            if(save.path==v.selectedSave)layer::Rect(pad,y,width,S(36),MW_TEXT,85);
            TitleText(pad+S(10),y,width-S(195),S(36),save.name);
            const time_t when=save.modified;tm local{};char stamp[40]="";
            if(localtime_r(&when,&local))strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M",&local);
            TitleText(w-pad-S(175),y,S(165),S(36),stamp,13,MW_DIM,layer::kRight);
            AddHit(pad,y,width,S(36),100+row,true);
        }
        if(!count)MwBody(pad,S(145),width,S(80),"No savegames found. Create and save a world with the Multiplayer mod enabled, then refresh.");
        TitleAction(pad,h-S(68),S(90),"BACK",91);
        TitleAction(pad+S(100),h-S(68),S(110),"REFRESH",92);
        TitleAction(w-pad-S(220),h-S(68),S(105),"PREVIOUS",93,P().savePage>0);
        TitleAction(w-pad-S(105),h-S(68),S(105),"NEXT",94,(P().savePage+1)*8<count);
    } else {
        TitleText(pad,S(58),width-S(180),S(28),v.title,14,MW_DIM);
        if(v.haveCode && !recovery)TitleAction(w-pad-S(180),S(57),S(180),"COPY INVITATION CODE",7);
        const int rw=S(360),cx=pad+rw+S(25),cw=w-pad-cx,footer=h-S(113);
        TitleText(pad,S(100),rw,S(24),"Players",18);
        if(recovery) {
            TitleText(cx,S(100),cw,S(24),"Resync",18);
            std::string label="World sync: "+v.recoveryPhase;
            const auto& phase=v.recoveryPhase;
            if(phase.empty() || phase=="manual")label="Reload the host's world";
            if(phase=="readiness")label=std::to_string(v.readyCount)+" / "+std::to_string(v.readyTotal)+" players ready";
            if(v.recoveryRequested)label="Waiting for the lobby...";
            TitleText(cx,S(133),cw,S(30),label);
            if(!v.recoveryRequested) {
                if(phase=="readiness" && !v.readyMine)TitleAction(cx,S(170),cw,"READY",82);
                else if(v.isHost && phase=="error")TitleAction(cx,S(170),cw,"TRY AGAIN",81);
                else if(v.isHost && (phase.empty() || phase=="manual" || phase=="detected" || phase=="waiting" || phase=="aborted" || phase=="complete"))TitleAction(cx,S(170),cw,"REQUEST SYNC",80);
            }
            if(v.isHost && phase=="detected" && !v.recoveryRequested)TitleAction(pad,h-S(68),S(150),"KEEP PLAYING",85);
            const std::string detail=phase=="transferring" && !v.transferHint.empty()?v.transferHint:v.recoveryDetail;
            MwBody(pad,h-S(108),width,S(36),detail.empty()?"Everyone reloads the host's world. Client-only changes will be lost.":detail.c_str());
            if(phase.empty() || phase=="manual" || phase=="detected" || phase=="unavailable" || phase=="complete")TitleAction(w-pad-S(150),h-S(68),S(150),"BACK TO LOBBY",83);
        } else if(!world) {
            TitleText(cx,S(100),cw,S(24),"Savegame",18);
            layer::Rect(cx,S(133),cw,S(30),rgb(0,0,0),50);
            const auto slash=v.selectedSave.find_last_of('/');
            TitleText(cx+S(10),S(133),cw-S(20),S(30),!v.isHost?"Supplied by the host":v.selectedSave.empty()?"No savegame selected":v.selectedSave.substr(slash==std::string::npos?0:slash+1));
            if(v.isHost && !v.lobbyDone)TitleAction(cx,S(170),cw,"CHOOSE SAVEGAME",90,!v.startPending);
        }
        TitleText(cx,world && !recovery?S(100):S(211),cw,S(24),"Chat",18);
        layer::Rect(pad,S(133),rw,footer-S(139),rgb(0,0,0),50);
        TitleText(pad+S(10),S(136),S(178),S(24),"Name",13,MW_DIM);
        TitleText(pad+S(195),S(136),S(160),S(24),"Company",13,MW_DIM);
        const int count=std::min(ROSTER_ROWS,int(v.players.size())),per=8;
        g_playerPage=std::min(g_playerPage,std::max(0,(count-1)/per));
        for(int row=0;row<per;++row) {
            const int i=g_playerPage*per+row;if(i>=count)break;
            const auto& p=v.players[i];const int y=S(170)+row*S(26);
            TitleText(pad+S(10),y,S(178),S(26),p.name+(p.host?" (Host)":""),13);
            TitleText(pad+S(195),y,S(155),S(26),p.stage.empty()?"Company "+std::to_string(p.company):p.stage,13);
            if(!recovery && (p.you || v.youAreHost))AddHit(pad+S(190),y,S(170),S(26),20+i,true);
        }
        TitleText(pad,footer-S(26),rw-S(120),S(22),std::to_string(v.players.size())+" players",12,MW_DIM);
        TitleAction(pad+rw-S(110),footer-S(35),S(50),"<",114,g_playerPage>0);
        TitleAction(pad+rw-S(55),footer-S(35),S(50),">",115,(g_playerPage+1)*per<count);
        const int chatTop=world && !recovery?S(133):S(245),logH=footer-chatTop-S(45),lh=S(22),lines=std::max(0,(logH-S(16))/lh);
        layer::Rect(cx,chatTop,cw,logH,rgb(0,0,0),50);
        for(int i=std::max(0,int(v.chat.size())-lines),y=chatTop+S(8);i<int(v.chat.size());++i,y+=lh)
            TitleText(cx+S(10),y,cw-S(20),lh,v.chat[i],13);
        MwField(cx,footer-S(37),cw,S(30),P().chatInput,v.active,"Message (Enter to send)",9);
        if(!recovery) {
        if(v.isHost) {
            MwCheck(pad,footer+S(6),"Separate companies",v.separateCompanies,50);
            if(!P().flagMaster.empty())MwCheck(pad+S(230),footer+S(6),"Public game",g_public,11);
            if(v.hostSteam)MwCheck(pad+S(430),footer+S(6),"Cross-play",v.crossplay,51);
        } else TitleText(pad,footer+S(6),width,S(28),world?"Session running. The host manages session settings.":"Waiting for the host to start.",14,MW_DIM);
        if(world) {
            if(v.isHost)TitleAction(pad,h-S(68),S(110),"RESYNC...",84);
            TitleAction(w-pad-S(110),h-S(68),S(110),"CLOSE",4);
        } else TitleAction(pad,h-S(68),S(110),"LEAVE LOBBY",5);
        if(!world && v.isHost)TitleAction(w-pad-S(155),h-S(68),S(155),v.startPending?"SHARING SAVEGAME...":"START GAME",6,v.lobbyReady && !v.startPending);
        }
    }
    if(!v.transferDetail.empty())TitleText(pad,h-S(29),width,S(22),v.transferDetail,12,MW_DIM);
    else MwStatus(w,h);
    if(!v.modsPrompt.empty()) {
        g_hitCount=0;layer::Rect(0,0,w,h,rgb(0,0,0),180);
        const int x=S(70),dw=w-S(140),y=(h-S(242))/2;
        layer::Rect(x,y,dw,S(242),rgb(50,70,85),255);
        TitleText(x+pad,y+S(12),dw-2*pad,S(32),"Required mods",18);
        MwBody(x+pad,y+S(66),dw-2*pad,S(70),v.modsPrompt.c_str());
        MwCheck(x+pad,y+S(145),"Auto-accept mod downloads",g_flagShareMods==1,19);
        TitleAction(x+pad,y+S(195),S(110),"CANCEL",17);
        TitleAction(x+dw-S(200),y+S(195),S(175),"DOWNLOAD MODS",16);
    }
}
