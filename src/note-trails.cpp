#include "note-trails.h"

#include <algorithm>

void NoteTrails::update(const std::vector<MidiNote> &notes, double now, double max_age)
{
	// Collapse channels: a pitch is held if any channel holds it; the most recent
	// onset wins so a re-press on any channel starts a new trail.
	std::array<bool, 128> held{};
	std::array<uint64_t, 128> onset{};
	std::array<uint8_t, 128> velocity{};
	for (const auto &n : notes) {
		if (n.note < 0 || n.note > 127)
			continue;
		const auto p = static_cast<std::size_t>(n.note);
		if (!held[p] || n.onset_ms >= onset[p]) {
			onset[p] = n.onset_ms;
			velocity[p] = n.velocity;
		}
		held[p] = true;
	}

	// Close trails whose key was released or re-struck.
	for (auto &trail : trails_) {
		if (!trail.open)
			continue;
		const auto p = static_cast<std::size_t>(trail.note);
		if (!held[p] || onset[p] != open_onset_[p]) {
			trail.open = false;
			trail.end = now;
			is_open_[p] = false;
		}
	}

	// Open trails for newly held keys.
	for (std::size_t p = 0; p < 128; ++p) {
		if (!held[p] || is_open_[p])
			continue;
		trails_.push_back({static_cast<int>(p), velocity[p], now, now, true});
		is_open_[p] = true;
		open_onset_[p] = onset[p];
	}

	// Drop trails that have floated off the top.
	std::erase_if(trails_, [&](const NoteTrail &t) { return !t.open && now - t.end > max_age; });

	// Safety cap: discard the oldest closed trails first.
	if (trails_.size() > max_trails) {
		std::size_t excess = trails_.size() - max_trails;
		std::erase_if(trails_, [&](const NoteTrail &t) {
			if (excess == 0 || t.open)
				return false;
			--excess;
			return true;
		});
	}
}

void NoteTrails::clear()
{
	trails_.clear();
	is_open_ = {};
	open_onset_ = {};
}
