#include "note-trails.h"

#include <iostream>
#include <stdexcept>

namespace {

size_t checks = 0;

void require(bool condition, const char *message)
{
	++checks;
	if (!condition)
		throw std::runtime_error(message);
}

MidiNote held(int note, uint64_t onset, uint8_t velocity = 100, int channel = 0)
{
	return {note, velocity, channel, true, onset};
}

void press_hold_release()
{
	NoteTrails t;
	t.update({held(60, 1)}, 0.0, 2.0);
	require(t.trails().size() == 1, "press opens one trail");
	require(t.trails()[0].open && t.trails()[0].note == 60, "trail is open for C4");
	t.update({held(60, 1)}, 0.5, 2.0);
	require(t.trails().size() == 1 && t.trails()[0].open, "holding keeps trail open");
	t.update({}, 1.0, 2.0);
	require(!t.trails()[0].open && t.trails()[0].end == 1.0, "release closes trail");
	t.update({}, 2.9, 2.0);
	require(t.trails().size() == 1, "trail kept while on screen");
	t.update({}, 3.1, 2.0);
	require(t.trails().empty(), "trail removed after leaving screen");
}

void repress_starts_new_trail()
{
	NoteTrails t;
	t.update({held(64, 10)}, 0.0, 5.0);
	t.update({held(64, 20)}, 0.2, 5.0);
	require(t.trails().size() == 2, "re-press creates a second trail");
	require(!t.trails()[0].open && t.trails()[1].open, "old closed, new open");
}

void channels_merge()
{
	NoteTrails t;
	t.update({held(48, 5, 80, 0), held(48, 5, 90, 1)}, 0.0, 5.0);
	require(t.trails().size() == 1, "same pitch on two channels is one trail");
	t.update({held(48, 5, 80, 0), held(48, 5, 90, 1), held(52, 6)}, 0.1, 5.0);
	require(t.trails().size() == 2, "chord adds a trail per pitch");
}

void cap_and_clear()
{
	NoteTrails t;
	double now = 0.0;
	for (int i = 0; i < 5000; ++i) {
		t.update({held(60, static_cast<uint64_t>(i + 1))}, now, 1e9);
		now += 0.001;
	}
	require(t.trails().size() <= NoteTrails::max_trails, "trail count is capped");
	require(t.trails().back().open, "newest trail survives the cap");
	t.clear();
	require(t.trails().empty(), "clear empties trails");
	t.update({held(60, 1)}, now, 1.0);
	require(t.trails().size() == 1, "after clear a held note opens again");
}

} // namespace

int main()
{
	try {
		press_hold_release();
		repress_starts_new_trail();
		channels_merge();
		cap_and_clear();
	} catch (const std::exception &e) {
		std::cerr << "FAILED: " << e.what() << '\n';
		return 1;
	}
	std::cout << "note-trails-tests: " << checks << " checks passed\n";
	return 0;
}
