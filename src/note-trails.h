#pragma once

#include "midi-state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// One floating note ("trail") that rises from the keyboard. Times are in seconds
// on the caller's clock. While the key is held the trail is open and its bottom
// edge stays glued to the keyboard; after release it detaches and keeps rising.
struct NoteTrail {
	int note = 0;
	uint8_t velocity = 0;
	double start = 0.0;
	double end = 0.0;
	bool open = true;
};

// Turns snapshots of held notes into a list of trails. Pure logic with no OBS or
// MIDI backend dependencies so it can be unit tested.
class NoteTrails {
public:
	static constexpr std::size_t max_trails = 4096;

	// notes: currently held notes (any channel). now: current time in seconds.
	// max_age: seconds after release before a closed trail is discarded.
	void update(const std::vector<MidiNote> &notes, double now, double max_age);
	void clear();
	const std::vector<NoteTrail> &trails() const { return trails_; }

private:
	std::vector<NoteTrail> trails_;
	std::array<uint64_t, 128> open_onset_{};
	std::array<bool, 128> is_open_{};
};
