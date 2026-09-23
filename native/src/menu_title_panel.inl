// Shared title-menu and in-game session presentation. Existing lobby actions and wire messages stay in
// menu_hook.cpp. Lato, spacing and colours follow the game's MenuWindow / NavList
// styles (res/config/style_sheet/{main-menu-windows,window,default}.lua).
// This is still the mod's renderer, not an engine-owned widget tree.
static int g_titleTab = 0; // join / host
static int g_titleServerPage = 0;
static int g_titlePlayerPage = 0;
static int TitleNextFocus(int focus,bool backwards)
{
    const int fields[]={3,2,g_titleTab?4:1};
    for(int i=0;i<3;++i) if(fields[i]==focus) return fields[(i+(backwards?2:1))%3];
    return fields[backwards?2:0];
}

static void titleText(int x, int y, int w, int h, const wchar_t* text,
                      int size = 14, COLORREF color = MW_TEXT, UINT align = DT_LEFT)
{
    HFONT f = mkLato(S((float)size));
    layerText(x,y,w,h,text,f,color,align|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    DeleteObject(f);
}
static void titleRule(int x, int y, int w) { layerRect(x,y,w,S(1),MW_TEXT,35); }
static void titleAction(int x,int y,int w,const wchar_t* label,int id,bool enabled=true,bool primary=false)
{
    // Native menu actions are text-only, including the primary action.
    (void)primary;
    std::wstring caption(label); for(auto& c:caption) c=towupper(c);
    titleText(x,y,w,S(32),caption.c_str(),13,enabled?MW_TEXT:MW_DIM,DT_CENTER);
    if(enabled) addHit(x,y,w,S(32),id,true);
}
static void titleHeading(int w,const wchar_t* label,int closeId)
{
    (void)closeId;
    std::wstring caption(label); for(auto& c:caption) c=towupper(c);
    titleText(S(25),S(10),w-S(50),S(32),caption.c_str(),16);
}
static void titleStatus(int w,int h)
{
    char value[256]="";
    if(g_csInit) { EnterCriticalSection(&g_statusCs); strcpy_s(value,g_transferDetail[0]?g_transferDetail:g_status); LeaveCriticalSection(&g_statusCs); }
    titleText(S(25),h-S(29),w-S(50),S(22),wideOf(value).c_str(),12,MW_DIM);
}
static void titleModPrompt(int w,int h)
{
    char prompt[300]="";
    if(g_csInit) { EnterCriticalSection(&g_statusCs); strcpy_s(prompt,g_modsPrompt); LeaveCriticalSection(&g_statusCs); }
    if(!prompt[0]) return;
    // No hit target from the underlying page survives a download question.
    g_hitCount=0;
    layerRect(0,0,w,h,RGB(0,0,0),180);
    const int x=S(70), dw=w-S(140), y=(h-S(242))/2;
    layerRect(x,y,dw,S(242),RGB(50,70,85),255);
    titleText(x+S(25),y+S(12),dw-S(50),S(32),L"Required mods",18);
    titleRule(x+S(25),y+S(50),dw-S(50));
    mwBody(x+S(25),y+S(66),dw-S(50),S(70),wideOf(prompt).c_str());
    mwCheck(x+S(25),y+S(145),L"Auto-accept mod downloads",g_flagShareMods==1,19);
    titleAction(x+S(25),y+S(195),S(110),L"Cancel",17);
    titleAction(x+dw-S(200),y+S(195),S(175),L"Download mods",16,true,true);
}
static void titleSavePicker(int w,int h)
{
    const int pad=S(25), width=w-2*pad;
    titleHeading(w,L"Choose a savegame",91);
    titleText(pad,S(57),width,S(28),L"Choose the world to share with all players.",14,MW_DIM);
    titleText(pad+S(10),S(88),width-S(195),S(22),L"Savegame",13,MW_DIM);
    titleText(w-pad-S(175),S(88),S(165),S(22),L"Last saved",13,MW_DIM,DT_RIGHT);
    layerRect(pad,S(88),width,S(340),RGB(0,0,0),50);
    for(int row=0;row<SAVE_ROWS;++row) {
        int i=g_savePage*SAVE_ROWS+row;
        if(i >= (int)g_lobbySaves.size()) break;
        const auto& save=g_lobbySaves[i]; int y=S(114.0f+row*39.0f);
        layerRect(pad,y,width,S(36),MW_TEXT,save.path==g_selectedSave?85:0);
        titleText(pad+S(10),y,width-S(195),S(36),save.name.c_str());
        FILETIME local{}; SYSTEMTIME date{}; wchar_t stamp[40]=L"";
        if(FileTimeToLocalFileTime(&save.modified,&local) && FileTimeToSystemTime(&local,&date))
            _snwprintf_s(stamp,_TRUNCATE,L"%04u-%02u-%02u %02u:%02u",date.wYear,date.wMonth,date.wDay,date.wHour,date.wMinute);
        titleText(w-pad-S(175),y,S(165),S(36),stamp,13,MW_DIM,DT_RIGHT);
        addHit(pad,y,width,S(36),100+row,true);
    }
    if(g_lobbySaves.empty()) mwBody(pad,S(145),width,S(80),L"No savegames found. Create and save a world with the Multiplayer mod enabled, then refresh.");
    wchar_t pages[64]; _snwprintf_s(pages,_TRUNCATE,L"Page %d / %d",g_savePage+1,(std::max)(1,((int)g_lobbySaves.size()+SAVE_ROWS-1)/SAVE_ROWS));
    titleText(pad,S(433),width,S(24),pages,13,MW_DIM,DT_RIGHT);

    titleAction(pad,h-S(68),S(90),L"Back",91);
    titleAction(pad+S(100),h-S(68),S(110),L"Refresh",92);
    titleAction(w-pad-S(220),h-S(68),S(105),L"Previous",93,g_savePage>0);
    titleAction(w-pad-S(105),h-S(68),S(105),L"Next",94,(g_savePage+1)*SAVE_ROWS<(int)g_lobbySaves.size());
    titleStatus(w,h);
}
static void titleBrowser(int w,int h)
{
    const int pad=S(25), width=w-2*pad;
    titleText(pad,S(278),width-S(260),S(30),L"Public games",16);
    titleAction(w-pad-S(250),S(278),S(75),L"Refresh",12);
    PubRow rows[8]{}; int count=0; char note[96]="";
    if(g_pubCsInit) { EnterCriticalSection(&g_pubCs); memcpy(rows,g_pub,sizeof(rows)); count=g_pubCount; strcpy_s(note,g_pubNote); LeaveCriticalSection(&g_pubCs); }
    count=(std::min)(8,(std::max)(0,count));
    const int perPage=(std::max)(1,(std::min)(8,(h-S(425))/S(28)));
    g_titleServerPage=(std::min)(g_titleServerPage,(std::max)(0,(count-1)/perPage));
    titleAction(w-pad-S(165),S(278),S(80),L"Previous",112,g_titleServerPage>0);
    titleAction(w-pad-S(80),S(278),S(80),L"Next",113,(g_titleServerPage+1)*perPage<count);
    const int nameW=width-S(380), typeX=pad+nameW, playersX=w-pad-S(245), versionX=w-pad-S(160), ageX=w-pad-S(80);
    titleText(pad+S(10),S(313),nameW-S(20),S(24),L"Game",13,MW_DIM);
    titleText(typeX,S(313),S(135),S(24),L"Type",13,MW_DIM);
    titleText(playersX,S(313),S(80),S(24),L"Players",13,MW_DIM);
    titleText(versionX,S(313),S(80),S(24),L"Version",13,MW_DIM);
    titleText(ageX,S(313),S(80),S(24),L"Seen",13,MW_DIM);
    layerRect(pad,S(310),width,h-S(310)-S(87),RGB(0,0,0),50);
    for(int row=0;row<perPage;++row) {
        const int i=g_titleServerPage*perPage+row;
        if(i>=count) break;
        const PubRow& r=rows[i]; int y=S(341)+row*S(28);
        bool selected=g_joinCode[0] && !strcmp(r.code,g_joinCode);
        layerRect(pad,y,width,S(26),MW_TEXT,selected?85:0);
        std::wstring name=wideOf(r.name); if(r.locked) name+=L"  [password]";
        titleText(pad+S(10),y,nameW-S(20),S(26),name.c_str());
        bool dedicated=!strcmp(r.type,"relay") || !strcmp(r.type,"dedicated") || !strcmp(r.game,"dedicated relay");
        titleText(typeX,y,S(130),S(26),dedicated?L"Dedicated":L"Player hosted",13,MW_DIM);
        wchar_t players[24]; _snwprintf_s(players,_TRUNCATE,L"%d / %d",r.players,r.max);
        titleText(playersX,y,S(75),S(26),players,13);
        titleText(versionX,y,S(75),S(26),wideOf(r.version).c_str(),13,MW_DIM);
        wchar_t age[32];
        if(r.age<60) wcscpy_s(age,L"Just now"); else _snwprintf_s(age,_TRUNCATE,L"%d min ago",r.age/60);
        titleText(ageX,y,S(75),S(26),age,12,MW_DIM);
        addHit(pad,y,width,S(26),40+i,true);
    }
    if(!count) titleText(pad+S(10),S(349),width-S(20),S(28),wideOf(note[0]?note:"Looking for public games...").c_str(),14,MW_DIM);
}
static void titleSetup(int w,int h)
{
    const int pad=S(25),width=w-2*pad,fieldX=pad+S(270),fieldW=width-S(270);
    titleHeading(w,L"MULTIPLAYER",4);
    titleAction(w-pad-S(110),S(10),S(110),L"Open logs",15);
    for(int i=0;i<2;++i) {
        int x=pad+i*S(160);
        titleAction(x,S(51),S(150),i?L"Create game":L"Join game",110+i);
        if(g_titleTab==i) layerRect(x,S(84),S(150),S(2),MW_TEXT,220);
    }
    titleText(pad,S(105),S(255),S(28),L"Player name");
    mwField(fieldX,S(105),fieldW,S(28),g_username,g_joinFocus==3,L"Your Steam name",13);
    titleText(pad,S(141),S(255),S(28),g_titleTab?L"Game name":L"Invitation code");
    mwField(fieldX,S(141),fieldW,S(28),g_titleTab?g_lobbyName:g_joinCode,
        g_joinFocus==(g_titleTab?4:1),g_titleTab?L"Your multiplayer game":L"Paste invitation code",g_titleTab?14:8);
    titleText(pad,S(177),S(255),S(28),L"Password (optional)");
    char masked[40]=""; for(int i=0;i<g_passLen && i<39;++i) masked[i]='*';
    mwField(fieldX,S(177),fieldW,S(28),masked,g_joinFocus==2,L"",10);
    if(g_titleTab) {
        titleText(pad,S(241),width,S(28),L"Lobby settings",18);
        if(g_flagMaster[0]) mwCheck(pad,S(283),L"Show in the public game browser",g_public!=0,11);
        mwCheck(pad,S(325),L"Separate companies",g_sepCompanies!=0,50);
        mwCheck(pad,S(367),L"Cross-play (players without Steam)",g_crossplay!=0,51);
        titleText(pad,S(409),width,S(28),L"Choose a savegame and invite players after creating the lobby.",14,MW_DIM);
    } else {
        if(g_flagMaster[0]) titleBrowser(w,h);
        else titleText(pad,S(278),width,S(50),L"Ask the host for an invitation code.",14,MW_DIM);
        mwCheck(pad,S(221),L"Auto-accept mod downloads",g_flagShareMods==1,19);
    }
    titleAction(pad,h-S(68),S(80),L"Back",4);
    titleAction(w-pad-S(150),h-S(68),S(150),g_titleTab?L"Create game":L"Join game",g_titleTab?2:3,g_titleTab || g_joinCode[0]);
    titleStatus(w,h);
}
// Hosting an already loaded world has no join/save/start flow.
static void sessionSetup(int w,int h)
{
    const int pad=S(25), fieldX=pad+S(270),fieldW=w-pad-fieldX;
    titleHeading(w,L"MULTIPLAYER - HOST SESSION",4); mwClose(w,4);
    titleText(pad,S(63),w-S(50),S(28),L"Invite players to the world you are currently playing.",14,MW_DIM);
    titleText(pad,S(117),S(255),S(28),L"Player name");
    mwField(fieldX,S(117),fieldW,S(28),g_username,g_joinFocus==3,L"Your Steam name",13);
    titleText(pad,S(159),S(255),S(28),L"Game name");
    mwField(fieldX,S(159),fieldW,S(28),g_lobbyName,g_joinFocus==4,L"Your multiplayer game",14);
    titleText(pad,S(201),S(255),S(28),L"Password (optional)");
    char masked[40]="";for(int i=0;i<g_passLen && i<39;++i) masked[i]='*';
    mwField(fieldX,S(201),fieldW,S(28),masked,g_joinFocus==2,L"",10);
    titleText(pad,S(262),w-S(50),S(26),L"Session settings",18);
    mwCheck(pad,S(303),L"Separate companies",g_sepCompanies!=0,50);
    mwCheck(pad+S(300),S(303),L"Cross-play",g_crossplay!=0,51);
    if(g_flagMaster[0]) mwCheck(pad,S(345),L"Show in the public game browser",g_public!=0,11);
    titleText(pad,S(391),w-S(50),S(28),L"New players receive a snapshot of the current world.",14,MW_DIM);
    titleAction(pad,h-S(68),S(90),L"Close",4);
    titleAction(pad+S(120),h-S(68),S(120),L"Open logs",15);
    titleAction(w-pad-S(170),h-S(68),S(170),L"Host session",2);
    titleStatus(w,h);
    titleModPrompt(w,h);
}
static void titleLobby(int w,int h)
{
    const bool world=WorldLoaded();
    if(!world && g_savePicker && g_isHost && !g_sessionStarted) { titleSavePicker(w,h); titleModPrompt(w,h); return; }
    const int pad=S(25), width=w-2*pad, rosterW=S(360), chatX=pad+rosterW+S(25), chatW=w-pad-chatX;
    std::wstring title=L"Multiplayer lobby";
    if(g_modelCsInit) { EnterCriticalSection(&g_modelCs); if(!g_lobbyTitle.empty()) title=wideOf(g_lobbyTitle.c_str()); LeaveCriticalSection(&g_modelCs); }
    titleHeading(w,world?L"MULTIPLAYER - SESSION":L"MULTIPLAYER - LOBBY",4);
    if(world) mwClose(w,4);
    titleText(pad,S(58),width-S(180),S(28),title.c_str(),14,MW_DIM);
    if(g_haveCode) titleAction(w-pad-S(170),S(57),S(170),L"Copy invitation code",7);
    if(!world) {
        const wchar_t* save=g_selectedSave.empty()?L"No savegame selected":wcsrchr(g_selectedSave.c_str(),L'\\');
        if(!g_selectedSave.empty()) save=save?save+1:g_selectedSave.c_str();
        titleText(chatX,S(100),chatW,S(24),L"Savegame",18);
        layerRect(chatX,S(133),chatW,S(30),RGB(0,0,0),50);
        titleText(chatX+S(10),S(133),chatW-S(20),S(30),g_isHost?save:L"Supplied by the host",14);
        if(g_isHost && !g_sessionStarted) titleAction(chatX,S(170),chatW,L"Choose savegame",90,!g_saveStartPending);
    }
    titleText(pad,S(100),rosterW,S(24),L"Players",18);
    titleText(chatX,world?S(100):S(211),chatW,S(24),L"Chat",18);
    const int footer=h-S(113), rowH=S(26), top=S(170);
    layerRect(pad,S(133),rosterW,footer-S(139),RGB(0,0,0),50);
    titleText(pad+S(10),S(136),S(178),S(24),L"Name",13,MW_DIM);
    titleText(pad+S(195),S(136),S(160),S(24),L"Company",13,MW_DIM);
    if(g_modelCsInit) {
        EnterCriticalSection(&g_modelCs);
        int n=playerCount(), capacity=(std::min)(ROSTER_ROWS,(std::max)(0,(footer-top-S(30))/rowH));
        capacity=(std::max)(1,capacity);
        g_titlePlayerPage=(std::min)(g_titlePlayerPage,(std::max)(0,((std::min)(n,ROSTER_ROWS)-1)/capacity));
        for(int row=0;row<capacity;++row) {
            int i=g_titlePlayerPage*capacity+row;
            if(i>=n || i>=ROSTER_ROWS) break;
            int y=top+row*rowH, cid=(std::max)(1,(std::min)(MAX_COMPANIES,g_companies[i]));
            std::wstring name=wideOf(g_players[i].c_str());
            if(g_players[i]==g_host) name+=L" (Host)";
            titleText(pad+S(10),y,S(178),rowH,name.c_str(),13);
            wchar_t company[32]; _snwprintf_s(company,_TRUNCATE,L"Company %d",cid);
            std::wstring stage=i<(int)g_stages.size()?wideOf(g_stages[i].c_str()):L"";
            titleText(pad+S(195),y,S(155),rowH,stage.empty()?company:stage.c_str(),13);
            if(g_players[i]==g_you || g_you==g_host) addHit(pad+S(190),y,S(170),rowH,20+i,true);

        }
        wchar_t count[80]; _snwprintf_s(count,_TRUNCATE,L"%d players",n);
        titleText(pad,footer-S(26),rosterW-S(120),S(22),count,12,MW_DIM);
        if(n>capacity) {
            titleAction(pad+rosterW-S(110),footer-S(35),S(50),L"<",114,g_titlePlayerPage>0);
            titleAction(pad+rosterW-S(55),footer-S(35),S(50),L">",115,(g_titlePlayerPage+1)*capacity<(std::min)(n,ROSTER_ROWS));
        }
        const int chatTop=world?S(133):S(245), logH=footer-chatTop-S(45), lh=S(22), maxLines=(std::max)(0,(logH-S(16))/lh);
        layerRect(chatX,chatTop,chatW,logH,RGB(0,0,0),50);
        for(int i=(std::max)(0,g_chatCount-maxLines),y=chatTop+S(8);i<g_chatCount;++i,y+=lh)
            titleText(chatX+S(10),y,chatW-S(20),lh,wideOf(g_chatLog[(g_chatHead+i)%14]).c_str(),13);
        LeaveCriticalSection(&g_modelCs);
    }
    mwField(chatX,footer-S(37),chatW,S(30),g_chatInput,true,L"Message (Enter to send)",9);

    if(g_isHost) {
        mwCheck(pad,footer+S(6),L"Separate companies",g_sepCompanies!=0,50);
        if(g_flagMaster[0]) mwCheck(pad+S(230),footer+S(6),L"Public game",g_public!=0,11);
        if(g_hostSteam) mwCheck(pad+S(430),footer+S(6),L"Cross-play",g_crossplay!=0,51);
    } else titleText(pad,footer+S(6),width,S(28),world?L"Session running. The host manages session settings.":L"Waiting for the host to start.",14,MW_DIM);
    if(world) {
        if(g_isHost) titleAction(pad,h-S(68),S(110),L"Resync...",87);
        titleAction(w-pad-S(110),h-S(68),S(110),L"Close",4);
    } else titleAction(pad,h-S(68),S(110),L"Leave lobby",5);
    if(!world && g_isHost) titleAction(w-pad-S(155),h-S(68),S(155),g_saveStartPending?L"Sharing savegame...":L"Start game",6,g_lobbyReady && !g_saveStartPending,true);
    titleStatus(w,h);
    titleModPrompt(w,h);
}
static bool RenderTitlePanel(int w,int h,LONG page)
{
    if(page!=1 && page!=2) return false;
    if(page==1) { if(WorldLoaded()) sessionSetup(w,h); else titleSetup(w,h); }
    else titleLobby(w,h);
    return true;
}
