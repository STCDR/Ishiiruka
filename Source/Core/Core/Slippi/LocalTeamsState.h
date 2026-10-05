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
constexpr unsigned PollPayloadSize = 9;  // mode + four big-endian button words
constexpr unsigned ConfirmPayloadSize = 9;
constexpr unsigned InputsPayloadSize = 73; // stock 25 + four raw PAD reports
constexpr unsigned CountPayloadSize = 17; // requested count + four character/costume/team/placed tuples
constexpr unsigned StatusSize = 32; // legacy nine fields, native CSS flags/ports, four saved picks

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
  std::int32_t lastSentFrame = 0;

  bool Valid() const
  {
    if (count < 1 || count > 4 || ports[0] != 0) return false;
    unsigned mask = 0;
    for (unsigned i = 0; i < count; ++i)
    {
      if (ports[i] > 3 || (mask & (1u << ports[i]))) return false;
      mask |= 1u << ports[i];
    }
    return true;
  }
  bool AllConfirmed() const { return confirmed == (1u << count) - 1; }
  bool Resize(unsigned requested)
  {
    if (phase != Phase::Selecting || requested < 1 || requested > 4 || requested == count) return false;
    const unsigned previous = count;
    count = requested;
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
  }
  void Poll(const std::array<std::uint16_t, 4>& buttons)
  {
    if (phase == Phase::InGame) ResetSelection();
    if ((phase == Phase::Selecting || phase == Phase::EnteringCode) && !(buttons[ports[active]] & 0x1000)) startArmed = true;
    nativeHeld = 0;
    for (unsigned i = 0; i < count; ++i)
    {
      const unsigned bit = 1u << i;
      if (buttons[ports[i]] & 0x1000) nativeHeld |= bit;
      else if (phase == Phase::Selecting || phase == Phase::EnteringCode) nativeArmed |= bit;
    }
  }
  bool Confirm(const Selection& selection, bool selected)
  {
    if (phase != Phase::Selecting || !startArmed || !selected || selection.character >= 26 || selection.team > 2)
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
    if (player >= count || phase != Phase::Selecting || !selected ||
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
