#include "Core/Slippi/LocalTeamsCoordinator.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/OnScreenDisplay.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

LocalTeamsCoordinator::LocalTeamsCoordinator(uintptr_t device_, SlippiUser* user_) : device(device_), user(user_)
{
  IniFile ini;
  ini.Load(File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini");
  auto section = ini.GetOrCreateSection("LocalTeams");
  section->Get("Enabled", &enabled, false);
  section->Get("Count", &state.count, 1u);
  section->Get("BasePort", &basePort, 49000u);
  section->Get("LoopbackTest", &loopback, false);
  section->Get("NativeCSS", &nativeCSS, true);
  section->Get("LobbyCode", &lobbyCode, std::string());
  for (unsigned i = 0; i < 4; ++i)
  {
    unsigned port = i + 1;
    section->Get("Controller" + std::to_string(i + 1), &port, port);
    state.ports[i] = port - 1;
    section->Get("Identity" + std::to_string(i + 1), &identityFiles[i], std::string());
  }
  if (enabled && (!state.Valid() || basePort < 1024 || basePort > 65532 ||
                  (loopback && state.count != 4)))
  {
    enabled = false;
    OSD::AddMessage("Local teams disabled: check local-teams.ini (1-4 players, first controller 1, distinct ports)", 15000, OSD::Color::RED);
  }
}

void LocalTeamsCoordinator::Fail(const std::string& message)
{
  error = message;
  state.phase = LocalTeams::Phase::Error;
  ERROR_LOG(SLIPPI_ONLINE, "[LocalTeams] %s", message.c_str());
  ShowStatus();
}

void LocalTeamsCoordinator::ShowStatus()
{
  std::ostringstream msg;
  if (state.phase == LocalTeams::Phase::Selecting && NativeCSS())
  {
    msg << "Local Teams: ";
    for (unsigned i = 0; i < state.count; ++i)
      msg << (i ? " | " : "") << "P" << i + 1 << ((state.confirmed & (1u << i)) ? " READY" : " pick + Start");
  }
  else if (state.phase == LocalTeams::Phase::Selecting)
    msg << "Local player " << state.active + 1 << "/" << state.count << " (controller "
        << state.ports[state.active] + 1 << "): pick, then press Start";
  else if (state.phase == LocalTeams::Phase::Error) msg << "Local teams: " << error << " | Z to reset";
  else if (state.phase == LocalTeams::Phase::EnteringCode) msg << "Local teams: enter the room code | Start to reopen, Z to reset";
  else if (state.phase == LocalTeams::Phase::Searching) msg << (loopback ? "Local teams: connecting localhost test" : "Local teams: searching for " + lobbyCode);
  else if (state.phase == LocalTeams::Phase::InGame) msg << "Local teams: in game";
  else msg << "Local teams: waiting for ready players";
  for (unsigned i = 0; i < state.count; ++i)
    if (state.confirmed & (1u << i)) msg << " | L" << i + 1 << " char " << unsigned(state.selections[i].character)
                                       << " team " << unsigned(state.selections[i].team) + 1;
  if (msg.str() != lastStatus)
  {
    lastStatus = msg.str();
    if (!NativeCSS() || state.phase == LocalTeams::Phase::Error)
      OSD::AddMessage(lastStatus, 30000, OSD::Color::CYAN);
    INFO_LOG(SLIPPI_ONLINE, "[LocalTeams] %s", lastStatus.c_str());
  }
}

std::array<u8, LocalTeams::StatusSize> LocalTeamsCoordinator::Poll(u8 mode, const u8* buttonBytes)
{
  active = (mode & 0x3F) == SlippiMatchmaking::TEAMS;
  std::array<u8, LocalTeams::StatusSize> result{};
  if (!Active()) return result;
  const bool returnedFromGame = state.phase == LocalTeams::Phase::InGame;
  std::array<std::uint16_t, 4> buttons{};
  for (unsigned i = 0; i < 4; ++i) buttons[i] = (u16(buttonBytes[2 * i]) << 8) | buttonBytes[2 * i + 1];
  if ((mode & 0x40) && !(mode & 0x80))
  {
    // The code keyboard can release/press Start repeatedly. Those inputs must
    // never arm CSS confirmation or immediately reopen a cancelled keyboard.
    state.startArmed = false;
    state.nativeArmed = state.nativeHeld = 0;
  }
  else if (!(mode & 0x80))
  {
    state.Poll(buttons);
    if (NativeCSS() && state.phase == LocalTeams::Phase::Selecting)
      for (unsigned i = 0; i < state.count; ++i)
        if (buttons[state.ports[i]] & 0x200) state.UnreadyPlayer(i);
    if (returnedFromGame) published = false;
  }
  result = {{1, u8(state.count), u8(state.active), u8(state.ports[state.active]), u8(state.confirmed),
             u8(state.phase), u8(state.startArmed), u8(state.clearToken), u8(loopback)}};
  result[9] = u8(NativeCSS()); result[10] = u8(state.nativeArmed); result[11] = u8(state.nativeHeld);
  for (unsigned i = 0; i < 4; ++i)
  {
    result[12 + i] = u8(state.ports[i]);
    result[16 + 4*i] = state.selections[i].character;
    result[17 + 4*i] = state.selections[i].costume;
    result[18 + 4*i] = state.selections[i].team;
    result[19 + 4*i] = u8((state.saved >> i) & 1);
  }
  state.clearToken = false;
  ShowStatus();
  return result;
}

bool LocalTeamsCoordinator::Confirm(const u8* payload, u32 rng)
{
  if (!Active()) return false;
  LocalTeams::Selection selection;
  selection.team = payload[0]; selection.character = payload[1]; selection.costume = payload[2];
  selection.altStage = payload[8]; selection.rng = rng;
  if (payload[7] & 0x80)
  {
    if (!NativeCSS() || !state.ConfirmPlayer(payload[7] & 3, selection, payload[3] == 1)) return false;
  }
  else if (!state.Confirm(selection, payload[3] != 0)) return false;
  if (state.AllConfirmed() && !loopback && !validated) state.phase = LocalTeams::Phase::EnteringCode;
  ShowStatus();
  return true;
}

bool LocalTeamsCoordinator::ChangeCount(const u8* payload)
{
  // A connected rematch also uses Selecting: validated guards its live roster.
  const unsigned previous = state.count;
  if (!Active() || !NativeCSS() || validated || !state.Resize(payload[0])) return false;
  for (unsigned i = 0; i < previous; ++i)
  {
    const u8* pick = payload + 1 + 4 * i;
    if (pick[3] && pick[0] < 26 && pick[2] <= 2)
    {
      state.selections[i].character = pick[0];
      state.selections[i].costume = pick[1];
      state.selections[i].team = pick[2];
      state.saved |= 1u << i;
    }
    else state.saved &= ~(1u << i);
  }
  published = false;
  ShowStatus();
  return true;
}

SlippiPlayerSelections LocalTeamsCoordinator::Selection(unsigned index) const
{
  const auto& value = state.selections[index];
  SlippiPlayerSelections result;
  result.characterId = value.character; result.characterColor = value.costume;
  result.teamId = value.team; result.alt_stage_mode = value.altStage;
  result.rngOffset = value.rng;
  result.isCharacterSelected = true;
  result.stageId = 0x1F; result.isStageSelected = true;
  return result;
}
SlippiPlayerSelections LocalTeamsCoordinator::PrimarySelection() const { return Selection(0); }

std::string LocalTeamsCoordinator::SearchCode() const
{
  // Match the stock connect-code UI's full-width Shift-JIS hashtag.
  return ReplaceAll(UTF8ToSHIFTJIS(lobbyCode), "#", std::string("\x81\x94", 2));
}

void LocalTeamsCoordinator::SetSearchCode(const std::string& shiftJisCode)
{
  // The native Slippi code-entry callback supplies the same Shift-JIS payload
  // that stock matchmaking uses, including its full-width hashtag.
  lobbyCode = SHIFTJISToUTF8(shiftJisCode);
}

SlippiUser::UserInfo LocalTeamsCoordinator::Identity(unsigned index, const SlippiUser::UserInfo& fallback)
{
  if (identityFiles[index].empty()) return fallback;
  std::ifstream input(identityFiles[index]);
  if (!input) throw std::runtime_error("Could not open configured identity " + std::to_string(index + 1));
  json value;
  input >> value;
  auto identity = fallback;
  identity.uid = value.at("uid").get<std::string>();
  identity.playKey = value.at("playKey").get<std::string>();
  identity.displayName = value.at("displayName").get<std::string>();
  identity.connectCode = value.at("connectCode").get<std::string>();
  return identity;
}

bool LocalTeamsCoordinator::BeginSearch(std::unique_ptr<SlippiMatchmaking>& primary)
{
  if (!Active() || !state.AllConfirmed()) return false;
  if (loopback)
  {
    validated = published = false;
    primary = std::make_unique<SlippiMatchmaking>(device, user);
    primary->SetupLocalTeamsLoopback(0, u16(basePort));
    for (unsigned i = 1; i < 4; ++i)
    {
      guests[i - 1].matchmaking = std::make_unique<SlippiMatchmaking>(device, user);
      guests[i - 1].matchmaking->SetupLocalTeamsLoopback(u8(i), u16(basePort));
    }
    return true;
  }
  if (SearchCode().empty() || SearchCode().size() > 18)
  { Fail("Enter a room code of at most 18 Shift-JIS bytes"); return false; }
  const auto fallback = user->GetUserInfo();
  // Read every identity before starting any thread. Never log play keys.
  std::array<SlippiUser::UserInfo, 4> identities;
  try
  {
    for (unsigned i = 0; i < state.count; ++i)
    {
      identities[i] = Identity(i, fallback);
      if (identities[i].uid.empty() || identities[i].playKey.empty()) throw std::runtime_error("Log in before searching");
    }
  }
  catch (const std::exception& ex) { Fail(ex.what()); return false; }
  validated = published = false;
  state.phase = LocalTeams::Phase::Searching;
  state.startArmed = false;
  state.nativeArmed = state.nativeHeld = 0;
  ShowStatus();
  SlippiMatchmaking::MatchSearchSettings search;
  search.mode = SlippiMatchmaking::TEAMS;
  search.connectCode = SearchCode();
  primary = std::make_unique<SlippiMatchmaking>(device, user, u16(basePort), &identities[0]);
  for (unsigned i = 1; i < state.count; ++i)
    guests[i - 1].matchmaking = std::make_unique<SlippiMatchmaking>(device, user, u16(basePort + i), &identities[i]);
  primary->FindMatch(search);
  for (unsigned i = 1; i < state.count; ++i) guests[i - 1].matchmaking->FindMatch(search);
  return true;
}

SlippiMatchmaking::ProcessState LocalTeamsCoordinator::Tick(SlippiMatchmaking& primary, SlippiNetplayClient* netplay)
{
  using MM = SlippiMatchmaking;
  if (!Active()) return primary.GetMatchmakeState();
  if (state.phase == LocalTeams::Phase::Error) return MM::ERROR_ENCOUNTERED;
  if (primary.GetMatchmakeState() == MM::IDLE) return MM::IDLE;
  std::array<SlippiMatchmaking*, 4> clients{{&primary, nullptr, nullptr, nullptr}};
  for (unsigned i = 1; i < state.count; ++i) clients[i] = guests[i - 1].matchmaking.get();
  bool complete = true;
  auto progress = MM::CONNECTION_SUCCESS;
  for (unsigned i = 0; i < state.count; ++i)
  {
    if (!clients[i]) { Fail("A local session is missing; reset with Z"); return MM::ERROR_ENCOUNTERED; }
    const auto status = clients[i]->GetMatchmakeState();
    if (status == MM::ERROR_ENCOUNTERED)
    { Fail("Player " + std::to_string(i + 1) + ": " + clients[i]->GetErrorMessage()); return MM::ERROR_ENCOUNTERED; }
    complete &= status == MM::CONNECTION_SUCCESS;
    if (status < progress) progress = status;
  }
  if (!complete) return progress; // preserve stock Searching -> Connecting UI states
  if (!validated)
  {
    const auto reference = primary.GetMatchmakeResult();
    unsigned assigned = 0;
    for (unsigned i = 0; i < state.count; ++i)
    {
      const auto result = clients[i]->GetMatchmakeResult();
      const int slot = clients[i]->LocalPlayerIndex();
      if (slot < 0 || slot > 3 || (assigned & (1u << slot)) || result.id != reference.id || result.players.size() != 4)
      { Fail("Sessions received different lobbies or duplicate slots"); return MM::ERROR_ENCOUNTERED; }
      assigned |= 1u << slot;
      localSlots[i] = unsigned(slot);
      std::array<std::string, 4> a{}, b{};
      for (const auto& player : reference.players)
      {
        if (player.port < 1 || player.port > 4) { Fail("Invalid primary roster"); return MM::ERROR_ENCOUNTERED; }
        a[player.port - 1] = player.uid + ":" + player.connectCode;
      }
      for (const auto& player : result.players)
      {
        if (player.port < 1 || player.port > 4) { Fail("Invalid roster"); return MM::ERROR_ENCOUNTERED; }
        b[player.port - 1] = player.uid + ":" + player.connectCode;
      }
      if (!LocalTeams::CompatibleRoster(a, b)) { Fail("Local sessions disagree about the roster"); return MM::ERROR_ENCOUNTERED; }
      INFO_LOG(SLIPPI_ONLINE, "[LocalTeams] local %u controller %u -> Melee slot %d", i + 1, state.ports[i] + 1, slot + 1);
    }
    for (unsigned i = 1; i < state.count; ++i) guests[i - 1].netplay = clients[i]->GetNetplayClient();
    validated = true;
    state.phase = LocalTeams::Phase::Waiting;
    ShowStatus();
  }
  for (unsigned i = 0; i < state.count; ++i)
  {
    auto* client = i ? guests[i - 1].netplay.get() : netplay;
    if (!client && i == 0) continue; // Primary ownership transfers just after initial Tick.
    if (!client) { Fail("Local P" + std::to_string(i + 1) + " session is missing"); return MM::ERROR_ENCOUNTERED; }
    const auto activePlayers = client->GetActivePlayerIndices();
    if (activePlayers.size() == 3 && client->GetSlippiConnectStatus() == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED) continue;
    for (unsigned slot = 0; slot < 4; ++slot)
    {
      if (slot == localSlots[i] || activePlayers.count(u8(slot))) continue;
      const auto label = primary.GetPlayerName(u8(slot));
      bool isLocal = false;
      for (unsigned local = 0; local < state.count; ++local) isLocal |= localSlots[local] == slot;
      const auto detail = "Connection to " + (label.empty() ? "player " + std::to_string(slot + 1) : label) + " ended";
      WARN_LOG(SLIPPI_ONLINE, "[LocalTeams] local %u observed slot %u missing; status %u, reason %u",
               i + 1, slot + 1, unsigned(client->GetSlippiConnectStatus()), unsigned(client->GetDisconnectReason()));
      if (isLocal) { Fail(detail + " on Local P" + std::to_string(i + 1)); return MM::ERROR_ENCOUNTERED; }
      // Stock Teams closes the lobby after a participant leaves, including
      // after a game finishes with someone disconnected. This is not a local fault.
      lobbyDisconnected = true;
      OSD::AddMessage(detail + "; lobby closed", 8000, OSD::Color::YELLOW);
      return MM::IDLE;
    }
    Fail("Local P" + std::to_string(i + 1) + " network session ended");
    return MM::ERROR_ENCOUNTERED;
  }
  if (netplay && !published && state.AllConfirmed()) PublishSelections(*netplay);
  return MM::CONNECTION_SUCCESS;
}

void LocalTeamsCoordinator::PublishSelections(SlippiNetplayClient& primary)
{
  if (!validated || published || !state.AllConfirmed()) return;
  auto selection = Selection(0);
  primary.SetMatchSelections(selection);
  for (unsigned i = 1; i < state.count; ++i)
  { selection = Selection(i); guests[i - 1].netplay->SetMatchSelections(selection); }
  published = true;
  state.phase = LocalTeams::Phase::Waiting;
}

bool LocalTeamsCoordinator::AllReady(SlippiNetplayClient& primary)
{
  if (!published || !state.AllConfirmed()) return false;
  for (unsigned i = 0; i < state.count; ++i)
  {
    auto* client = i == 0 ? &primary : guests[i - 1].netplay.get();
    if (!client || client->GetActivePlayerIndices().size() != 3) return false;
    auto info = client->GetMatchInfoSnapshot();
    if (!info.localPlayerSelections.isCharacterSelected) return false;
    for (const auto& selection : info.remotePlayerSelections)
      if (!selection.isCharacterSelected) return false;
  }
  return true;
}

void LocalTeamsCoordinator::StartGame()
{
  state.phase = LocalTeams::Phase::InGame;
  state.lastSentFrame = 0;
  for (unsigned i = 1; i < state.count; ++i)
  { guests[i - 1].previous.fill(0); guests[i - 1].netplay->StartSlippiGame(); }
}

void LocalTeamsCoordinator::SendInputs(s32 frame, u8 delay, s32 finalized, u32 checksum, const u8* reports, bool skip)
{
  if (!Active() || state.phase != LocalTeams::Phase::InGame) return;
  if (!skip && !state.AcceptFrame(frame)) skip = true;
  for (unsigned i = 1; i < state.count; ++i)
  {
    auto& guest = guests[i - 1];
    guest.netplay->DropOldRemoteInputs(finalized);
    if (skip) { guest.netplay->SendSlippiPad(nullptr); continue; }
    if (frame == 1)
      for (s32 initial = 1; initial <= delay; ++initial) guest.netplay->SendSlippiPad(std::make_unique<SlippiPad>(initial));
    LocalTeams::Pad pad{};
    std::copy_n(reports + 12 * state.ports[i], 12, pad.begin());
    pad = LocalTeams::NormalizePad(pad, guest.previous);
    guest.netplay->SendSlippiPad(std::make_unique<SlippiPad>(frame + delay, finalized, checksum, pad.data()));
  }
}

void LocalTeamsCoordinator::Cleanup()
{
  RequestStop();
  // Join all workers before the EXI/Rust user owner is destroyed or ports reused.
  for (auto& guest : guests) { guest.matchmaking.reset(); guest.netplay.reset(); guest.previous.fill(0); }
  state.ResetSelection();
  validated = published = false;
  lobbyDisconnected = false;
  localSlots.fill(4);
  error.clear(); lastStatus.clear();
}

void LocalTeamsCoordinator::RequestStop()
{
  for (auto& guest : guests)
  {
    if (guest.matchmaking) guest.matchmaking->RequestStop();
    if (guest.netplay) guest.netplay->RequestStop();
  }
}
