"""Shared preview geometry and four-state transport tests, running real Lua 5.2.

No game commands are available in the harness. Local node IDs intentionally
differ between clients; rendering must use only sender-resolved coordinates.
"""
from pathlib import Path
import lupa.lua52 as lupa

ROOT = Path(__file__).resolve().parents[1]
MP = ROOT / 'mod/mp_lockstep_1/res/scripts/mp'
lua = lupa.LuaRuntime(unpack_returned_tuples=True)
lua.globals().SCRIPT_PATH = (MP.parent / '?.lua').as_posix()
lua.execute("package.path = SCRIPT_PATH .. ';' .. package.path")
lua.globals().SOURCE = (MP / 'previews.lua').read_text(encoding='utf-8')
lua.globals().NET = (MP / 'net.lua').read_text(encoding='utf-8')
lua.execute(r'''
local realPrint = print
local count = 0
function check(name, condition)
  assert(condition, name)
  count = count + 1
  realPrint("ok   " .. name)
end
T = 100
os = { time = function() return math.floor(T) end, clock = function() return T end }
FILES, ZONES, SENT = {}, {}, {}
io = { open = function(path, mode)
  if mode == "r" then
    if not FILES[path] then return nil end
    return { read = function(_, n) return FILES[path]:sub(1, n) end, close = function() end }
  end
  local buf = ""
  return { write = function(self, s) buf = buf .. s; return self end,
    close = function() FILES[path] = buf; return true end }
end }
local reads = 0
api = { type = { ComponentType = { BASE_NODE = 1 } }, engine = {
  entityExists = function(id) assert(type(id) == "number" and id >= 0); return id == 123 end,
  getComponent = function(id) assert(id == 123); reads = reads+1; return { position = {100, 200, 30} } end
} }
game = { interface = { setZone = function(id, zone) ZONES[id] = zone end } }
local function side(letter, base)
  local C, K = {queue={}, seqNo=0}, { INSTANCE=letter, BASE=base }
  C.broadcast = function(line) SENT[#SENT+1] = line end
  C.cursorColor = function(o) return o == "a" and 1 or 0, 0, o == "b" and 1 or 0 end
  assert(load(SOURCE, "@previews.lua"))()(C, K, function() end)
  C.previewControlsVisible = function() return true end
  return C, K
end
SG, SK = side("a", "A/")
SS = side("a", "A/")
RG = side("b", "B/")
RS = side("b", "B/")
local function proposal(kind)
  return { proposal = { proposal = {
    addedNodes = {
      {entity=-7, comp={position={0,0,12}}}, {entity=-19, comp={position={100,0,22}}}
    }, addedSegments = {
      {entity=-41, type=kind or 0, comp={node0=-7,node1=-19,tangent0={100,80,0},tangent1={100,-80,0}}}
    }, new2oldSegments = {}
  } } }
end
P = proposal()
local extracted = SG.previewExtract("streetBuilder", P)
check("copy actual proposal curve", extracted.kind == "road" and extracted.curves[1][6] == 80)
local payload = SG.previewEncode(extracted)
check("round-trip bounded geometry", SG.previewEncode(SG.previewDecode(payload)) == payload)
P.proposal.proposal.addedNodes[1].comp.position[1] = 500
check("event tables not retained", extracted.curves[1][1] == 0)
P = proposal()
P.proposal.proposal.addedSegments[1].comp.node1 = 123
check("existing endpoint resolved locally", SG.previewExtract("streetBuilder", P).curves[1][3] == 100 and reads == 1)
P.proposal.proposal.addedSegments[1].comp.node1 = nil
check("nil endpoint never enters engine", SG.previewExtract("streetBuilder", P) == nil and reads == 1)
P.proposal.proposal.addedSegments[1].comp.node1 = 999
check("deleted endpoint never enters component API", SG.previewExtract("streetBuilder", P) == nil and reads == 1)
P = proposal()
P.proposal.proposal.new2oldSegments[-41] = 123
check("companion replacement excluded", SG.previewExtract("streetBuilder", P) == nil)
check("rail capture", SG.previewExtract("trackBuilder", proposal(1)).kind == "rail")
check("unsupported builder ignored", SG.previewExtract("bulldozer", proposal()) == nil)
local polygon = SG.previewPolygon(extracted.curves[1], 3)
local mid = polygon[5]
check("curved preview follows tangents", mid[2] > 15)
check("polygon finite and closed by renderer", #polygon == 18 and polygon[1][2] ~= polygon[#polygon][2])
for _, bad in ipairs({"", "rail ", "road ;", "road 1,,2", "road 1,2,3,4,5,6,7,8;", "road ;1,2,3,4,5,6,7,8", "road 1,2,3,4,5,6,7,8,9", "road nan,0,1,2,3,4,5,6", "road 1000001,0,1,2,3,4,5,6", string.rep("x",5000)}) do
  check("reject malformed payload " .. bad:sub(1,24), SG.previewDecode(bad) == nil)
end
SS.previewTick(); RS.previewTick(); SG.previewGuiTick(); RG.previewGuiTick(); SENT = {}
T = T+0.25
SG.previewGuiEvent("streetBuilder", "builder.proposalCreate", proposal())
SG.previewGuiTick(); SS.previewTick()
check("latest preview sent outside command queue", #SENT == 1 and SENT[1]:find("LSPREVIEW",1,true) == 1 and #SS.queue == 0 and SS.seqNo == 0)
local first = SENT[1]
RS.previewRecv(first); RS.previewTick(); RG.previewGuiTick()
check("peer renders coloured preview", ZONES.mppreview_a_1 and ZONES.mppreview_a_1.drawColor[1] == 1)
local zone = ZONES.mppreview_a_1
T = T+0.25; SG.previewGuiTick(); SS.previewTick(); RS.previewTick(); RG.previewGuiTick()
check("unchanged snapshot does not redraw", ZONES.mppreview_a_1 == zone)
SENT = {}
for i=1,40 do
  T = T+0.25; SG.previewGuiTick(); SS.previewTick()
  for _, line in ipairs(SENT) do RS.previewRecv(line) end
  SENT={}; RS.previewTick(); RG.previewGuiTick()
end
check("stationary preview survives keepalive", ZONES.mppreview_a_1 ~= nil)
SG.previewGuiEvent("streetBuilder", "builder.apply", {})
T=T+0.25; SG.previewGuiTick(); SS.previewTick()
local off = SENT[#SENT]
RS.previewRecv(off); RS.previewTick(); RG.previewGuiTick()
check("confirm removes peer preview", ZONES.mppreview_a_1 == nil)
RS.previewRecv(first)
T=T+0.25; RS.previewTick(); RG.previewGuiTick()
check("old packet cannot resurrect cancelled preview", ZONES.mppreview_a_1 == nil)
SG.previewGuiEvent("trackBuilder", "builder.proposalCreate", proposal(1))
T=T+0.25; SG.previewGuiTick(); SS.previewTick(); RS.previewRecv(SENT[#SENT]); RS.previewTick(); RG.previewGuiTick()
check("next tool draws its own preview", ZONES.mppreview_a_1 ~= nil)
T=T+5; RS.previewTick(); RG.previewGuiTick()
check("disconnected peer expires", ZONES.mppreview_a_1 == nil)
T=T+0.25; SS.previewTick()
check("stale GUI file sends off", SENT[#SENT]:sub(-3) == "off")
FILES['A/tpf2mp_preview_out_a.txt'] = T .. '\n' .. payload -- no end marker
T=T+2; SS.previewTick()
check("partial file cannot publish geometry", SENT[#SENT]:sub(-3) == "off")
T=T+1; SG.previewGuiTick(); SS.previewTick(); RS.previewRecv(SENT[#SENT]); RS.previewTick(); RG.previewGuiTick()
T=T+5; RG.previewGuiTick()
check("stale engine file clears rendering", ZONES.mppreview_a_1 == nil)

-- Dispatch through real net.lua. Any accidental normal command routing fails.
local C, K = {ticks=0}, { INSTANCE='b', BASE='net/', EVENTS_FILE='wire', HIST_RING=16 }
assert(load(NET, '@net.lua'))()(C,K,function() end)
assert(load(SOURCE, '@previews.lua'))()(C,K,function() end)
C.readFrom = function() return first .. '\n', #first+1 end
C.pollEvents()
check("network dispatch routes cosmetic packet", C.previewPeers and C.previewPeers.a ~= nil)

-- Real UI query against the measured shape, including unnamed nested layouts.
local visibleFlag = true
local function layout(items)
  return { getNumItems=function() return #items end, getItem=function(_, i) return items[i+1] end }
end
local function component(name, child, shown)
  return { getName=function() return name end, isVisible=function() return shown ~= false end,
    getLayout=function() return child end }
end
local cancel = component('BuildControlComp::CancelButton')
cancel.isVisible = function() return visibleFlag end
local control = component('BuildControlComp', layout({layout({cancel})}))
local actionLayer = component('RendererComponent::Layer1', layout({control}))
local renderer = component('RendererComponent', layout({actionLayer}))
api.gui = {util={downcast=function(v) return v end, getGameUI=function()
  return {getMainRendererComponent=function() return renderer end}
end}}
check('native action controls detected', C.previewControlsVisible())
visibleFlag=false
check('hidden cancel button detected', not C.previewControlsVisible())
SG.previewControlsVisible=C.previewControlsVisible
SG.previewGuiEvent('streetBuilder','builder.proposalCreate',proposal())
T=T+0.5; SG.previewGuiTick()
check('cancel without builder event clears preview', SG.previewLocal == nil)
visibleFlag=true
SG.previewGuiEvent('trackBuilder','builder.proposalCreate',proposal(1))
T=T+0.5; SG.previewGuiTick()
check('unchanged real control keeps preview', SG.previewLocal ~= nil)
SG.previewGuiEvent('menu.construction','tabWidget.currentChanged',{})
check('tool switch clears preview', SG.previewLocal == nil)
SG.previewGuiEvent('trackBuilder','builder.proposalCreate',proposal(1))
SG.previewGuiEvent('constructionBuilder','builder.proposalCreate',{})
check('unsupported tool cannot retain previous preview', SG.previewLocal == nil)

local rate, before = side('c','rate/'), #SENT
for i=1,100 do T=T+0.01; rate.previewTick() end
check('high simulation speed cannot flood wire', #SENT-before <= 2)
local invalid=P
invalid.proposal.proposal.addedSegments={}
for i=1,25 do invalid.proposal.proposal.addedSegments[i]=proposal().proposal.proposal.addedSegments[1] end
invalid.proposal.proposal.new2oldSegments={}
check('oversized proposal is not partially shown', SG.previewExtract('streetBuilder',invalid)==nil)

-- Restart/first-message ordering and degenerate geometry replacing a valid one.
local early=side('d','early/')
early.previewRecv(first); early.previewTick()
check('first update retains already-received preview', early.previewPeers.a ~= nil)
local good='road 0,0,100,0,100,0,100,0'
local flat='road 0,0,0,0,0,0,0,0'
FILES['B/tpf2mp_preview_in_b.txt']=math.floor(T)..'\na 1.000 0.000 0.000 '..good..'\nend\n'
RG.previewGuiTick()
check('valid curve draws before degenerate replacement', ZONES.mppreview_a_1 ~= nil)
T=T+0.3
FILES['B/tpf2mp_preview_in_b.txt']=math.floor(T)..'\na 1.000 0.000 0.000 '..flat..'\nend\n'
RG.previewGuiTick()
check('degenerate replacement clears old geometry', ZONES.mppreview_a_1 == nil)
-- Native bridge: separate resource IDs, full heights, no command submission.
local C3=side('c','native/')
api.res={}
local function rep(name, id)
  return {getName=function(n) assert(n==id); return name end,
    find=function(s) return s==name and id or -1 end}
end
api.res.streetTypeRep=rep('standard/town_small_new.lua',25)
api.res.trackTypeRep=rep('standard.lua',1)
api.res.bridgeTypeRep=rep('stone.lua',7)
api.res.tunnelTypeRep=rep('rock.lua',8)
local event3=proposal()
local edge3=event3.proposal.proposal.addedSegments[1]
edge3.comp.type=0; edge3.comp.typeIndex=-1
edge3.streetEdge={streetType=25,hasBus=true,tramTrackType=2}
C3.previewGuiEvent('streetBuilder','builder.proposalCreate',event3)
local wire3=C3.previewEncode(C3.previewLocal)
local decoded3=C3.previewDecode(wire3)
check('3D wire includes height and file name', decoded3.details[1][1]==12 and decoded3.details[1].file=='standard/town_small_new.lua')
check('3D wire round trip', C3.previewEncode(decoded3)==wire3)
check('3D capture keeps tram and bus options', decoded3.details[1].bus==1 and decoded3.details[1].tram==2)
check('old 3D status remains unknown',decoded3.invalid==nil)
event3.data={errorState={critical=false,messages={},warnings={'warning only'}}}
C3.previewGuiEvent('streetBuilder','builder.proposalCreate',event3)
local valid4=C3.previewEncode(C3.previewLocal)
check('sender warnings do not create red error tint',valid4:sub(1,4)=='4|0|' and C3.previewDecode(valid4).invalid==false)
event3.data.errorState.messages={'Bridge pillar collision'}
C3.previewGuiEvent('streetBuilder','builder.proposalCreate',event3)
local invalid4=C3.previewEncode(C3.previewLocal)
check('sender errors survive wire round trip',C3.previewDecode(invalid4).invalid==true and C3.previewEncode(C3.previewDecode(invalid4))==invalid4)
check('only status changes wire with identical geometry',valid4:sub(5)==invalid4:sub(5) and valid4~=invalid4)
check('missing sender error state stays unknown',C3.previewExtractInvalid({data={}})==nil)
check('malformed sender error state stays unknown',C3.previewExtractInvalid({data={errorState={messages='bad'}}})==nil)
for _,bad in ipairs({'4|2|'..wire3:sub(3),'4||'..wire3:sub(3),invalid4..'|extra'}) do
  check('reject malformed sender status',C3.previewDecode(bad)==nil)
end
-- Exercise the actual GUI -> engine -> peer engine -> GUI payload path.
local VSG=side('a','statusA/'); local VSS=side('a','statusA/')
local VRG=side('b','statusB/'); local VRS=side('b','statusB/')
VSS.previewTick(); VRS.previewTick()
VSG.previewGuiTick(); VRG.previewGuiTick()
for _,wire in ipairs({valid4,invalid4,valid4}) do
  T=T+0.3; SENT={}
  VSG.previewLocal=VSG.previewDecode(wire); VSG.previewEventAt=T
  VSG.previewGuiTick(); VSS.previewTick()
  check('status-only change is broadcast immediately',#SENT==1)
  VRS.previewRecv(SENT[1]); VRS.previewTick()
  check('peer transport preserves source status',FILES['statusB/tpf2mp_preview_in_b.txt']:find(wire,1,true)~=nil)
end
for _,bad in ipairs({wire3..';',wire3..'|x',wire3:gsub('%.lua','%%ZZ.lua'),wire3:gsub('standard/','../'),wire3:gsub(',0,standard',',3,standard'), '3|'..flat..'|nan,0,0,0,0,a.lua,0,0,0,-'}) do
  check('reject malformed 3D payload',C3.previewDecode(bad)==nil)
end
local bridge3=proposal(1)
local be=bridge3.proposal.proposal.addedSegments[1]
be.comp.type,be.comp.typeIndex=1,7
be.trackEdge={trackType=1,catenary=true}
C3.previewGuiEvent('trackBuilder','builder.proposalCreate',bridge3)
local rail3=C3.previewDecode(C3.previewEncode(C3.previewLocal))
check('rail bridge captures structure and catenary',rail3.details[1].structure=='stone.lua' and rail3.details[1].cat==1)
api.type.SimpleProposal={new=function() return {streetProposal={nodesToAdd={},edgesToAdd={}}} end}
api.type.NodeAndEntity={new=function() return {comp={}} end}
api.type.SegmentAndEntity={new=function() return {comp={}} end}
api.type.Vec3f={new=function(x,y,z) return {x=x,y=y,z=z} end}
api.type.BaseEdgeStreet={new=function() return {} end}
api.type.BaseEdgeTrack={new=function() return {} end}
api.res.streetTypeRep=rep('standard/town_small_new.lua',91)
api.res.trackTypeRep=rep('standard.lua',43)
api.res.bridgeTypeRep=rep('stone.lua',12)
local rebuilt=C3.previewNativeProposal(rail3)
check('receiver resolves its own resource indices',rebuilt.streetProposal.edgesToAdd[1].trackEdge.trackType==43 and rebuilt.streetProposal.edgesToAdd[1].comp.typeIndex==12)
check('receiver keeps full 3D geometry',rebuilt.streetProposal.nodesToAdd[2].comp.position.z==22)
check('receiver creates isolated temporary entities',rebuilt.streetProposal.nodesToAdd[1].entity<0 and rebuilt.streetProposal.edgesToAdd[1].comp.node0<0)
api.res.bridgeTypeRep=rep('different.lua',12)
check('missing bridge resource falls back',C3.previewNativeProposal(rail3)==nil)
api.res.bridgeTypeRep=rep('stone.lua',12)
local degenerate3=C3.previewDecode(wire3)
degenerate3.curves[1]={0,0,0,0,0,0,0,0}; degenerate3.details[1][2]=12
check('degenerate 3D geometry never reaches native evaluator',C3.previewNativeProposal(degenerate3)==nil)
local makes=0
local lastNativeMode
api.cmd={sendCommand=function() error('Preview must never submit a command') end, make={buildProposal=function(sp)
  makes=makes+1
  local request=FILES['native/tpf2mp_preview_native_request.txt']
  local sessionId,nonce=request:match('^(%d+) (%d+)')
  lastNativeMode=request:match('^%d+ %d+ %a+ (%a+)')
  FILES['native/tpf2mp_preview_native_ack.txt']=sessionId..' '..nonce..' ok\nend\n'
  return {proposal=sp}
end}}
check('absent native service uses 2D without constructing commands',not C3.previewNativeUpdate('a',decoded3,wire3) and makes==0)
FILES['native/tpf2mp_preview_native_ready.txt']='12345\nend\n'
check('native draw requires matching acknowledgement',C3.previewNativeUpdate('a',decoded3,wire3) and makes==1)
check('unchanged native preview avoids geometry recomputation',C3.previewNativeUpdate('a',decoded3,wire3) and makes==1)
T=T+1.1
check('native keepalive uses empty conversion',C3.previewNativeUpdate('a',decoded3,wire3) and makes==2)
C3.previewNativeFinish({})
check('native cancellation clears peer state',next(C3.previewNativeDrawn)==nil and makes==3)
for _,wire in ipairs({valid4,invalid4,valid4}) do
  local p=C3.previewDecode(wire)
  check('status-only update reaches native colour mode',C3.previewNativeUpdate('a',p,wire) and lastNativeMode==(p.invalid and 'drawbad' or 'drawok'))
end
C3.previewNativeFinish({})
api.cmd.make.buildProposal=function() makes=makes+1; return {} end
check('stale acknowledgement cannot claim native rendering',not C3.previewNativeUpdate('a',decoded3,wire3))
check('conversion request is always cleared',FILES['native/tpf2mp_preview_native_request.txt']=='\nend\n')
local building={proposal={toAdd={{fileName='station/rail/modular_station/modular_station.con',
 transf={0,1,0,0,-1,0,0,0,0,0,1,0,123,456,20,1},params={seed=17,year=1950,modules={[10]={variant=2,enabled=false}}}}},toRemove={}}}
local bp=SG.previewExtract('constructionBuilder',building)
check('station preview captured with rotation and placement',bp and bp.transf[13]==123 and #bp.curves==4)
local bw=SG.previewEncode(bp)
local bd=RG.previewDecode(bw)
check('station options and booleans round-trip without executable Lua',bd and bd.params.modules[10].enabled==false and RG.previewEncode(bd)==bw)
building.proposal.toAdd[1].params.modules[10].variant=99
building.proposal.toAdd[1].transf[13]=888
check('construction capture owns a deep copy',bp.params.modules[10].variant==2 and bp.transf[13]==123)
building.proposal.toRemove={123}
check('existing station replacement excluded',SG.previewExtract('constructionBuilder',building)==nil)
for _,bad in ipairs({'5|0|../bad.con|0|m0:',bw..'garbage',bw:gsub('123','nan',1),bw:gsub('m3:','m999:',1),
 '5|0|test.con|0,1,0,0,-1,0,0,0,0,0,1,0,123,456,20,1|m1:s2:61s2:zz'}) do
 check('reject malformed construction payload',RG.previewDecode(bad)==nil)
end
local cyclic={}; cyclic.self=cyclic; bp.params=cyclic
check('cyclic parameters are bounded',SG.previewEncode(bp)==nil)
bp.params={huge=string.rep('x',513)}
check('oversized parameters are bounded',SG.previewEncode(bp)==nil)
api.res.constructionRep={find=function(f) return f==bd.file and 1 or -1 end,getName=function() return bd.file end}
api.type.SimpleProposal.new=function() return {streetProposal={nodesToAdd={},edgesToAdd={}},constructionsToAdd={}} end
api.type.SimpleProposal.ConstructionEntity={new=function() return {} end}
api.type.Vec4f={new=function(...) return {...} end}
api.type.Mat4f={new=function(...) return {...} end}
api.engine.util={getPlayer=function() return 55 end}
local nativeBuilding=RG.previewNativeProposal(bd)
check('receiver creates isolated construction with local player',nativeBuilding.constructionsToAdd[1].playerEntity==55 and #nativeBuilding.streetProposal.edgesToAdd==0)
check('receiver retains station model and options',nativeBuilding.constructionsToAdd[1].fileName==bd.file and nativeBuilding.constructionsToAdd[1].params.seed==17)
api.res.constructionRep.find=function() return -1 end
check('missing building resource falls back without native proposal',RG.previewNativeProposal(bd)==nil)
SG.previewGuiEvent('constructionBuilder','builder.proposalCreate',{proposal={toAdd={{fileName=bd.file,transf=bd.transf,params=bd.params}},toRemove={}},data={errorState={messages={'collision'}}}})
check('station sender error tint captured',SG.previewLocal and SG.previewLocal.invalid==true)
SG.previewGuiEvent('constructionBuilder','builder.apply',{})
check('station confirmation clears preview',SG.previewLocal==nil)
local BG,BS,PG,PS=side('e','E/'),side('e','E/'),side('f','F/'),side('f','F/')
BG.previewGuiTick(); PG.previewGuiTick(); BS.previewTick(); PS.previewTick()
T=T+0.25; SENT={}
local depot={proposal={toAdd={{fileName='depot/road/road_depot.con',transf=bd.transf,params=bd.params}},toRemove={}}}
BG.previewGuiEvent('constructionBuilder','builder.proposalCreate',depot)
BG.previewGuiTick(); BS.previewTick()
check('building preview uses cosmetic transport only',#SENT==1 and #BS.queue==0 and BS.seqNo==0)
PS.previewRecv(SENT[1]); PS.previewTick(); PG.previewGuiTick()
check('building without native service draws oriented placement marker',ZONES.mppreview_e_1~=nil and ZONES.mppreview_e_4~=nil)
for i=1,20 do
 T=T+0.25; SENT={}; BG.previewGuiTick(); BS.previewTick()
 for _,line in ipairs(SENT) do PS.previewRecv(line) end
 PS.previewTick(); PG.previewGuiTick()
end
check('held building preview survives heartbeat',ZONES.mppreview_e_1~=nil)
BG.previewControlsVisible=function() return false end
T=T+0.5; SENT={}; BG.previewGuiTick(); BS.previewTick()
for _,line in ipairs(SENT) do PS.previewRecv(line) end
PS.previewTick(); PG.previewGuiTick()
check('building cancellation without apply clears peer marker',ZONES.mppreview_e_1==nil and ZONES.mppreview_e_4==nil)
realPrint(string.format('%d preview checks passed', count))
''')
