#pragma once
#include "Core/Slippi/LocalTeamsState.h"
#include "Core/Slippi/SlippiMatchmaking.h"
#include <memory>

// Called exclusively by the CPU/EXI thread. Matchmaking and ENet retain their own threads.
class LocalTeamsCoordinator
{
public:
  LocalTeamsCoordinator(uintptr_t device, SlippiUser* user);
  bool Active() const { return enabled && active; }
  LocalTeams::State state;
  std::array<u8, LocalTeams::StatusSize> Poll(u8 mode, const u8* buttons);
  bool Confirm(const u8* payload, u32 rng);
  bool ChangeCount(const u8* payload);
  bool BeginSearch(std::unique_ptr<SlippiMatchmaking>& primary);
  SlippiMatchmaking::ProcessState Tick(SlippiMatchmaking& primary, SlippiNetplayClient* netplay);
  void PublishSelections(SlippiNetplayClient& primary);
  bool AllReady(SlippiNetplayClient& primary);
  void StartGame();
  void SendInputs(s32 frame, u8 delay, s32 finalized, u32 checksum, const u8* reports, bool skip);
  void Cleanup();
  void RequestStop();
  bool LobbyDisconnected() const { return lobbyDisconnected; }
  std::string Error() const { return error; }
  std::string Code() const { return lobbyCode; }
  std::string SearchCode() const;
  void SetSearchCode(const std::string& shiftJisCode);
  bool Loopback() const { return loopback; }
  bool NativeCSS() const { return nativeCSS && !loopback; }
  SlippiPlayerSelections PrimarySelection() const;
private:
  friend int RunLocalTeamsSelfTest(const std::string& reportPath);
  struct Session
  {
    std::unique_ptr<SlippiMatchmaking> matchmaking;
    std::unique_ptr<SlippiNetplayClient> netplay;
    LocalTeams::Pad previous{};
  };
  std::array<Session, 3> guests;
  uintptr_t device;
  SlippiUser* user;
  bool enabled = false, active = false, validated = false, published = false;
  bool lobbyDisconnected = false;
  std::array<unsigned, 4> localSlots{{4, 4, 4, 4}};
  unsigned basePort = 49000;
  bool loopback = false;
  bool nativeCSS = true;
  std::array<std::string, 4> identityFiles;
  std::string lobbyCode, error, lastStatus;
  SlippiUser::UserInfo Identity(unsigned index, const SlippiUser::UserInfo& fallback);
  SlippiPlayerSelections Selection(unsigned index) const;
  void ShowStatus();
  void Fail(const std::string& message);
};
