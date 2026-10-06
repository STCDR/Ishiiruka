// Headless regression tests for the assembled game hooks, using Dolphin's real
// PowerPC interpreter. Game services (heap, EXI, sound) are explicit test stubs.
#pragma once
#include "Common/MsgHandler.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <vector>
#include <functional>
#include <cstring>
#include <cmath>

namespace LocalTeamsAsmTest
{
struct Elf
{
  static constexpr u32 Base = 0x80800000;
  std::vector<u8> file, text;
  std::map<std::string, u32> symbols;
  u32 Read(size_t p, unsigned n = 4) const
  {
    if (p + n > file.size()) throw std::runtime_error("truncated test ELF");
    u32 value = 0;
    for (unsigned i = 0; i < n; ++i) value = (value << 8) | file[p + i];
    return value;
  }
  explicit Elf(const std::string& path)
  {
    std::ifstream input(path, std::ios::binary);
    file.assign(std::istreambuf_iterator<char>(input), {});
    if (file.size() < 52 || Read(0) != 0x7F454C46 || file[4] != 1 || file[5] != 2)
      throw std::runtime_error("assemble PowerPC test objects first: " + path);
    const u32 table = Read(32), stride = Read(46, 2), count = Read(48, 2);
    u32 textIndex = 0;
    for (u32 i = 0; i < count; ++i)
    {
      const u32 section = table + i * stride;
      if (Read(section + 4) == 1 && (Read(section + 8) & 4))
      {
        const u32 offset = Read(section + 16), size = Read(section + 20);
        if (offset + size > file.size()) throw std::runtime_error("bad ELF text size");
        text.assign(file.begin() + offset, file.begin() + offset + size); textIndex = i;
      }
    }
    for (u32 i = 0; i < count; ++i)
    {
      const u32 section = table + i * stride;
      if (Read(section + 4) != 2) continue;
      const u32 strings = table + Read(section + 24) * stride;
      const u32 stringOffset = Read(strings + 16);
      const u32 offset = Read(section + 16), size = Read(section + 20);
      for (u32 p = offset; p < offset + size; p += 16)
      {
        const u32 index = Read(p + 14, 2), name = stringOffset + Read(p);
        if (index != textIndex && index != 0xFFF1) continue;
        size_t end = name;
        while (end < file.size() && file[end]) ++end;
        if (end == file.size()) throw std::runtime_error("bad ELF symbol name");
        symbols[std::string(file.begin() + name, file.begin() + end)] = Read(p + 4) + (index == textIndex ? Base : 0);
      }
    }
  }
  u32 Symbol(const std::string& name) const
  {
    const auto it = symbols.find(name);
    if (it == symbols.end()) throw std::runtime_error("missing ELF symbol: " + name);
    return it->second;
  }
};

struct Runner
{
  static constexpr u32 Stack = 0x81700000, Css = 0x81000000, Msrb = 0x81001000;
  static constexpr u32 Buffer = 0x81002000, Minor = 0x81003000, SceneData = 0x81004000;
  static constexpr u32 Return = 0x80F00000, R13 = 0x804D6D5C;
  Elf& elf;
  Interpreter interpreter;
  std::vector<std::vector<u8>> packets;
  std::vector<u8> pollResponse;
  std::map<u32, std::function<void()>> services;
  std::map<u32, u32> callbackTables;
  std::vector<std::pair<u32, u32>> extraCodeRanges;
  unsigned allocations = 0;
  explicit Runner(Elf& elf_) : elf(elf_)
  {
    Memory::Clear();
    Memory::CopyToEmu(Elf::Base, elf.text.data(), elf.text.size());
    auto& cpu = PowerPC::ppcState;
    std::fill(std::begin(cpu.gpr), std::end(cpu.gpr), 0);
    std::fill(std::begin(cpu.spr), std::end(cpu.spr), 0);
    cpu.Exceptions = 0; cpu.msr = 0x2030; cpu.iCache.Init();
    cpu.gpr[1] = Stack; cpu.gpr[13] = R13; cpu.spr[SPR_LR] = Return;
    SetCR(0x20000000); // replaced CSS beq takes the Start-check path
    interpreter.Init();
    Memory::Write_U32(0x08000000, 0x80479D30); // online CSS major/minor
    Memory::Write_U32(10, 0x80479D60);
    Memory::Write_U32(Css, elf.Symbol("CSSDT_BUF_ADDR"));
    Memory::Write_U32(Msrb, Css + elf.Symbol("CSSDT_MSRB_ADDR"));
    Memory::Write_U8(3, R13 + elf.Symbol("OFST_R13_ONLINE_MODE"));
    Memory::Write_U8(1, R13 - 0x49A9); // visible character selected
    Memory::Write_U32(SceneData, R13 - 0x49F0);
    Memory::Write_U8(24, SceneData + 0x70);
    Memory::Write_U32(SceneData, Minor + 0x14);
    Memory::Write_U32(SceneData, 0x803DAD40);
    Memory::Write_U32(Buffer, SceneData + 0x88);
    Memory::Write_U32(0x48000000, elf.Symbol("INJ_FREEZE_STADIUM"));
    Memory::Write_U8(1, Css + elf.Symbol("CSSDT_TEAM_IDX"));
  }
  void Status(unsigned count, unsigned active, unsigned phase = 0, bool loopback = false)
  {
    const u8 status[9] = {1, u8(count), u8(active), u8(active), 0, u8(phase), 1, 0, u8(loopback)};
    Memory::CopyToEmu(Css + elf.Symbol("CSSDT_LOCAL_TEAMS_STATUS"), status, sizeof(status));
    for(unsigned i=0;i<4;++i) Memory::Write_U8(u8(i),Css+elf.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+12+i);
    Memory::Write_U8(u8((1u<<count)-1),Css+elf.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+32);
    PowerPC::ppcState.gpr[7] = 0x1000;
  }
  u32 Run(u32 entry, const std::vector<u32>& stops)
  {
    auto& cpu = PowerPC::ppcState;
    cpu.pc = entry;
    for (unsigned steps = 0; steps < 20000; ++steps)
    {
      if (std::find(stops.begin(), stops.end(), cpu.pc) != stops.end()) return cpu.pc;
      bool stub = true;
      const auto table = callbackTables.find(cpu.pc);
      if (table != callbackTables.end())
      {
        cpu.pc = cpu.spr[SPR_LR]; cpu.spr[SPR_LR] = table->second; continue;
      }
      const auto service = services.find(cpu.pc);
      if (service != services.end()) service->second();
      else if (cpu.pc == elf.Symbol("HSD_MemAlloc")) { cpu.gpr[3] = Buffer; ++allocations; }
      else if (cpu.pc == elf.Symbol("FN_EXITransferBuffer"))
      {
        if (cpu.gpr[5] == elf.Symbol("CONST_ExiRead") && !pollResponse.empty())
        {
          Require(cpu.gpr[4] == pollResponse.size(), "native CSS requested the wrong status length");
          // Retail EXIDma at 0x80345F10 masks the address to 0x03FFFFE0.
          // Direct-pointer stubs hid the unaligned stack-buffer count bug.
          Memory::CopyToEmu(cpu.gpr[3]&0x03FFFFE0, pollResponse.data(), pollResponse.size());
        }
        else
        {
          std::vector<u8> packet(cpu.gpr[4]);
          Require(packet.size() <= 100, "unexpected EXI buffer size in menu test");
          Memory::CopyFromEmu(packet.data(), cpu.gpr[3]&0x03FFFFE0, packet.size()); packets.push_back(packet);
        }
      }
      else if (cpu.pc == elf.Symbol("Zero_AreaLength"))
      {
        Require(cpu.gpr[4] <= 1024, "invalid test clear length");
        const std::vector<u8> zero(cpu.gpr[4], 0);
        Memory::CopyToEmu(cpu.gpr[3], zero.data(), zero.size());
      }
      else if (cpu.pc == elf.Symbol("memcpy"))
      {
        Require(cpu.gpr[5] <= 1024, "invalid test copy length");
        std::vector<u8> bytes(cpu.gpr[5]);
        Memory::CopyFromEmu(bytes.data(), cpu.gpr[4], bytes.size());
        Memory::CopyToEmu(cpu.gpr[3], bytes.data(), bytes.size());
      }
      else if (cpu.pc == elf.Symbol("Inputs_GetPlayerHeldInputs")) { cpu.gpr[4]=0; }
      else if (cpu.pc == elf.Symbol("JObj_SetFlagsAll"))
        Memory::Write_U32(Memory::Read_U32(cpu.gpr[3]+0x14)|cpu.gpr[4],cpu.gpr[3]+0x14);
      else if (cpu.pc == elf.Symbol("JObj_ClearFlagsAll"))
        Memory::Write_U32(Memory::Read_U32(cpu.gpr[3]+0x14)&~cpu.gpr[4],cpu.gpr[3]+0x14);
      else if (cpu.pc != elf.Symbol("HSD_Free") && cpu.pc != elf.Symbol("SFX_Menu_CommonSound") && cpu.pc != 0x801BAAD0)
        stub = false;
      if (stub) { cpu.pc = cpu.spr[SPR_LR]; continue; }
      bool inCode = cpu.pc >= Elf::Base && cpu.pc < Elf::Base + elf.text.size();
      for (const auto& range : extraCodeRanges) inCode |= cpu.pc >= range.first && cpu.pc < range.second;
      Require(inCode, "game hook branched to an unexpected callback");
      Require(Memory::Read_U32(cpu.pc) != 0, "game hook reached a zero instruction");
      interpreter.SingleStepInner();
      Require(cpu.Exceptions == 0, "PowerPC exception while executing game hook");
    }
    throw std::runtime_error("game hook exceeded instruction limit");
  }
};

void Run(const std::string& directory, std::ofstream& report)
{
  RegisterMsgAlertHandler([](const char*, const char* message, bool, int) -> bool {
    throw std::runtime_error(std::string("PowerPC fixture alert: ") + message);
  });
  Elf css(directory + "css.elf"), scene(directory + "scene.elf"), input(directory + "input.elf");
  SConfig::GetInstance().bWii = false;
  Memory::Init();
  PPCTables::InitTables(PowerPC::CORE_INTERPRETER);
  try
  {
    const std::vector<u32> exits{0x80263334, 0x80263264};
    for (unsigned count : {2u, 3u, 4u})
    {
      for (unsigned active = 0; active + 1 < count; ++active)
      {
        Runner first(css); first.Status(count, active);
        first.Run(Elf::Base, exits);
        Require(Memory::Read_U8(Runner::R13 + css.Symbol("OFST_R13_ISWINNER")) == 0xFF,
                "local Start left first-match winner state uninitialized (routes to SSS)");
        Require(Memory::Read_U8(Runner::R13 - 0x49AA) == 0, "early local Start opened another screen");
        Require(first.packets.size() == 1 && first.packets[0][0] == 0xC6, "early Start must only save a local pick");
      }
      Runner last(css); last.Status(count, count - 1);
      last.Run(Elf::Base, exits);
      Require(Memory::Read_U8(Runner::R13 - 0x49AA) == 4, "last Start did not open native code entry");
      const auto callback = Memory::Read_U32(Runner::R13 + css.Symbol("OFST_R13_CALLBACK"));
      Require(callback == css.Symbol("FN_TX_FIND_MATCH"), "code entry did not install native matchmaking callback");
      Require(last.packets.size() == 1 && last.packets[0][0] == 0xC6, "search began before room code confirmation");
      // Native keyboard letters are two-byte Shift-JIS (full-width "00011").
      const u8 code[18] = {0x82, 0x4F, 0x82, 0x4F, 0x82, 0x4F, 0x82, 0x50, 0x82, 0x50};
      for (unsigned i = 0; i < 9; ++i)
        Memory::Write_U16((u16(code[2 * i]) << 8) | code[2 * i + 1], 0x804A0740 + 3 * i);
      PowerPC::ppcState.spr[SPR_LR] = Runner::Return;
      last.Run(callback, {Runner::Return});
      Require(last.packets.size() == 2 && last.packets[1][0] == css.Symbol("CONST_SlippiCmdFindOpponent"), "native callback did not send find-opponent command");
      Require(last.packets[1][1] == 3 && std::equal(std::begin(code), std::end(code), last.packets[1].begin() + 2), "entered room code or teams mode changed in EXI payload");
    }
    report << "PASS: assembled two/three/four-local Start handlers initialize scene state, wait for native code entry and transmit its exact teams room\n";
    {
      Runner retry(css); retry.Status(2, 1, 5); retry.Run(Elf::Base, exits);
      Require(Memory::Read_U8(Runner::R13 - 0x49AA) == 4 && retry.packets.empty(), "Start after cancelled code entry lost choices or searched early");
      Runner fixture(css); fixture.Status(4, 3, 0, true); fixture.Run(Elf::Base, exits);
      Require(Memory::Read_U8(Runner::R13 - 0x49AA) == 0, "localhost fixture opened code entry");
      for (unsigned localReady : {0u, 1u})
        for (unsigned remoteReady : {0u, 1u})
        {
          Runner waiting(css); waiting.Status(2, 1, 2);
          Memory::Write_U8(u8(localReady), Runner::Msrb + 1);
          Memory::Write_U8(u8(remoteReady), Runner::Msrb + 2);
          Require(waiting.Run(Elf::Base, exits) == (localReady && remoteReady ? 0x80263264u : 0x80263334u), "CSS launch bypassed aggregate readiness");
        }
    }
    report << "PASS: assembled code-entry retry, localhost fixture and aggregate readiness gate\n";
    // Continue past the injected branch into retail Melee's second ready check.
    // Two locals on one team fail that offline check even when all four Slippi
    // clients are ready. Stopping at 0x80263264 missed the repeated error sound.
    const u32 retailLaunch[]={0x880DB657,0x28000000,0x418200C0,0x38600001,
                             0x380000FF,0x986DB656,0x28050001,0x980DB652};
    for(bool native : {false,true})
      for(unsigned offlineReady : {0u,1u})
        for(unsigned localReady : {0u,1u})
          for(unsigned remoteReady : {0u,1u})
          {
            Runner handoff(css); handoff.Status(2,0,2);
            const auto status=Runner::Css+css.Symbol("CSSDT_LOCAL_TEAMS_STATUS");
            Memory::Write_U8(u8(native),status+9);
            Memory::Write_U8(localReady?3:0,status+4);
            Memory::Write_U8(u8(offlineReady),Runner::R13-0x49A9);
            Memory::Write_U8(u8(localReady),Runner::Msrb+1);
            Memory::Write_U8(u8(remoteReady),Runner::Msrb+2);
            PowerPC::ppcState.gpr[7]=0; // No fresh Start is needed after connecting.
            for(unsigned i=0;i<sizeof(retailLaunch)/4;++i) Memory::Write_U32(retailLaunch[i],0x80263264+i*4);
            Memory::Write_U32(0x38600003,0x8026332C); // Retail error sound ID 3.
            Memory::Write_U32(0x4BDC0D01,0x80263330); // bl SFX_Menu_CommonSound.
            handoff.extraCodeRanges={{0x80263264,0x80263284},{0x8026332C,0x80263334}};
            unsigned errorSounds=0;
            handoff.services[css.Symbol("Inputs_GetPlayerHeldInputs")]=[]{ PowerPC::ppcState.gpr[4]=0; };
            handoff.services[css.Symbol("SFX_Menu_CommonSound")]=[&]{
              if(PowerPC::ppcState.gpr[3]==3)++errorSounds;
            };
            const auto reached=handoff.Run(Elf::Base,{0x80263264,0x80263270,0x80263334});
            if(localReady && remoteReady) handoff.Run(reached,{0x80263284,0x80263334});
            const bool launch=localReady && remoteReady && (native || offlineReady);
            Require(Memory::Read_U8(Runner::R13-0x49AA)==(launch?1:0),
                    "native same-team handoff replayed offline error sound instead of requesting scene change");
            Require(errorSounds==unsigned(localReady && remoteReady && !launch),
                    "native handoff made an offline error sound or stock rejection behavior changed");
            Require(Memory::Read_U8(Runner::R13-0x49A9)==offlineReady,
                    "Slippi handoff changed the offline VS ready state");
          }
    report << "PASS: retail CSS launch requests scene change for same-team locals without error sounds; incomplete Slippi readiness remains gated\n";
    for (unsigned mode : {2u, 3u})
      for (u8 winner : {u8(0xFF), u8(0), u8(1)})
      {
        Runner decide(scene);
        decide.services[0x801BAAD0]=[]{
          Require(PowerPC::ppcState.gpr[3]==Runner::Minor,"retail CSS exit received status buffer instead of scene data");
        };
        PowerPC::ppcState.gpr[3] = Runner::Minor;
        Memory::Write_U8(u8(mode), Runner::R13 + scene.Symbol("OFST_R13_ONLINE_MODE"));
        Memory::Write_U8(winner, Runner::R13 + scene.Symbol("OFST_R13_ISWINNER"));
        const u32 reached = decide.Run(scene.Symbol("CSSSceneDecide"), {scene.Symbol("SplashSceneInit"), Runner::Return});
        Require((winner != 0 && reached == scene.Symbol("SplashSceneInit")) ||
                (winner == 0 && reached == Runner::Return && Memory::Read_U8(0x80479D35) == 2), "native first-match/rematch routing changed");
      }
    report << "PASS: native scene code routes first match directly to splash; stock loser stage selection remains intact\n";
    {
      Runner reload(scene);
      PowerPC::ppcState.gpr[3]=Runner::Minor;
      Memory::Write_U8(1,Runner::Css+scene.Symbol("CSSDT_NATIVE_RELOAD"));
      Memory::Write_U8(5,0x80479D35);
      bool stockDecide=false;
      reload.services[0x801BAAD0]=[&]{ stockDecide=true; };
      Require(reload.Run(scene.Symbol("CSSSceneDecide"),{scene.Symbol("SplashSceneInit"),Runner::Return})==Runner::Return &&
              Memory::Read_U8(0x80479D35)==1 && !stockDecide &&
              !Memory::Read_U8(Runner::Css+scene.Symbol("CSSDT_NATIVE_RELOAD")),
              "count change launched a match or skipped rebuilding CSS");
      // Retail routing interprets zero as automatic advance, and positive
      // requests as minor+1. Continue into those actual instructions.
      const u32 routing[]={0x881F0003,0x981F0004,0x887F0005,0x28030000,0x41820018,
                           0x3803FFFF,0x981F0003,0x38000000,0x981F0005,0x48000068};
      for(unsigned i=0;i<sizeof(routing)/4;++i) Memory::Write_U32(routing[i],0x801A4148+4*i);
      reload.extraCodeRanges={{0x801A4148,0x801A4170}};
      PowerPC::ppcState.gpr[31]=0x80479D30;
      Require(reload.Run(0x801A4148,{0x801A4170,0x801A41D4})==0x801A41D4 &&
              Memory::Read_U8(0x80479D33)==0 && Memory::Read_U8(0x80479D35)==0,
              "retail scene dispatcher advanced to SSS instead of repeating CSS");
    }
    report << "PASS: count-selector scene exit reloads online CSS without advancing to splash or stage select\n";
    for(unsigned stackOffset : {0u,8u,16u,24u})
    for(unsigned count : {1u,2u,3u,4u}) for(unsigned selected=0;selected<=count;++selected)
      for(bool requestInScene : {false,true})
      {
        Runner back(scene); back.Status(count,0);
        PowerPC::ppcState.gpr[1] += stackOffset;
        Memory::Write_U8(1,Runner::Css+scene.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
        Memory::Write_U8(2,requestInScene?Runner::SceneData+3:Runner::R13-0x49AA);
        Memory::Write_U8(u8((1u<<selected)-1),Runner::Css+scene.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+4);
        for(unsigned player=0;player<4;++player)
        {
          Memory::Write_U8(u8(player+1),Runner::SceneData+0x70+36*player);
          Memory::Write_U8(3,Runner::SceneData+0x73+36*player);
          Memory::Write_U8(2,Runner::SceneData+0x79+36*player);
          const u32 door=0x803F0DFC+36*player;
          Memory::Write_U8(1,door+9); Memory::Write_U8(2,door+0x0a);
          Memory::Write_U8(3,door+0x0d); Memory::Write_U8(u8(player+1),door+0x0e); Memory::Write_U8(u8(player+1),door+0x0f);
        }
        const u32 retailMajor[]={0x3C808048,0x38849D30,0x98640001,0x38000001,0x9804000C,0x4E800020};
        for(unsigned i=0;i<6;++i) Memory::Write_U32(retailMajor[i],0x801A42F8+4*i);
        back.extraCodeRanges={{0x801A42F8,0x801A4310}};
        back.services[0x801BAAD0]=[]{ throw std::runtime_error("Back reached event character copying"); };
        PowerPC::ppcState.gpr[3]=Runner::Minor;
        Require(back.Run(scene.Symbol("CSSSceneDecide"),{scene.Symbol("SplashSceneInit"),Runner::Return})==Runner::Return &&
                Memory::Read_U8(0x80479D31)==1 && Memory::Read_U8(0x80479D3C)==1 &&
                back.packets.size()==2 && back.packets[0].size()==LocalTeams::PollPayloadSize+1 &&
                back.packets[0][0]==0xc5 && back.packets[0][1]==0x23 && back.packets[0][10]==4 &&
                back.packets[1]==std::vector<u8>{u8(scene.Symbol("CONST_SlippiCmdCleanupConnections"))},
                "native Back advanced to stage select or failed to return to menu/close local clients");
        for(unsigned player=0;player<4;++player)
        {
          const u32 door=0x803F0DFC+36*player;
          Require(Memory::Read_U8(Runner::SceneData+0x70+36*player)==26 &&
                  Memory::Read_U8(Runner::SceneData+0x73+36*player)==0 && Memory::Read_U8(Runner::SceneData+0x79+36*player)==0 &&
                  Memory::Read_U8(door+9)==0 && Memory::Read_U8(door+0x0a)==0 && Memory::Read_U8(door+0x0d)==0 &&
                  Memory::Read_U8(door+0x0e)==25 && Memory::Read_U8(door+0x0f)==25,
                  "mode exit retained a native card's character, costume or team");
        }
      }
    for(unsigned localReady : {0u,1u}) for(unsigned remoteReady : {0u,1u})
    {
      Runner guard(scene); guard.Status(2,0);
      Memory::Write_U8(1,Runner::Css+scene.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      Memory::Write_U8(u8(localReady),Runner::Msrb+1); Memory::Write_U8(u8(remoteReady),Runner::Msrb+2);
      Memory::Write_U8(0xFF,Runner::R13+scene.Symbol("OFST_R13_ISWINNER"));
      guard.services[0x801BAAD0]=[]{ Require(PowerPC::ppcState.gpr[3]==Runner::Minor,"scene argument was overwritten"); };
      PowerPC::ppcState.gpr[3]=Runner::Minor;
      Require(guard.Run(scene.Symbol("CSSSceneDecide"),{scene.Symbol("SplashSceneInit"),Runner::Return})==
              (localReady&&remoteReady?scene.Symbol("SplashSceneInit"):Runner::Return),"unready CSS exit entered an uninitialized match");
      if(!(localReady&&remoteReady)) Require(Memory::Read_U8(0x80479D35)==1,"unready exit did not repeat CSS");
    }
    report << "PASS: native Back at every local count/selection returns to menu using retail major routing; unready exits cannot advance; event callback receives correct scene argument\n";
    for(unsigned count : {1u,2u,3u,4u}) for(unsigned holder=0;holder<count;++holder)
    {
      Runner hold(css); hold.Status(count,0,2);
      Memory::Write_U8(1,Runner::Css+css.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      Memory::Write_U8(u8(css.Symbol("MM_STATE_CONNECTION_SUCCESS")),Runner::Msrb);
      bool pressed=true;
      hold.services[css.Symbol("Inputs_GetPlayerHeldInputs")]=[&]{
        PowerPC::ppcState.gpr[4]=(pressed&&PowerPC::ppcState.gpr[3]==holder)?0x10:0;
      };
      auto tick=[&]{ PowerPC::ppcState.gpr[7]=0x10; PowerPC::ppcState.spr[SPR_LR]=Runner::Return; hold.Run(Elf::Base,exits); };
      for(unsigned frame=1;frame<=48;++frame) { tick(); Require(hold.packets.empty(),"connected Z disconnected before stock hold threshold"); }
      pressed=false; tick();
      Require(!Memory::Read_U8(Runner::Css+css.Symbol("CSSDT_NATIVE_Z_TIMERS")+holder),"Z release did not reset hold timer");
      pressed=true;
      for(unsigned frame=1;frame<=48;++frame) { tick(); Require(hold.packets.empty(),"partial Z hold survived release"); }
      tick();
      Require(hold.packets.size()==1 && hold.packets[0][0]==css.Symbol("CONST_SlippiCmdCleanupConnections"),"49-frame Z hold failed to disconnect local group");
    }
    for(unsigned connection : {css.Symbol("MM_STATE_MATCHMAKING"),css.Symbol("MM_STATE_OPPONENT_CONNECTING"),css.Symbol("MM_STATE_ERROR_ENCOUNTERED")})
    {
      Runner cancel(css); cancel.Status(2,0,1);
      Memory::Write_U8(1,Runner::Css+css.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      Memory::Write_U8(u8(connection),Runner::Msrb); PowerPC::ppcState.gpr[7]=0x10;
      cancel.Run(Elf::Base,exits);
      Require(cancel.packets.size()==1 && cancel.packets[0][0]==css.Symbol("CONST_SlippiCmdCleanupConnections"),"search/error cancellation no longer matches stock press-Z behavior");
    }
    report << "PASS: connected Z uses stock 49-frame hold for each local, resets on release; searching/connecting/error use stock press-Z cancellation\n";
    {
      Runner game(input);
      auto& cpu = PowerPC::ppcState;
      cpu.gpr[1] = Runner::Stack - 0xE0;
      Memory::Write_U32(Runner::Stack, cpu.gpr[1]);
      Memory::Write_U32(Runner::Return, Runner::Stack + 4);
      cpu.gpr[26] = 123; cpu.gpr[27] = Runner::Css;
      game.Run(input.Symbol("INCREMENT_AND_EXIT"), {Elf::Base + u32(input.text.size())});
      Require(Memory::Read_U32(Runner::Css + input.Symbol("ODB_FRAME")) == 124, "gameplay frame did not advance");
      Require(game.allocations == 0 && game.packets.empty(), "gameplay fell through into CSS EXI polling");
      Require(cpu.gpr[1] == Runner::Stack, "gameplay hook did not restore its parent stack");
    }
    report << "PASS: assembled gameplay exit restores stack and never executes CSS polling\n";
    Elf nativeCount(directory + "native-count.elf"), nativeInit(directory + "native-init.elf"),
        nativeDraw(directory + "native-draw.elf"), nativeTeam(directory + "native-team.elf");
    for (bool enabled : {false, true})
    {
      Runner count(nativeCount); count.Status(2, 0);
      Memory::Write_U8(u8(enabled), Runner::Css + nativeCount.Symbol("CSSDT_LOCAL_TEAMS_STATUS") + 9);
      Memory::Write_U8(14, Runner::SceneData + 2);
      PowerPC::ppcState.gpr[3] = Runner::SceneData;
      count.Run(Elf::Base, {Elf::Base + u32(nativeCount.text.size())});
      Require(PowerPC::ppcState.gpr[0] == (enabled ? 0u : 14u) && PowerPC::ppcState.gpr[3] == Runner::SceneData + 2,
              "native cursor-path override changed CSS pointer or stock single-player behavior");
      Require(Memory::Read_U8(Runner::SceneData + 2) == 14, "native cursor override replaced the online scene type");
    }
    for(unsigned count : {1u,2u,3u,4u}) for (unsigned player=0;player<count;++player)
    {
      const unsigned characters[]={2,20,13,0};
      Runner ready(css); ready.Status(count, 0);
      const auto status = Runner::Css + css.Symbol("CSSDT_LOCAL_TEAMS_STATUS");
      Memory::Write_U8(1, status + 9); Memory::Write_U8(u8((1u<<count)-1), status + 10); Memory::Write_U8(u8(1u << player), status + 11);
      Memory::Write_U8(u8(((1u<<count)-1) ^ (1u<<player)), status + 4);
      const auto token = Runner::Buffer + 0x100;
      Memory::Write_U32(token, 0x804A0BD0 + 4*player);
      Memory::Write_U8(0, token + 5);
      Memory::Write_U8(u8(characters[player]), Runner::SceneData + 0x70 + 36*player);
      Memory::Write_U8(u8(player+1), Runner::SceneData + 0x73 + 36*player);
      Memory::Write_U8(u8(player%2), Runner::SceneData + 0x79 + 36*player);
      SetCR(0); // exercise native path when stock VS would bypass this injection
      ready.Run(Elf::Base, exits);
      Require(ready.packets.size() == 1 && ready.packets[0][0] == 0xC6 && ready.packets[0][8] == (0x80|player),
              "native Start sent the wrong logical-player discriminator");
      Require(ready.packets[0][1] == player%2 && ready.packets[0][2] == characters[player] && ready.packets[0][3] == player+1,
              "native Start captured another player's character, costume or team");
      Require(Memory::Read_U8(Runner::R13 - 0x49AA) == 4 && Memory::Read_U8(Runner::R13 - 0x49A7) == 0,
              "final native Start did not open room entry on the primary controller");
    }
    report << "PASS: native multiplayer cursor-path override, any of 1-4 players can ready last, exact independent selections and primary room entry\n";
    for(unsigned count : {1u,2u,3u,4u})
    {
      Runner empty(css); empty.Status(count,0);
      const auto status=Runner::Css+css.Symbol("CSSDT_LOCAL_TEAMS_STATUS");
      Memory::Write_U8(1,status+9); Memory::Write_U8(u8((1u<<count)-1),status+10);
      Memory::Write_U8(u8((1u<<count)-1),status+11);
      for(unsigned player=0;player<count;++player) {
        Memory::Write_U32(Runner::Buffer+0x100*player,0x804A0BD0+4*player);
        Memory::Write_U8(0,Runner::Buffer+0x100*player+5); // retail empty-token default
        Memory::Write_U8(26,Runner::SceneData+36*player+0x70);
      }
      empty.Run(Elf::Base,exits);
      Require(empty.packets.empty() && !Memory::Read_U8(status+4) && !Memory::Read_U8(Runner::R13-0x49AA),
              "Start readied an empty fallback portrait or entered room entry without a character");
    }
    report << "PASS: empty native selections cannot ready or open code entry at any local count\n";
    {
      Runner first(css); first.Status(2, 0);
      const auto status=Runner::Css+css.Symbol("CSSDT_LOCAL_TEAMS_STATUS");
      Memory::Write_U8(1,status+9); Memory::Write_U8(3,status+10); Memory::Write_U8(1,status+11);
      Memory::Write_U32(Runner::Buffer,0x804A0BD0);
      Memory::Write_U8(0,Runner::Buffer+5);
      SetCR(0);
      first.Run(Elf::Base,exits);
      Require(first.packets.size()==1 && first.packets[0][8]==0x80 && Memory::Read_U8(status+4)==1 &&
              Memory::Read_U8(Runner::R13-0x49AA)==0,"one player's Start opened the keyboard or readied both players");
      // Exercise the entire callback/return path with stale keyboard Start.
      Memory::Write_U8(5,status+5); Memory::Write_U8(3,status+4);
      PowerPC::ppcState.spr[SPR_LR]=Runner::Return;
      first.Run(css.Symbol("FN_TX_FIND_MATCH"),{Runner::Return});
      Require(Memory::Read_U8(status+5)==1 && !Memory::Read_U8(status+10) && !Memory::Read_U8(status+11),
              "code callback left a cached EnteringCode/Start that can open a second window");
      PowerPC::ppcState.spr[SPR_LR]=Runner::Return;
      first.Run(Elf::Base,exits);
      Require(Memory::Read_U8(Runner::R13-0x49AA)==0 && first.packets.size()==2,
              "return from confirmed code entry opened a second keyboard");
    }
    report << "PASS: first local Start remains on CSS; confirmed code callback/return never reopens its keyboard\n";
    {
      Runner pads(input); pads.Status(2, 0);
      pads.pollResponse.assign(LocalTeams::StatusSize, 0);
      pads.pollResponse[0]=1; pads.pollResponse[1]=2; pads.pollResponse[9]=1;
      pads.pollResponse[10]=3; pads.pollResponse[11]=3; pads.pollResponse[12]=0; pads.pollResponse[13]=2;
      pads.pollResponse[32]=3;
      const auto raw = Runner::Stack + input.Symbol("P1_PAD_OFFSET");
      const u8 values[48] = {0x11,0,12,13,14,15,16,17,0,0,0,0, 2,0,22,23,24,25,26,27,0,0,0,0,
                            0x10,0x10,32,33,34,35,36,37,0,0,0,0, 1,0,42,43,44,45,46,47,0,0,0,0};
      Memory::CopyToEmu(raw, values, sizeof(values));
      pads.Run(Elf::Base, {Elf::Base + u32(input.text.size())});
      Require(Memory::Read_U16(raw) == 0x100 && Memory::Read_U8(raw+2)==12 &&
              Memory::Read_U16(raw+12)==0x10 && Memory::Read_U8(raw+14)==32, "native CSS remapped pads incorrectly or let stock VS consume Start");
      Require(Memory::Read_U8(raw+34)==0xFF && Memory::Read_U8(raw+46)==0xFF, "native CSS enabled unused physical controllers");
      Require(pads.packets.size()==1 && pads.packets[0].size()==LocalTeams::PollPayloadSize+1 && pads.packets[0][6]==0x10 && pads.packets[0][7]==0x10,
              "native CSS lost raw Start before coordinator polling");
    }
    for (unsigned phase : {0u,1u,2u,5u})
    {
      Runner moving(input); moving.Status(2,0);
      moving.pollResponse.assign(LocalTeams::StatusSize,0);
      moving.pollResponse[0]=1; moving.pollResponse[1]=2; moving.pollResponse[4]=phase==0?1:3;
      moving.pollResponse[5]=u8(phase); moving.pollResponse[9]=1;
      moving.pollResponse[12]=0; moving.pollResponse[13]=2;
      moving.pollResponse[32]=3;
      const auto raw=Runner::Stack+input.Symbol("P1_PAD_OFFSET");
      u8 values[48]={};
      values[0]=0x11; values[2]=55; values[3]=66;
      values[24]=0x11; values[25]=0x10; values[26]=77; values[27]=88;
      Memory::CopyToEmu(raw,values,sizeof(values));
      moving.Run(Elf::Base,{Elf::Base+u32(input.text.size())});
      Require(Memory::Read_U8(raw+2)==55 && Memory::Read_U8(raw+3)==66 &&
              Memory::Read_U8(raw+14)==77 && Memory::Read_U8(raw+15)==88,
              "ready/searching/waiting native CSS froze a cursor's movement");
      Require(Memory::Read_U16(raw)==0 && Memory::Read_U16(raw+12)==(phase==0?0x110:0x10),
              "locked character controls leaked or unready player's controls were blocked");
    }
    report << "PASS: both cursors retain stick movement after ready, code entry, searching and connecting\n";
    for(unsigned primary=0;primary<4;++primary)
    {
      Runner blocked(input); blocked.Status(2,0);
      blocked.pollResponse.assign(LocalTeams::StatusSize,0);
      blocked.pollResponse[0]=blocked.pollResponse[9]=1; blocked.pollResponse[1]=2;
      blocked.pollResponse[12]=u8(primary); blocked.pollResponse[13]=4; blocked.pollResponse[32]=1;
      const auto raw=Runner::Stack+input.Symbol("P1_PAD_OFFSET");
      u8 values[48]={};
      for(unsigned i=0;i<4;++i) { values[12*i]=0x1f; values[12*i+1]=0x7f; values[12*i+2]=u8(10+i); }
      Memory::CopyToEmu(raw,values,sizeof(values));
      blocked.Run(Elf::Base,{Elf::Base+u32(input.text.size())});
      Require(Memory::Read_U8(raw+2)==10+primary,"entering controller failed to own card one");
      for(unsigned i=1;i<4;++i) Require(Memory::Read_U16(raw+12*i)==0 && Memory::Read_U8(raw+12*i+2)==0 && Memory::Read_U8(raw+12*i+10)==(i==1?0:0xff),
                                      "unclaimed controller affected CSS input");
      blocked.pollResponse[32]=3; blocked.pollResponse[13]=u8((primary+1)%4); blocked.pollResponse[33]=2;
      Memory::CopyToEmu(raw,values,sizeof(values)); PowerPC::ppcState.spr[SPR_LR]=Runner::Return;
      blocked.Run(Elf::Base,{Elf::Base+u32(input.text.size())});
      Require(Memory::Read_U16(raw+12)==0 && Memory::Read_U8(raw+14)==10+(primary+1)%4 && Memory::Read_U8(raw+22)==0,"joining buttons leaked or held stick movement did not immediately reach the claimed cursor");
    }
    report << "PASS: every primary adapter port owns card one; unclaimed controls stay masked, claimed movement works immediately while joining buttons remain consumed\n";
    Elf menuEnter(directory+"menu-enter.elf"), gameInit(directory+"game-init.elf"), cursorPort(directory+"cursor-port.elf"), tokenPort(directory+"token-port.elf");
    for(unsigned primary=0;primary<4;++primary)
    {
      Runner enter(menuEnter); Memory::Write_U8(u8(primary),Runner::R13-0x5108);
      enter.pollResponse.assign(LocalTeams::StatusSize,0);
      enter.Run(menuEnter.Symbol("FN_LOCAL_TEAMS_ENTER"),{Runner::Return});
      Require(enter.packets.size()==1 && enter.packets[0].size()==LocalTeams::PollPayloadSize+1 && enter.packets[0][1]==0x23 && enter.packets[0][10]==primary,
              "mode entry did not transmit the actual entering controller");
      Runner game(gameInit); game.Status(2,0);
      Memory::Write_U8(1,Runner::Css+gameInit.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      Memory::Write_U8(u8(primary),Runner::Css+gameInit.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+12);
      Memory::Write_U8(0,Runner::R13-0x5108);
      PowerPC::ppcState.gpr[27]=Runner::Buffer; // this section uses r27; later routines redefine the alias
      game.Run(gameInit.Symbol("LOCAL_TEAMS_SELECT_SOURCE"),{gameInit.Symbol("LOCAL_TEAMS_SOURCE_SET")});
      Require(Memory::Read_U8(Runner::Buffer+gameInit.Symbol("ODB_INPUT_SOURCE_INDEX"))==primary,
              "gameplay switched the primary input back to adapter port one");
      Runner token(tokenPort); token.Status(2,0);
      Memory::Write_U8(1,Runner::Css+tokenPort.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      Memory::Write_U8(u8(primary),Runner::Css+tokenPort.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+13);
      PowerPC::ppcState.gpr[29]=Runner::Buffer; Memory::Write_U8(1,Runner::Buffer+4);
      token.Run(Elf::Base,{Elf::Base+u32(tokenPort.text.size())});
      Require(PowerPC::ppcState.gpr[4]==primary && Memory::Read_U8(Runner::Buffer+4)==1,"token label changed card ownership or lost its adapter port");
      for(unsigned team=0;team<3;++team)
      {
        Runner cursor(cursorPort); cursor.Status(2,0);
        Memory::Write_U8(1,Runner::Css+cursorPort.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
        Memory::Write_U8(u8(primary),Runner::Css+cursorPort.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+13);
        PowerPC::ppcState.gpr[31]=Runner::Buffer; Memory::Write_U8(1,Runner::Buffer+4);
        rPS0(1)=double(4+team);
        unsigned requested=99;
        cursor.services[cursorPort.Symbol("FN_IntToFloat")]=[&]{ requested=PowerPC::ppcState.gpr[3]; rPS0(1)=double(requested); };
        cursor.Run(Elf::Base,{Elf::Base+u32(cursorPort.text.size())});
        Require(requested==4*primary+team && PowerPC::ppcState.gpr[31]==Runner::Buffer,"hand label lost its real port/team or card pointer");
      }
    }
    report << "PASS: all four mode-entry/gameplay source ports and real hand/token port labels, preserving card ownership and team colors\n";
    for (bool own : {false, true})
    {
      Runner team(nativeTeam); team.Status(2,0);
      Memory::Write_U8(1, Runner::Css + nativeTeam.Symbol("CSSDT_LOCAL_TEAMS_STATUS") + 9);
      PowerPC::ppcState.gpr[31]=Runner::Buffer;
      Memory::Write_U8(1, Runner::Buffer+4);
      PowerPC::ppcState.gpr[25]=0x803F0DFC + (own ? 36 : 0);
      const auto end = Elf::Base + u32(nativeTeam.text.size());
      Require(team.Run(Elf::Base,{end,0x80261BFC}) == (own ? end : 0x80261BFC), "native cursor changed another player's team");
    }
    report << "PASS: actual native pad remap 1/3, Start masking, unused cursors and team ownership\n";
    auto writeFloat = [](float value, u32 address) { u32 bits; std::memcpy(&bits,&value,4); Memory::Write_U32(bits,address); };
    auto readFloat = [](u32 address) { const auto bits=Memory::Read_U32(address); float value; std::memcpy(&value,&bits,4); return value; };
    for(unsigned count : {1u,2u,3u,4u})
    for(bool waiting : {false,true})
    {
      Runner init(nativeInit); init.Status(count,0);
      if(waiting) Memory::Write_U8(1,Runner::Css+nativeInit.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+32);
      Memory::Write_U8(1, Runner::Css + nativeInit.Symbol("CSSDT_LOCAL_TEAMS_STATUS") + 9);
      Memory::Write_U32(Runner::Buffer,Runner::R13-0x49C8);
      Memory::Write_U32(0x81207000,Runner::Buffer+0x60);
      init.services[0x80370E44]=[]{
        Require(PowerPC::ppcState.gpr[3]==0x81207000,"native header did not load SingleMenu descriptors");
        PowerPC::ppcState.gpr[3]=0x81208000;
      };
      std::vector<u32> removedDObjs;
      for(unsigned joint : {1u,36u})
      {
        Memory::Write_U32(0x81300000+joint*0x100,0x81100000+joint*0x100+0x18);
        Memory::Write_U32(0x81400000+joint*0x100,0x81210000+joint*0x100+0x18);
      }
      Memory::Write_U32(0x81410000,0x81400100+8); // frame material/texture/image
      Memory::Write_U32(0x81411000,0x81410000+8);
      Memory::Write_U32(0x81412000,0x81411000+0x58);
      Memory::Write_U32(0x81413000,0x81412000);
      Memory::Write_U32(0x00680048,0x81412000+4);
      const std::vector<u8> originalFrame(104*72/2,0xFF);
      Memory::CopyToEmu(0x81413000,originalFrame.data(),originalFrame.size());
      init.services[0x8035E24C]=[&]{ removedDObjs.push_back(PowerPC::ppcState.gpr[3]); };
      init.services[0x80371590]=[]{
        Require(PowerPC::ppcState.gpr[3]==0x81208000,"native header released the live menu root");
        for(unsigned joint : {1u,36u})
          Require(Memory::Read_U32(0x81210000+joint*0x100+0x18)==0,"header transfer freed its new frame/title with the temporary menu");
      };
      for (unsigned i=0;i<4;++i)
      {
        const u32 cursor=0x81200000+i*0x100, token=0x81203000+i*0x100;
        Memory::Write_U32(cursor,0x804A0BC0+4*i); Memory::Write_U32(token,0x804A0BD0+4*i);
        Memory::Write_U32(cursor+0x40,cursor); Memory::Write_U32(token+0x40,token);
        Memory::Write_U32(cursor+0x80,cursor+0x40+0x28); Memory::Write_U32(token+0x80,token+0x40+0x28);
      }
      init.services[nativeInit.Symbol("JObj_GetJObjChild")]=[] {
        auto& cpu=PowerPC::ppcState;
        Memory::Write_U32((cpu.gpr[3]==0x81208000?0x81210000:0x81100000)+cpu.gpr[5]*0x100,cpu.gpr[4]);
      };
      init.services[nativeInit.Symbol("JObj_SetFlagsAll")]=[] {
        auto& cpu=PowerPC::ppcState; Memory::Write_U32(cpu.gpr[4],cpu.gpr[3]+0x14);
      };
      init.Run(Elf::Base,{Elf::Base+u32(nativeInit.text.size())});
      Require(Memory::Read_U8(Runner::Css+nativeInit.Symbol("CSSDT_NATIVE_VISIBLE_COUNT"))==count,
              "new CSS did not commit its matching visible player count");
      for(unsigned player=0;player<4;++player)
        Require((Memory::Read_U32(0x81200080+player*0x100+0x14)==0x10)==(player>=count || (waiting && player>0)),
                "native CSS cursor visibility did not match claimed cards");
      for(unsigned player=0;player<count;++player)
        Require(Memory::Read_U8(0x81203000+player*0x100+7)==40,"first visible token can retain the wrong adapter-port label");
      const float shift=count==1?0:count==2?-2.6f:count==3?6.4f:9.4f;
      if(count>=2) Require(std::abs(readFloat(0x803F0DFC+36+0x1C)-(-11.4f-shift))<0.001f,
                          "overlapping panel's team hitbox did not move with its marker");
      Require(removedDObjs==std::vector<u32>({0x81300100,0x81302400}),"header replacement removed a panel instead of the old frame/title");
      for(unsigned joint : {1u,36u})
        Require(Memory::Read_U32(0x81100000+joint*0x100+0x18)==0x81400000+joint*0x100,
                "native header retained incompatible VS geometry");
      Require(Memory::Read_U32(0x81402400+0x14)==1,"stock 10-Man heading was not hidden independently of the Teams title");
      for(unsigned y=0;y<72;++y) for(unsigned x=0;x<104;x+=2)
      {
        const unsigned offset=(y/8)*416+(x/8)*32+(y%8)*4+(x%8)/2;
        const bool glyph=x>=16&&x<24&&y>=17&&y<30;
        if(!glyph) Require(Memory::Read_U8(0x81413000+offset)==0xFF,"count digit damaged the original hyphen/P or surrounding Teams ring/frame");
      }
      for(unsigned y=17;y<30;++y)
        Require(Memory::Read_U32(0x81413000+(y/8)*416+64+(y%8)*4)!=0,"count digit has an empty row");
      for(unsigned player=0;player<count;++player)
      {
        const float left=readFloat(0x803F0DFC+36*player+0x1C), right=readFloat(0x803F0DFC+36*player+0x20);
        Require(std::abs(readFloat(0x81200000+player*0x100+0x0C)-(left+right)*0.5f)<0.001f &&
                std::abs(readFloat(0x81200000+player*0x100+0x10)+3.4f)<0.001f,
                "a local hand did not spawn on its own shifted team marker");
      }
    }
    for(unsigned count : {1u,2u,3u,4u})
    {
      Runner draw(nativeDraw); draw.Status(count,0);
      Memory::Write_U8(1,Runner::Css+nativeDraw.Symbol("CSSDT_NATIVE_INITIALIZED"));
      Memory::Write_U8(u8(count),Runner::Css+nativeDraw.Symbol("CSSDT_NATIVE_VISIBLE_COUNT"));
      std::vector<unsigned> order;
      std::vector<unsigned> hiddenGroups;
      for(unsigned player=0;player<4;++player)
      {
        const u32 data=0x81205000+player*0x100, name=0x81206000+player*0x100;
        Memory::Write_U32(data,0x803F0E8C+player*12); Memory::Write_U32(name,data);
      }
      for (unsigned player=0;player<count;++player)
        for (unsigned component=0;component<3;++component)
        {
          const unsigned joint=(component==0?41:component==1?51:56)+player;
          const auto offset=player*12+component*4;
          Memory::Write_U32(0x81100000+joint*0x100,Runner::Css+nativeDraw.Symbol("CSSDT_NATIVE_JOINTS")+offset);
          writeFloat(float(player*15.4f),Runner::Css+nativeDraw.Symbol("CSSDT_NATIVE_ORIGINAL_X")+offset);
        }
      draw.services[nativeDraw.Symbol("JObj_GetJObjChild")]=[] {
        auto& cpu=PowerPC::ppcState; Memory::Write_U32(0x81100000+cpu.gpr[5]*0x100,cpu.gpr[4]);
      };
      for (const auto address : {nativeDraw.Symbol("JObj_SetFlagsAll"),nativeDraw.Symbol("JObj_SetFlags"),nativeDraw.Symbol("JObj_ClearFlags"),0x80391070u})
        draw.services[address]=[]{};
      draw.services[nativeDraw.Symbol("JObj_SetFlagsAll")]=[&hiddenGroups]{ hiddenGroups.push_back((PowerPC::ppcState.gpr[3]-0x81100000)/0x100); };
      draw.services[0x80390EB8]=[]{ PowerPC::ppcState.gpr[3]=2; };
      draw.services[0x803709DC]=[&order]{ order.push_back((PowerPC::ppcState.gpr[3]-0x81100000)/0x100); };
      for (unsigned pass=0;pass<3;++pass)
      {
        const auto drawnBefore=order.size();
        if(pass) // backend resize/poll precedes CSS teardown: keep drawing the old complete layout
          Memory::Write_U8(u8((count+pass-1)%4+1),Runner::Css+nativeDraw.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+1);
        for(unsigned player=0;player<count;++player)
          Memory::Write_U8(pass==0?25:u8(player),0x803F0DFC+36*player+0x0E);
        PowerPC::ppcState.gpr[3]=Runner::Buffer; PowerPC::ppcState.gpr[4]=1;
        PowerPC::ppcState.spr[SPR_LR]=Runner::Return;
        draw.Run(nativeDraw.Symbol("DRAW_CALLBACK")+4,{Runner::Return});
        Require(order.size()-drawnBefore==count*(pass?3u:2u),
                "backend resize changed the drawn player fields before CSS reset");
        const float shift=count==1?0:count==2?-2.6f:count==3?6.4f:9.4f;
        if(count>=2) Require(std::abs(readFloat(0x81100000+42*0x100+0x38)-(15.4f-shift))<0.001f,
                            "panel moved to a new roster's spacing before CSS reset");
        if(count>=2) Require(std::abs(readFloat(0x81100000+(40+count)*0x100+0x38)-18.0f)<0.001f,
                            "two/three/four-player layouts do not share the same right margin before status text");
      }
      std::vector<unsigned> expected;
      for(unsigned pass=0;pass<3;++pass) for(unsigned i=count;i>0;--i)
        for(unsigned base : {41u,51u,56u}) if(pass||base!=51) expected.push_back(base+i-1);
      Require(order==expected,"overlapping panels did not draw in descending player order");
      Require(std::find(hiddenGroups.begin(),hiddenGroups.end(),1)==hiddenGroups.end() &&
              std::find(hiddenGroups.begin(),hiddenGroups.end(),39)==hiddenGroups.end(),"VS banner was visible or Back was hidden");
      for(unsigned player=0;player<4;++player)
        Require(Memory::Read_U8(0x81206000+player*0x100+0x4D)==1,"character name beneath portrait was visible");
    }
    report << "PASS: all 1-4 native layouts, descending panel overlap, hidden names/10-Man heading, stock hyphen/P/ring retained with matching digit, own-marker cursor spawns and Back preserved\n";
    report << "PASS: two/three/four-player right margins match; backend roster changes keep old portrait spacing/fields until CSS initialization\n";
    for(unsigned joined : {1u,3u,7u,15u})
    {
      Runner visibility(nativeDraw); visibility.Status(4,0);
      Memory::Write_U8(1,Runner::Css+nativeDraw.Symbol("CSSDT_NATIVE_INITIALIZED"));
      Memory::Write_U8(4,Runner::Css+nativeDraw.Symbol("CSSDT_NATIVE_VISIBLE_COUNT"));
      Memory::Write_U8(u8(joined),Runner::Css+nativeDraw.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+32);
      for(unsigned i=0;i<4;++i)
      {
        const u32 hand=0x81200000+i*0x100, token=0x81203000+i*0x100;
        Memory::Write_U32(hand,0x804A0BC0+4*i); Memory::Write_U32(token,0x804A0BD0+4*i);
        Memory::Write_U32(hand+0x40,hand); Memory::Write_U32(token+0x40,token);
        Memory::Write_U32(hand+0x80,hand+0x40+0x28); Memory::Write_U32(token+0x80,token+0x40+0x28);
        Memory::Write_U32(0x50,hand+0x80+0x14); Memory::Write_U32(0x40,token+0x80+0x14);
        Memory::Write_U8(u8(i),hand+4); writeFloat(float(i),hand+0x0c);
      }
      visibility.Run(nativeDraw.Symbol("DRAW_CALLBACK")+4,{nativeDraw.Symbol("NATIVE_CURSOR_VISIBILITY_DONE")});
      for(unsigned i=0;i<4;++i)
      {
        const bool claimed=(joined & (1u<<i))!=0;
        Require(Memory::Read_U32(0x81200080+i*0x100+0x14)==(claimed?0x40u:0x50u),"unclaimed hand visible or claimed hand failed to reveal");
        Require(Memory::Read_U32(0x81203080+i*0x100+0x14)==(claimed?0x40u:0x50u),"unclaimed token visible or another render flag changed");
        Require(Memory::Read_U8(0x81200000+i*0x100+4)==i && readFloat(0x81200000+i*0x100+0x0c)==float(i),"claim visibility changed cursor ownership or position");
      }
    }
    report << "PASS: claiming reveals prepared hands without a CSS reload; unclaimed hands and tokens stay hidden without moving existing cursors\n";
    Elf text(directory+"text.elf"), hideRules(directory+"hide-rules.elf"), disableRules(directory+"disable-rules.elf"),
        readyBanner(directory+"ready-banner.elf"), countClick(directory+"count-click.elf");
    for(bool native : {false,true})
    {
      Runner hidden(hideRules); hidden.Status(2,0);
      Memory::Write_U8(u8(native),Runner::Css+hideRules.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      PowerPC::ppcState.gpr[28]=Runner::Buffer;
      const auto end=Elf::Base+u32(hideRules.text.size());
      Require(hidden.Run(Elf::Base,{end,0x80266438})==(native?0x80266438:end),"VS banner suppression changed stock behavior");
      Runner disabled(disableRules); disabled.Status(2,0);
      Memory::Write_U8(u8(native),Runner::Css+disableRules.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      const auto rulesEnd=Elf::Base+u32(disableRules.text.size());
      Require(disabled.Run(Elf::Base,{rulesEnd,0x80261944})==(native?0x80261944:rulesEnd),"VS rules hitbox suppression changed stock behavior");
      Runner banner(readyBanner); banner.Status(2,0);
      Memory::Write_U8(u8(native),Runner::Css+readyBanner.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      PowerPC::ppcState.gpr[3]=0x80390000;
      banner.Run(Elf::Base,{Elf::Base+u32(readyBanner.text.size())});
      Require(PowerPC::ppcState.gpr[4]==(native?readyBanner.Symbol("NO_DRAW")+4:0x80391070),
              "offline Ready to Fight banner remained visible or stock callback changed");
      Require(PowerPC::ppcState.gpr[3]==0x80390000,"ready banner hook corrupted the stock object register");
      if(native)
      {
        PowerPC::ppcState.spr[SPR_LR]=Runner::Return;
        banner.Run(readyBanner.Symbol("NO_DRAW")+4,{Runner::Return});
      }
    }
    report << "PASS: native local Teams suppresses standalone offline Ready to Fight rendering; stock callback preserved\n";
    for(unsigned stackOffset : {0u,8u,16u,24u})
    for(unsigned count : {1u,2u,3u,4u}) for(unsigned scenario=0;scenario<9;++scenario)
    {
      Runner click(countClick); click.Status(count,0,scenario==3?1:0);
      auto& cpu=PowerPC::ppcState;
      cpu.gpr[1] += stackOffset;
      Memory::Write_U8(1,Runner::Css+countClick.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      cpu.gpr[31]=Runner::Buffer+0x500; cpu.gpr[28]=scenario==2?0:0x100;
      Memory::Write_U8(scenario==1?1:0,cpu.gpr[31]+4);
      writeFloat(scenario==0?-10.0f:-18.0f,cpu.gpr[31]+0x0C);
      writeFloat(23.0f,cpu.gpr[31]+0x10);
      const unsigned requested=count==4?1:count+1;
      click.pollResponse.assign(LocalTeams::StatusSize,0); click.pollResponse[0]=1;
      click.pollResponse[1]=u8(scenario==4?count:requested); click.pollResponse[9]=1;
      if(scenario==6) click.pollResponse[0]=0;
      if(scenario==7) click.pollResponse[9]=0;
      if(scenario==8) click.pollResponse[1]=0xEE; // unrelated DMA data must not count as acceptance
      for(unsigned player=0;player<4;++player)
      {
        Memory::Write_U8(u8(player+2),Runner::SceneData+0x70+36*player);
        Memory::Write_U8(u8(player),Runner::SceneData+0x73+36*player);
        Memory::Write_U8(u8(player%2),Runner::SceneData+0x79+36*player);
        Memory::Write_U32(Runner::Buffer+0x600+player*0x20,0x804A0BD0+4*player);
        Memory::Write_U8(player<count?0:5,Runner::Buffer+0x605+player*0x20);
      }
      const auto pass=Elf::Base+u32(countClick.text.size());
      const auto reached=click.Run(Elf::Base,{pass,0x802622A8});
      Require(reached==(scenario<3?pass:0x802622A8u),"count click did not respect P1, A edge, hitbox or consume its action");
      Require(click.packets.size()==unsigned(scenario>=4),"count selector sent a command when not clicked or busy");
      if(scenario>=4)
      {
        const auto& packet=click.packets[0];
        Require(packet.size()==18 && packet[0]==0xC8 && packet[1]==requested,"count selector sent the wrong count or payload length");
        for(unsigned player=0;player<4;++player)
          Require(packet[2+4*player]==player+2 && packet[3+4*player]==player &&
                  packet[4+4*player]==player%2 && packet[5+4*player]==unsigned(player<count),
                  "count selector captured another player's pick or a held token");
      }
      Require(Memory::Read_U8(Runner::Css+countClick.Symbol("CSSDT_NATIVE_RELOAD"))==unsigned(scenario==5) &&
              Memory::Read_U8(Runner::R13-0x49AA)==unsigned(scenario==5?2:0),"rejected count change reloaded CSS or accepted change used a cancellable exit");
    }
    report << "PASS: P1 hover+A count cycle 1-4, placed-pick payloads, hitbox/edge/ownership guards, busy denial and CSS-only rebuild request\n";
    for(bool native : {false,true}) for(unsigned player=0;player<4;++player)
    {
      Runner hold(countClick); hold.Status(4,0);
      Memory::Write_U8(u8(native),Runner::Css+countClick.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      PowerPC::ppcState.gpr[31]=Runner::Buffer+0x500;
      Memory::Write_U8(u8(player),Runner::Buffer+0x504); Memory::Write_U16(30,Runner::Buffer+0x50a);
      hold.Run(Elf::Base,{Elf::Base+u32(countClick.text.size())});
      Require(Memory::Read_U16(Runner::Buffer+0x50a)==(native?0:30),"retail B exit can bypass the per-player leave route or stock timer changed");
    }
    for(unsigned requested : {0u,1u,3u}) for(bool accepted : {false,true})
    {
      const unsigned oldCount=requested==0?3:requested==1?3:2;
      Runner roster(css); roster.Status(oldCount,0);
      const auto status=Runner::Css+css.Symbol("CSSDT_LOCAL_TEAMS_STATUS");
      Memory::Write_U8(1,status+9); Memory::Write_U8(u8(requested),status+34);
      roster.pollResponse.assign(LocalTeams::StatusSize,0);
      roster.pollResponse[0]=u8(accepted); roster.pollResponse[9]=1;
      roster.pollResponse[1]=u8(requested?requested:1);
      for(unsigned player=0;player<4;++player)
      {
        Memory::Write_U8(u8(player+2),Runner::SceneData+0x70+36*player);
        Memory::Write_U8(u8(player),Runner::SceneData+0x73+36*player);
        Memory::Write_U8(u8(player%2),Runner::SceneData+0x79+36*player);
        Memory::Write_U32(Runner::Buffer+0x600+player*0x20,0x804A0BD0+4*player);
        Memory::Write_U8(0,Runner::Buffer+0x605+player*0x20);
      }
      roster.Run(css.Symbol("FN_NATIVE_ROSTER_REQUEST"),{Runner::Return});
      Require(roster.packets.size()==1 && roster.packets[0].size()==18 && roster.packets[0][0]==0xc8 && roster.packets[0][1]==requested,"automatic roster action sent the wrong request");
      for(unsigned player=0;player<4;++player)
        Require(roster.packets[0][2+4*player]==player+2 && roster.packets[0][3+4*player]==player && roster.packets[0][4+4*player]==player%2 && roster.packets[0][5+4*player]==unsigned(player<oldCount),"automatic roster action lost current picks or captured a nonexistent field");
      Require(Memory::Read_U8(Runner::Css+css.Symbol("CSSDT_NATIVE_RELOAD"))==unsigned(accepted&&requested) && Memory::Read_U8(Runner::R13-0x49aa)==unsigned(accepted?2:0),"automatic roster action exited on rejection or reloaded instead of returning to menu");
    }
    report << "PASS: per-player B route preserves the main controller's menu exit; automatic add/remove snapshots current picks and reloads only after acceptance\n";
    {
      Elf loadCss(directory+"load-css.elf");
      File::WriteStringToFile("[LocalTeams]\nEnabled=True\nCount=1\nBasePort=49120\n",
                              File::GetUserPath(D_CONFIG_IDX)+"local-teams.ini");
      LocalTeamsCoordinator ui(0,nullptr);
      const u8 buttons[8]={};
      // Use the real coordinator for each command and each new CSS query.
      // Only retail game services are stubbed; the DMA pointer still rounds
      // down to 32 bytes exactly as EXIDma does on the user's game.
      auto transfer=[&](Runner& runner) {
        auto& cpu=PowerPC::ppcState;
        const u32 address=cpu.gpr[3]&0x03FFFFE0;
        if(cpu.gpr[5]==runner.elf.Symbol("CONST_ExiWrite"))
        {
          std::vector<u8> packet(cpu.gpr[4]);
          Memory::CopyFromEmu(packet.data(),address,packet.size());
          runner.packets.push_back(packet);
          if(packet[0]==0xC8)
          {
            Require(packet.size()==18 && ui.ChangeCount(packet.data()+1),
                    "real coordinator did not receive/accept the clicked roster");
          }
          else Require(packet.size()==LocalTeams::PollPayloadSize+1 && packet[0]==0xC5 && packet[1]==0x83,
                       "CSS reload sent an invalid configuration query");
        }
        else
        {
          Require(cpu.gpr[4]==LocalTeams::StatusSize,"roster round-trip used wrong response size");
          const auto status=ui.Poll(u8(SlippiMatchmaking::TEAMS|0x80),buttons);
          Memory::CopyToEmu(address,status.data(),status.size());
        }
      };
      for(unsigned stackOffset : {0u,8u,16u,24u})
      for(unsigned requested : {2u,3u,4u,1u})
      {
        Runner click(countClick);
        const auto status=ui.Poll(u8(SlippiMatchmaking::TEAMS|0x80),buttons);
        Memory::CopyToEmu(Runner::Css+countClick.Symbol("CSSDT_LOCAL_TEAMS_STATUS"),status.data(),status.size());
        auto& cpu=PowerPC::ppcState;
        cpu.gpr[1]+=stackOffset; cpu.gpr[31]=Runner::Buffer+0x500; cpu.gpr[28]=0x100;
        writeFloat(-30.0f,cpu.gpr[31]+0x0C); writeFloat(24.0f,cpu.gpr[31]+0x10);
        click.services[countClick.Symbol("FN_EXITransferBuffer")]=[&]{transfer(click);};
        click.Run(Elf::Base,{0x802622A8});
        Require(ui.state.count==requested && Memory::Read_U8(Runner::Css+countClick.Symbol("CSSDT_NATIVE_RELOAD"))==1,
                "grey-corner click only reloaded CSS without changing the real coordinator");

        Runner reload(loadCss);
        cpu.gpr[1]+=stackOffset;
        const u32 allocations[]={Runner::Css,Runner::Msrb,Runner::Buffer};
        unsigned nextAllocation=0;
        reload.services[loadCss.Symbol("HSD_MemAlloc")]=[&]{
          Require(nextAllocation<3,"unexpected CSS load allocation");
          cpu.gpr[3]=allocations[nextAllocation++];
        };
        reload.callbackTables[loadCss.Symbol("FG_UserDisplay")]=0x80009000;
        reload.services[0x80009014]=[]{};
        reload.services[loadCss.Symbol("FN_EXITransferBuffer")]=[&]{transfer(reload);};
        Memory::Write_U32(0x48000000,loadCss.Symbol("INJ_InitTeamToggleButton"));
        reload.services[0x80016BE0]=[&]{cpu.gpr[3]=0x81100000;};
        reload.services[0x80380358]=[&]{cpu.gpr[3]=0x81101000;};
        reload.Run(Elf::Base,{Elf::Base+u32(loadCss.text.size())});
        Require(Memory::Read_U8(Runner::Css+loadCss.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+1)==requested,
                "new CSS reverted the clicked player count");
        for(unsigned player=0;player<4;++player)
          Require(Memory::Read_U8(Runner::SceneData+0x71+36*player)==(player<requested?0:3) &&
                  Memory::Read_U8(Runner::SceneData+0x70+36*player)==26,
                  "new CSS did not create exactly the requested empty player fields");
      }
    }
    report << "PASS: real coordinator DMA round-trip and fresh CSS load cycle 1-2-3-4-1 at every 8-byte stack alignment, with matching empty player fields\n";
    for(unsigned handState : {1u,2u}) for(float x : {-35.0f,-30.0f,-18.0f,-14.5f})
    {
      Runner incoming(countClick); incoming.Status(1,0);
      auto& cpu=PowerPC::ppcState;
      Memory::Write_U8(1,Runner::Css+countClick.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      cpu.gpr[31]=Runner::Buffer+0x500; cpu.gpr[28]=0x100; cpu.gpr[2]=0x804D0000;
      Memory::Write_U8(u8(handState),cpu.gpr[31]+5);
      writeFloat(x,cpu.gpr[31]+0x0C); writeFloat(24.0f,cpu.gpr[31]+0x10);
      writeFloat(25.0f,cpu.gpr[2]-0x354C); writeFloat(-22.0f,cpu.gpr[2]-0x3548);
      writeFloat(26.0f,cpu.gpr[2]-0x3544); writeFloat(-35.0f,cpu.gpr[2]-0x3540);
      // Actual retail cursor clamp leading into the injection, including the
      // carried-token state which bypassed the former 0x802616C0 click hook.
      const u32 clamp[]={0xC022CAB4,0xC01F0010,0xFC010040,0x40800008,0xD03F0010,
                         0xC022CAB8,0xC01F0010,0xFC010040,0x40810008,0xD03F0010,
                         0xC022CABC,0xC01F000C,0xFC010040,0x40800008,0xD03F000C,
                         0xC022CAC0,0xC01F000C,0xFC010040,0x40810008,0xD03F000C};
      for(unsigned i=0;i<20;++i) Memory::Write_U32(clamp[i],0x80260888+4*i);
      Memory::Write_U32(0x48000000|((Elf::Base-0x802608D8)&0x03FFFFFC),0x802608D8);
      incoming.extraCodeRanges={{0x80260888,0x802608DC}};
      incoming.pollResponse.assign(LocalTeams::StatusSize,0); incoming.pollResponse[0]=1; incoming.pollResponse[1]=2; incoming.pollResponse[9]=1;
      Require(incoming.Run(0x80260888,{0x802622A8})==0x802622A8 && incoming.packets.size()==1 && incoming.packets[0][1]==2,
              "retail incoming cursor/carried-token path bypassed grey-corner count click");
      // Follow the real OnFrame switch. The old flag 1 could be cancelled by
      // an unplaced P1 token; flag 2 must always notify the scene dispatcher.
      const u32 dispatch[]={0x880DB656,0x2C000003,0x418200EC,0x40800014,0x2C000001,
                            0x41820018,0x408000CC,0x48000138,0x2C000005,0x40800130,0x480000FC};
      for(unsigned i=0;i<11;++i) Memory::Write_U32(dispatch[i],0x80266C08+4*i);
      const u32 leave[]={0x4BF3DE75,0x38600000,0x4BDBD33D,0x48000064};
      for(unsigned i=0;i<4;++i) Memory::Write_U32(leave[i],0x80266CEC+4*i);
      incoming.extraCodeRanges={{0x80266C08,0x80266C34},{0x80266CEC,0x80266CFC}};
      bool exited=false;
      incoming.services[0x801A4B60]=[&]{ exited=true; };
      incoming.Run(0x80266C08,{0x80266D5C});
      Require(exited && Memory::Read_U8(Runner::R13-0x49AA)==2,"retail OnFrame cancelled the native roster rebuild");
    }
    report << "PASS: real retail cursor clamp reaches grey-corner selector with carried/free tokens; empty roster rebuild survives retail OnFrame exit switch\n";
    // Retail Melee's actual menu-loading branch, including the 1P-only old hook.
    const u32 retailMenuLoad[]={0x880DB655,0x906DB61C,0x28000001,0x40820030,0x806DB638,0x80630060,
      0x4810C919,0x906DB620,0x80CDB638,0x806DB620,0x80860064,0x80A60068,0x80C6006C,0x4810B615,
      0x4800002C,0x806DB638,0x80630030,0x4810C8ED,0x906DB620,0x80CDB638,0x806DB620,0x80860034,
      0x80A60038,0x80C6003C,0x4810B5E9,0x888DC1A9,0x806DB61C,0x80ADB620};
    for(bool native : {false,true})
    {
      Runner ui(text); ui.Status(2,0);
      auto& cpu=PowerPC::ppcState;
      cpu.gpr[2]=0x81010000; // Fixture constant table for the title's animation frames.
      writeFloat(0,cpu.gpr[2]-0x513C); writeFloat(16,cpu.gpr[2]-0x5138);
      Memory::Write_U8(u8(native),Runner::Css+text.Symbol("CSSDT_LOCAL_TEAMS_STATUS")+9);
      Memory::Write_U8(native?4:1,Runner::R13-0x49AB);
      Memory::Write_U32(Runner::Buffer,Runner::R13-0x49C8);
      Memory::Write_U8(3,Runner::R13-0x3E57);
      for(unsigned i=0;i<sizeof(retailMenuLoad)/4;++i) Memory::Write_U32(retailMenuLoad[i],0x80264514+i*4);
      ui.extraCodeRanges.push_back({0x80264514,0x80264584});
      ui.services[0x80370E44]=[]{ PowerPC::ppcState.gpr[3]=0x81110000; };
      ui.services[0x8036FB5C]=[]{};
      Require(ui.Run(0x80264514,{text.Symbol("CSS_TEXT_HOOK_ADDR"),0x80264584})==text.Symbol("CSS_TEXT_HOOK_ADDR"),
              "native multiplayer branch skipped Slippi's account/status initialization");
      unsigned accountCalls=0,subtexts=0,textObjects=0;
      ui.callbackTables[text.Symbol("FG_UserDisplay")]=0x80009000;
      ui.services[0x80009000]=[&]{
        ++accountCalls;
        Require(cpu.gpr[4]==1 && cpu.gpr[5]==0,"account display mode/buffer initialization changed");
        Require(std::abs(readFloat(cpu.gpr[3])-(native?250.0f:-112.0f))<0.001f &&
                std::abs(readFloat(cpu.gpr[3]+4)-(native?255.0f:20.0f))<0.001f,"account display position changed");
      };
      ui.services[text.Symbol("GObj_Create")]=[]{ PowerPC::ppcState.gpr[3]=Runner::Buffer+0x800; };
      ui.services[text.Symbol("GObj_AddUserData")]=[]{};
      ui.services[text.Symbol("GObj_AddProc")]=[]{};
      ui.services[text.Symbol("Text_CreateStruct")]=[&]{ cpu.gpr[3]=0x81120000+0x100*textObjects++; };
      ui.services[text.Symbol("FG_CreateSubtext")]=[&]{
        if(subtexts==15) Require(std::abs(rPS0(3)-121.0)<0.001,"P4 row lost the 23-unit spacing of P1-P3");
        cpu.gpr[3]=subtexts; ++subtexts;
      };
      ui.Run(Elf::Base,{Elf::Base+u32(text.text.size())});
      Require(accountCalls==1 && subtexts==(native?17u:15u) && cpu.gpr[4]==3 &&
              Memory::Read_U32(Runner::Css+text.Symbol("CSSDT_TEXT_STRUCT_ADDR"))==0x81120000 && textObjects==1,
              "shared CSS hook did not initialize account/status once or changed the replaced instruction");
      // Both local picks may be on the same team; stock VS's selected flag is zero.
      if(native)
      {
        const auto status=Runner::Css+text.Symbol("CSSDT_LOCAL_TEAMS_STATUS");
        Memory::Write_U8(3,status+4); Memory::Write_U8(1,status+5);
        Memory::Write_U8(0,Runner::R13-0x49A9);
        Memory::Write_U8(u8(text.Symbol("MM_STATE_MATCHMAKING")),Runner::Msrb);
        const u8 room[]={0x82,0x4F,0x82,0x4F,0x82,0x4F,0x82,0x50,0x82,0x50};
        for(unsigned i=0;i<5;++i)Memory::Write_U16((u16(room[2*i])<<8)|room[2*i+1],0x804A0740+3*i);
        std::map<unsigned,std::string> lines;
        auto stringAt=[](u32 address){ std::string value; for(unsigned i=0;i<200;++i){const auto c=Memory::Read_U8(address+i);if(!c)break;value+=char(c);}return value; };
        ui.services[text.Symbol("Text_UpdateSubtextContents")]=[&]{
          const auto format=stringAt(cpu.gpr[5]);
          const unsigned index=cpu.gpr[4];
          lines[index]=format;
          if(format=="P%d: %s") lines[index]+="|"+std::to_string(cpu.gpr[6])+"|"+stringAt(cpu.gpr[7]);
          else if(format.find("%s")!=std::string::npos) lines[index]+="|"+stringAt(cpu.gpr[6]);
        };
        ui.services[text.Symbol("Text_ChangeTextColor")]=[]{};
        ui.services[text.Symbol("JObj_GetJObjChild")]=[&]{ Memory::Write_U32(0x81130000,cpu.gpr[4]); };
        for(auto address:{0x8036F644u,0x8036FA10u,text.Symbol("JObj_ReqAnim"),text.Symbol("JObj_Anim")})ui.services[address]=[]{};
        Memory::Write_U8(1,status+4); Memory::Write_U8(0,status+5);
        Memory::Write_U32(Runner::Buffer+0xA00,0x804A0BD4);
        Memory::Write_U8(0,Runner::Buffer+0xA05);
        cpu.gpr[3]=Runner::Buffer+0x800; cpu.spr[SPR_LR]=Runner::Return;
        ui.Run(text.Symbol("CSS_ONLINE_TEXT_THINK")+4,{Runner::Return});
        Require(lines[1]=="P%d: %s|1|Ready" && lines[3]=="P%d: %s|2|Press START",
                "native CSS does not display each player's independent selection/ready state");
        Memory::Write_U8(4,status+1); Memory::Write_U8(15,status+4); Memory::Write_U8(15,status+32);
        cpu.gpr[3]=Runner::Buffer+0x800; cpu.spr[SPR_LR]=Runner::Return;
        ui.Run(text.Symbol("CSS_ONLINE_TEXT_THINK")+4,{Runner::Return});
        Require(lines[5]=="P%d: %s|3|Ready" && lines[15]=="P%d: %s|4|Ready" &&
                Memory::Read_U8(Runner::Css+text.Symbol("CSSDT_SPINNER4"))==2,
                "four-player readiness overwrote stock help rows or left a stale count");
        Memory::Write_U8(0,status+4); Memory::Write_U8(7,status+32);
        Memory::Write_U8(2,status+13); Memory::Write_U8(1,status+14); Memory::Write_U8(4,status+15);
        cpu.gpr[3]=Runner::Buffer+0x800; cpu.spr[SPR_LR]=Runner::Return;
        ui.Run(text.Symbol("CSS_ONLINE_TEXT_THINK")+4,{Runner::Return});
        Require(lines[15]=="Waiting for input" &&
                lines[3]=="P%d: %s|3|Press START" && lines[5]=="P%d: %s|2|Select your character",
                "unclaimed card label/text or claimed physical-port labels are incorrect");
        Memory::Write_U8(15,status+32); Memory::Write_U8(3,status+15);
        cpu.gpr[3]=Runner::Buffer+0x800; cpu.spr[SPR_LR]=Runner::Return;
        ui.Run(text.Symbol("CSS_ONLINE_TEXT_THINK")+4,{Runner::Return});
        Require(lines[15]=="P%d: %s|4|Select your character",
                "claiming a waiting card did not replace its number with the controller label");
        for(unsigned player=0;player<4;++player) Memory::Write_U8(u8(player),status+12+player);
        Memory::Write_U8(2,status+1);
        Memory::Write_U8(3,status+4); Memory::Write_U8(1,status+5);
        for(unsigned connection : {text.Symbol("MM_STATE_MATCHMAKING"),text.Symbol("MM_STATE_OPPONENT_CONNECTING")})
        {
          Memory::Write_U8(u8(connection),Runner::Msrb); lines.clear();
          cpu.gpr[3]=Runner::Buffer+0x800; cpu.spr[SPR_LR]=Runner::Return;
          ui.Run(text.Symbol("CSS_ONLINE_TEXT_THINK")+4,{Runner::Return});
          Require(lines[5]==std::string(connection==text.Symbol("MM_STATE_MATCHMAKING")?"Searching for %s|":"Connecting to %s|")+std::string(reinterpret_cast<const char*>(room),sizeof(room)),
                  "same-team native CSS did not show the entered room's searching/connecting status");
        }
      }
    }
    report << "PASS: retail single/multiplayer setup reaches Slippi account/status init; same-team search/connecting text contains exact room; offline rules disabled\n";
  }
  catch (...) { Memory::Shutdown(); throw; }
  Memory::Shutdown();
}
}
