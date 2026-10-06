// Local-only setup state. Never restored by Melee rollback.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace LocalTeams
{
constexpr std::uint8_t PollCommand = 0xC5;
constexpr std::uint8_t ConfirmCommand = 0xC6;
constexpr std::uint8_t InputsCommand = 0xC7;
constexpr std::uint8_t CountCommand = 0xC8;
constexpr unsigned PollPayloadSize = 62; // mode, buttons, entering port, four PAD reports, CSS frame
constexpr unsigned ConfirmPayloadSize = 9;
constexpr unsigned InputsPayloadSize = 73; // stock 25 + four raw PAD reports
constexpr unsigned CountPayloadSize = 17; // requested count + four character/costume/team/placed tuples
constexpr unsigned StatusSize = 36; // saved picks plus joined/suppressed local masks
constexpr unsigned RosterHoldFrames = 31; // match the native CSS B hold

enum class Phase : std::uint8_t { Selecting, Searching, Waiting, InGame, Error, EnteringCode };

struct Selection
{
  std::uint8_t character = 0, costume = 0, team = 0, altStage = 0;
  std::uint32_t rng = 0;
};

class State
{
public:
  unsigned count = 2;
  std::array<unsigned, 4> ports{{0, 1, 2, 3}};
  std::array<Selection, 4> selections{};
  Phase phase = Phase::Selecting;
  unsigned active = 0, confirmed = 0;
  bool startArmed = false, clearToken = false;
  unsigned saved = 0, nativeArmed = 0, nativeHeld = 0;
  bool joining = false;
  unsigned joined = 15, joinArmed = 0, suppressed = 0;
  std::array<unsigned,4> joinPreviousButtons{};
  std::array<unsigned,4> joinStartHold{}, leaveHold{};
  unsigned rosterAction = 0, rosterCount = 0;
  std::uint32_t lastJoinFrame = UINT32_MAX;
  std::int32_t lastSentFrame = 0;

  bool Valid() const
  {
    if (count < 1 || count > 4 || !Claimed(0)) return false;
    unsigned mask = 0;
    for (unsigned i = 0; i < count; ++i)
    {
      if (!Claimed(i)) { if (ports[i] != 4) return false; continue; }
      if (ports[i] > 3 || (mask & (1u << ports[i]))) return false;
      mask |= 1u << ports[i];
    }
    return true;
  }
  bool Claimed(unsigned player) const { return player < count && (!joining || (joined & (1u << player))); }
  unsigned JoinedMask() const { return (joining ? joined : 15u) & ((1u << count) - 1); }
  bool AllConfirmed() const { return JoinedMask() == (1u << count) - 1 && confirmed == JoinedMask(); }
  void BeginJoining(unsigned primary)
  {
    if (primary > 3) return;
    joining = true; joined = 1; joinArmed = 0; suppressed = 1u << primary;
    joinPreviousButtons.fill(0); lastJoinFrame = UINT32_MAX;
    joinStartHold.fill(0); leaveHold.fill(0); rosterAction = 0;
    ports.fill(4); ports[0] = primary;
    saved = 0; selections = {};
    ResetSelection();
  }
  // PAD reports contain signed GC stick coordinates (-128..127).
  // 51 is the first whole coordinate at 40% of the full stick range.
  static bool Neutral(const std::uint8_t* report)
  {
    if (report[10] != 0 || report[0] || report[1]) return false;
    for (unsigned axis = 2; axis < 6; ++axis)
    {
      const int value = static_cast<std::int8_t>(report[axis]);
      if (value < -20 || value > 20) return false;
    }
    return true;
  }
  void PollJoining(const std::uint8_t* reports) { PollJoining(reports, lastJoinFrame + 1); }
  void PollJoining(const std::uint8_t* reports, std::uint32_t frame, bool rosterAllowed = true)
  {
    if (!joining || !reports) return;
    if (frame == lastJoinFrame) return;
    if (frame - lastJoinFrame != 1)
    { joinPreviousButtons.fill(0); joinStartHold.fill(0); leaveHold.fill(0); }
    lastJoinFrame = frame;
    for (unsigned port = 0; port < 4; ++port)
    {
      const auto* report = reports + 12 * port;
      const unsigned bit = 1u << port;
      const unsigned buttons = ((unsigned(report[0]) << 8) | report[1]) & 0x1F7F;
      const unsigned held = buttons & joinPreviousButtons[port];
      joinPreviousButtons[port] = report[10] == 0 ? buttons : 0;
      if (Neutral(report)) joinArmed |= bit;
      // Consume joining buttons until release, independently of stick movement.
      if (report[10] == 0 && !report[0] && !report[1]) suppressed &= ~bit;
      unsigned owner = 4;
      for (unsigned i = 0; i < count; ++i) if (Claimed(i) && ports[i] == port) owner = i;
      // The main controller can leave CSS while searching or in a lobby,
      // just as in stock Slippi. This does not change a live lobby's roster.
      if (owner == 0 && phase != Phase::InGame && report[10] == 0)
      {
        joinStartHold[port] = 0;
        leaveHold[port] = InputEnabled(owner) && (buttons & 0x200) ? leaveHold[port] + 1 : 0;
        if (leaveHold[port] >= RosterHoldFrames && !rosterAction)
        { rosterAction = 0x40; rosterCount = 0; }
        continue;
      }
      if (phase != Phase::Selecting || report[10] != 0 || !rosterAllowed)
      { joinStartHold[port] = leaveHold[port] = 0; continue; }
      if (owner < 4)
      {
        joinStartHold[port] = 0;
        leaveHold[port] = InputEnabled(owner) && (buttons & 0x200) ? leaveHold[port] + 1 : 0;
        if (leaveHold[port] >= RosterHoldFrames && !rosterAction)
        { rosterAction = 0x40 | owner; rosterCount = owner == 0 ? 0 : count - 1; }
        continue;
      }
      leaveHold[port] = 0;
      if (!(joinArmed & bit)) { joinStartHold[port] = 0; continue; }
      joinStartHold[port] = (buttons & 0x1000) ? joinStartHold[port] + 1 : 0;
      if (JoinedMask() == (1u << count) - 1 && count < 4 && joinStartHold[port] >= RosterHoldFrames && !rosterAction)
      { rosterAction = 0x80 | port; rosterCount = count + 1; }
      bool moved = false;
      for (unsigned axis = 2; axis < 6; ++axis)
      {
        const int value = static_cast<std::int8_t>(report[axis]);
        moved |= value <= -51 || value >= 51;
      }
      if (rosterAction || (!held && !moved)) continue; // buttons require two consecutive CSS frames
      for (unsigned i = 1; i < count; ++i)
        if (!Claimed(i))
        {
          ports[i] = port; joined |= 1u << i; suppressed |= bit; joinArmed &= ~bit;
          saved &= ~(1u << i); selections[i] = {};
          nativeArmed &= ~(1u << i);
          joinStartHold[port] = 0;
          break;
        }
    }
  }
  bool InputEnabled(unsigned player) const
  {
    return Claimed(player) && !(suppressed & (1u << ports[player]));
  }
  void ClaimAddedPort(unsigned port)
  {
    ports[count-1] = port; joined |= 1u << (count-1); suppressed |= 1u << port;
  }
  void RemovePlayer(unsigned player)
  {
    const unsigned port = ports[player];
    for (unsigned i = player; i + 1 < count; ++i) { ports[i] = ports[i+1]; selections[i] = selections[i+1]; }
    const unsigned low = (1u << player) - 1;
    saved = (saved & low) | ((saved >> 1) & ~low);
    joined = (joined & low) | ((joined >> 1) & ~low);
    --count; ports[count] = 4; selections[count] = {};
    suppressed &= ~(1u << port); joinArmed &= ~(1u << port);
    ResetSelection();
  }
  unsigned SuppressedMask() const
  {
    unsigned mask = 0;
    for (unsigned i = 0; i < count; ++i) if (Claimed(i) && !InputEnabled(i)) mask |= 1u << i;
    return mask;
  }
  bool Resize(unsigned requested)
  {
    if (phase != Phase::Selecting || requested < 1 || requested > 4 || requested == count) return false;
    const unsigned previous = count;
    count = requested;
    if (joining)
    {
      joined &= (1u << count) - 1; saved &= joined; joinArmed = 0;
      joinPreviousButtons.fill(0); lastJoinFrame = UINT32_MAX;
      for (unsigned i = count; i < 4; ++i) { ports[i] = 4; selections[i] = {}; }
    }
    if (!Valid()) { count = previous; return false; }
    ResetSelection();
    return true;
  }
  void ResetSelection()
  {
    phase = Phase::Selecting;
    active = confirmed = 0;
    startArmed = clearToken = false;
    nativeArmed = nativeHeld = 0;
    lastSentFrame = 0;
    joinStartHold.fill(0); leaveHold.fill(0); rosterAction = 0;
  }
  void ClearCards()
  {
    saved = 0;
    selections = {};
    ResetSelection();
  }
  void Poll(const std::array<std::uint16_t, 4>& buttons)
  {
    if (phase == Phase::InGame) ResetSelection();
    if ((phase == Phase::Selecting || phase == Phase::EnteringCode) && InputEnabled(active) && !(buttons[ports[active]] & 0x1000)) startArmed = true;
    nativeHeld = 0;
    for (unsigned i = 0; i < count; ++i)
    {
      const unsigned bit = 1u << i;
      if (!InputEnabled(i)) { nativeArmed &= ~bit; continue; }
      if (buttons[ports[i]] & 0x1000) nativeHeld |= bit;
      else if (phase == Phase::Selecting || phase == Phase::EnteringCode) nativeArmed |= bit;
    }
  }
  bool Confirm(const Selection& selection, bool selected)
  {
    if (phase != Phase::Selecting || !InputEnabled(active) || !startArmed || !selected || selection.character >= 26 || selection.team > 2)
      return false;
    selections[active] = selection;
    saved |= 1u << active;
    confirmed |= 1u << active;
    startArmed = false;
    if (AllConfirmed()) phase = Phase::Searching;
    else { ++active; clearToken = true; }
    return true;
  }
  bool ConfirmPlayer(unsigned player, const Selection& selection, bool selected)
  {
    if (!InputEnabled(player) || phase != Phase::Selecting || !selected ||
        !(nativeArmed & (1u << player)) || (confirmed & (1u << player)) ||
        selection.character >= 26 || selection.team > 2) return false;
    selections[player] = selection;
    saved |= 1u << player;
    confirmed |= 1u << player;
    nativeArmed &= ~(1u << player);
    if (AllConfirmed()) phase = Phase::Searching;
    return true;
  }
  void UnreadyPlayer(unsigned player)
  {
    if (player < count && phase == Phase::Selecting) confirmed &= ~(1u << player);
  }
  bool AcceptFrame(std::int32_t frame)
  {
    if (frame <= lastSentFrame) return false;
    lastSentFrame = frame;
    return true;
  }
};

// Identity is keyed by assigned Melee slot, allowing repeated UIDs/usernames.
inline bool CompatibleRoster(const std::array<std::string, 4>& primary,
                             const std::array<std::string, 4>& other)
{
  return primary == other;
}

using Pad = std::array<std::uint8_t, 12>;
inline Pad NormalizePad(const Pad& current, Pad& previous)
{
  Pad result = current;
  if (static_cast<std::int8_t>(current[10]) == -3) result = previous;
  // Match TriggerSendInput's stick-at-rest fix for both sticks, signed GC values.
  for (unsigned i = 2; i < 6; i += 2)
  {
    const auto x = static_cast<std::int8_t>(result[i]);
    const auto y = static_cast<std::int8_t>(result[i + 1]);
    if (x >= -2 && x <= 2 && y >= -2 && y <= 2) result[i] = result[i + 1] = 0;
  }
  previous = result;
  return result;
}
}
