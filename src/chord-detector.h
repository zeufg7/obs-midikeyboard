#pragma once

#include "midi-state.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Structural description of a chord relative to its root. Every pitch of the
// voicing is explained by exactly one of these functions, so any combination of
// third, fifth, seventh, tensions and alterations can be named.
enum class ChordThird : uint8_t { Major, Minor, Sus2, Sus4, None };
enum class ChordFifth : uint8_t { Perfect, Flat, Sharp, None };
enum class ChordSeventh : uint8_t { None, Minor, Major, Diminished };

namespace chord_ext {
constexpr uint8_t flat9 = 1u << 0;
constexpr uint8_t nine = 1u << 1;
constexpr uint8_t sharp9 = 1u << 2;
constexpr uint8_t eleven = 1u << 3;
constexpr uint8_t sharp11 = 1u << 4;
constexpr uint8_t flat13 = 1u << 5;
constexpr uint8_t thirteen = 1u << 6; // also the "6" of a sixth chord when there is no seventh
} // namespace chord_ext

struct ChordSpec {
	ChordThird third = ChordThird::Major;
	ChordFifth fifth = ChordFifth::Perfect;
	ChordSeventh seventh = ChordSeventh::None;
	uint8_t extensions = 0;
	bool power = false;

	bool operator==(const ChordSpec &) const = default;
};

struct ChordCandidate {
	int root = 0;
	int bass = 0;
	ChordSpec spec;
	std::string suffix;         // canonical international suffix, e.g. "m7b5", "13#11"
	bool omitted_fifth = false; // the perfect fifth is absent
	bool foreign_bass = false;  // the bass is not part of the chord (e.g. D/C over a triad)
	int score = 0;

	bool same_chord(const ChordCandidate &other) const;
};

enum class ChordStyle : int { International = 0, Brazilian = 1, Jazz = 2 };
enum class ChordAccidentals : int { Auto = 0, Sharps = 1, Flats = 2 };

struct ChordFormat {
	bool show_bass = true;
	ChordAccidentals accidentals = ChordAccidentals::Sharps;
	ChordStyle style = ChordStyle::International;
	bool symbols = false; // use ♭ ♯ instead of b #
};

std::vector<ChordCandidate> detect_chords(std::span<const MidiNote> notes,
					  const std::optional<ChordCandidate> &previous = std::nullopt);
std::string format_chord(const ChordCandidate &chord, const ChordFormat &format);
std::string format_chord(const ChordCandidate &chord, bool show_bass = true, bool prefer_flats = false);

class ChordTracker {
public:
	void configure(uint64_t settle_ms, uint64_t clear_ms);
	void reset();
	const std::optional<ChordCandidate> &update(std::span<const MidiNote> notes, uint64_t now_ms);
	const std::optional<ChordCandidate> &current() const { return current_; }

private:
	std::optional<ChordCandidate> current_;
	std::optional<ChordCandidate> pending_;
	uint16_t last_mask_ = 0;
	int last_bass_ = -1;
	bool initialized_ = false;
	bool pending_active_ = false;
	bool clearing_ = false;
	uint64_t settle_ms_ = 70;
	uint64_t clear_ms_ = 180;
	uint64_t pending_since_ = 0;
	uint64_t changed_at_ = 0;
	uint64_t clear_since_ = 0;
};
