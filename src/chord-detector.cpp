#include "chord-detector.h"

#include <algorithm>
#include <array>
#include <bit>
#include <string>
#include <string_view>

namespace {

using namespace chord_ext;

struct NoteSet {
	uint16_t mask = 0;
	int bass = -1;
	int bass_count = 0; // how many held notes share the bass pitch class
};

NoteSet note_set(std::span<const MidiNote> notes)
{
	NoteSet result;
	int lowest = 128;
	for (const auto &note : notes) {
		if (note.note < 0 || note.note > 127)
			continue;
		result.mask |= static_cast<uint16_t>(1u << (note.note % 12));
		lowest = std::min(lowest, note.note);
	}
	if (lowest == 128)
		return result;
	result.bass = lowest % 12;
	for (const auto &note : notes)
		if (note.note >= 0 && note.note <= 127 && note.note % 12 == result.bass)
			++result.bass_count;
	return result;
}

uint16_t rotate(uint16_t mask, int root)
{
	uint16_t relative = 0;
	for (int pitch = 0; pitch < 12; ++pitch)
		if (mask & (1u << pitch))
			relative |= static_cast<uint16_t>(1u << ((pitch - root + 12) % 12));
	return relative;
}

struct Analysis {
	ChordSpec spec;
	int score = 0;
};

// Explains every interval of a root-relative pitch set as a chord function.
// Returns nothing when the set cannot be read as a chord built on this root.
std::optional<Analysis> analyze(uint16_t rel)
{
	auto has = [&](int interval) {
		return (rel >> interval) & 1u;
	};
	if (!has(0))
		return std::nullopt;
	const int count = std::popcount(static_cast<unsigned int>(rel));

	ChordSpec s;
	if (rel == ((1u << 0) | (1u << 7))) {
		s.power = true;
		s.third = ChordThird::None;
		return Analysis{s, 96};
	}
	if (count < 3 || (has(10) && has(11)))
		return std::nullopt;

	uint16_t used = 1;
	auto use = [&](int interval) {
		used |= static_cast<uint16_t>(1u << interval);
	};

	// Third (or its suspension).
	if (has(4)) {
		s.third = ChordThird::Major;
		use(4);
		if (has(3)) {
			s.extensions |= sharp9;
			use(3);
		}
	} else if (has(3)) {
		s.third = ChordThird::Minor;
		use(3);
	} else if (has(5)) {
		s.third = ChordThird::Sus4;
		use(5);
	} else if (has(2)) {
		s.third = ChordThird::Sus2;
		use(2);
	} else {
		s.third = ChordThird::None;
	}
	const bool tertian = s.third == ChordThird::Major || s.third == ChordThird::Minor;

	// Fifth.
	if (has(7)) {
		s.fifth = ChordFifth::Perfect;
		use(7);
	} else if (has(6) && tertian && !(s.third == ChordThird::Major && (has(2) || has(9) || has(11)))) {
		// With a major third plus 9, 13 or maj7 the tritone reads as #11, not b5.
		s.fifth = ChordFifth::Flat;
		use(6);
	} else if (has(8) && s.third == ChordThird::Major) {
		s.fifth = ChordFifth::Sharp;
		use(8);
	} else {
		s.fifth = ChordFifth::None;
	}

	// Seventh.
	if (has(10)) {
		s.seventh = ChordSeventh::Minor;
		use(10);
	} else if (has(11)) {
		s.seventh = ChordSeventh::Major;
		use(11);
	} else if (s.third == ChordThird::Minor && s.fifth == ChordFifth::Flat && has(9)) {
		s.seventh = ChordSeventh::Diminished;
		use(9);
	}

	// Tensions: whatever is left.
	const std::array<std::pair<int, uint8_t>, 7> tensions = {
		{{1, flat9}, {2, nine}, {3, sharp9}, {5, eleven}, {6, sharp11}, {8, flat13}, {9, thirteen}}};
	for (const auto &[interval, bit] : tensions) {
		if (has(interval) && !(used & (1u << interval))) {
			s.extensions |= bit;
			use(interval);
		}
	}
	if (used != rel)
		return std::nullopt;

	const bool seventh = s.seventh != ChordSeventh::None;
	const bool six = !seventh && (s.extensions & thirteen);

	// Shapes that are not readable as a chord on this root.
	if (s.third == ChordThird::None && (s.fifth == ChordFifth::None || !seventh))
		return std::nullopt;
	if (s.fifth == ChordFifth::None && !seventh && !six) {
		if (!tertian || (s.extensions & ~nine) != 0)
			return std::nullopt;
	}

	// Score: start high and subtract for everything unusual.
	int score = 100;
	switch (s.third) {
	case ChordThird::Sus4:
		score -= 6;
		break;
	case ChordThird::Sus2:
		score -= 8;
		break;
	case ChordThird::None:
		score -= 20;
		break;
	default:
		break;
	}
	switch (s.fifth) {
	case ChordFifth::None:
		score -= seventh ? 2 : 6;
		break;
	case ChordFifth::Flat:
		score -= s.third == ChordThird::Minor ? 3 : (s.seventh == ChordSeventh::Minor ? 5 : 8);
		break;
	case ChordFifth::Sharp:
		score -= s.seventh == ChordSeventh::Minor ? 4 : 6;
		break;
	default:
		break;
	}
	if (s.seventh == ChordSeventh::Diminished)
		score -=
			2 + 8 * std::popcount(static_cast<unsigned int>(s.extensions)); // dim7 + tension = rootless 7b9
	if (s.seventh == ChordSeventh::Major && s.third == ChordThird::Minor)
		score -= 3;
	constexpr uint8_t naturals = nine | eleven | thirteen;
	constexpr uint8_t altered = flat9 | sharp9 | sharp11 | flat13;
	const int natural_count = std::popcount(static_cast<unsigned int>(s.extensions & naturals));
	const int altered_count = std::popcount(static_cast<unsigned int>(s.extensions & altered));
	const bool dominant = s.third == ChordThird::Major && s.seventh == ChordSeventh::Minor;
	// Altered tensions are idiomatic on dominant chords and rare elsewhere.
	score -= 2 * natural_count + (dominant ? 4 : 7) * altered_count;
	if (s.seventh == ChordSeventh::Major && s.third == ChordThird::Minor && s.fifth == ChordFifth::Flat)
		score -= 14; // dim(maj7) is almost always a triad over a foreign bass
	if (!seventh) {
		// Added notes on a triad are rarer than the same notes on a seventh chord.
		uint8_t added = s.extensions;
		if (six)
			added &= static_cast<uint8_t>(~(thirteen | nine));
		score -= 3 * std::popcount(static_cast<unsigned int>(added));
		if (s.extensions & sharp9)
			score -= 6;
	}
	if ((s.extensions & eleven) && s.third == ChordThird::Major && seventh)
		score -= 4; // natural 11 over a major third is an avoid note
	if (s.third == ChordThird::Minor && s.fifth == ChordFifth::None && (s.extensions & flat13))
		score -= 8; // m(b6) without a fifth is usually a major chord over its third
	return Analysis{s, score};
}

void append_alterations(std::string &out, const ChordSpec &s, bool include_flat5)
{
	if (include_flat5 && s.fifth == ChordFifth::Flat)
		out += "b5";
	if (s.fifth == ChordFifth::Sharp)
		out += "#5";
	if (s.extensions & flat9)
		out += "b9";
	if (s.extensions & sharp9)
		out += "#9";
	if (s.extensions & sharp11)
		out += "#11";
	if (s.extensions & flat13)
		out += "b13";
}

int highest_natural(uint8_t ext)
{
	if (ext & thirteen)
		return 13;
	if (ext & eleven)
		return 11;
	if (ext & nine)
		return 9;
	return 7;
}

const char *sus_text(ChordThird third)
{
	return third == ChordThird::Sus4 ? "sus4" : third == ChordThird::Sus2 ? "sus2" : "";
}

std::string international_suffix(const ChordSpec &s)
{
	if (s.power)
		return "5";
	const bool minor = s.third == ChordThird::Minor;
	const bool major = s.third == ChordThird::Major;
	std::string out;
	if (s.seventh == ChordSeventh::None) {
		const bool six = s.extensions & thirteen;
		bool nine_used = false;
		if (minor && s.fifth == ChordFifth::Flat)
			out = "dim";
		else if (major && s.fifth == ChordFifth::Sharp)
			out = "aug";
		else if (minor)
			out = "m";
		if (six) {
			out += "6";
			if (s.extensions & nine) {
				out += "/9";
				nine_used = true;
			}
		}
		out += sus_text(s.third);
		if ((s.extensions & nine) && !nine_used)
			out += "add9";
		if (s.extensions & eleven)
			out += "add11";
		std::string alt;
		const bool show_flat5 = !(minor && s.fifth == ChordFifth::Flat);
		ChordSpec copy = s;
		if (major && s.fifth == ChordFifth::Sharp)
			copy.fifth = ChordFifth::Perfect; // already called "aug"
		append_alterations(alt, copy, show_flat5);
		if (!alt.empty())
			out += "(" + alt + ")";
		if (s.fifth == ChordFifth::None)
			out += "(no5)";
		return out;
	}

	if (s.seventh == ChordSeventh::Diminished) {
		out = "dim7";
		if (s.extensions & nine)
			out += "add9";
		if (s.extensions & eleven)
			out += "add11";
		if (s.extensions & thirteen)
			out += "add13";
		ChordSpec copy = s;
		copy.fifth = ChordFifth::Perfect;
		append_alterations(out, copy, false);
		return out;
	}

	const int n = highest_natural(s.extensions);
	const std::string number = std::to_string(n);
	if (minor && s.seventh == ChordSeventh::Minor)
		out = "m" + number;
	else if (minor)
		out = "mMaj" + number;
	else if (s.seventh == ChordSeventh::Major)
		out = "maj" + number;
	else
		out = number;
	out += sus_text(s.third);
	append_alterations(out, s, true);
	if (s.third == ChordThird::None)
		out += "(no3)";
	return out;
}

std::string brazilian_suffix(const ChordSpec &s)
{
	if (s.power)
		return "5";
	const bool minor = s.third == ChordThird::Minor;
	std::string base;
	std::vector<std::string> list;
	const bool seventh = s.seventh != ChordSeventh::None;

	if (s.seventh == ChordSeventh::Diminished) {
		base = "°";
	} else if (!seventh && minor && s.fifth == ChordFifth::Flat) {
		base = "dim";
	} else {
		if (minor)
			base = "m";
		if (s.seventh == ChordSeventh::Minor)
			base += "7";
		else if (s.seventh == ChordSeventh::Major && minor)
			list.push_back("7M");
		else if (s.seventh == ChordSeventh::Major)
			base += "7M";
		else if (s.extensions & thirteen)
			base += "6";
	}
	if (s.third == ChordThird::Sus4) {
		if (seventh)
			list.push_back("4");
		else
			base += "sus4";
	}
	if (s.third == ChordThird::Sus2)
		base += "sus2";

	const bool flat5_implied = minor && s.fifth == ChordFifth::Flat && s.seventh != ChordSeventh::Minor &&
				   s.seventh != ChordSeventh::Major;
	if (s.fifth == ChordFifth::Flat && !flat5_implied)
		list.push_back("b5");
	if (s.fifth == ChordFifth::Sharp)
		list.push_back("#5");
	if (s.extensions & flat9)
		list.push_back("b9");
	if (s.extensions & nine)
		list.push_back("9");
	if (s.extensions & sharp9)
		list.push_back("#9");
	if (s.extensions & eleven)
		list.push_back("11");
	if (s.extensions & sharp11)
		list.push_back("#11");
	if (s.extensions & flat13)
		list.push_back("b13");
	if ((s.extensions & thirteen) && seventh)
		list.push_back("13");

	std::string out = base;
	if (!list.empty()) {
		out += "(";
		for (size_t i = 0; i < list.size(); ++i) {
			if (i)
				out += "/";
			out += list[i];
		}
		out += ")";
	}
	if (!seventh && s.fifth == ChordFifth::None)
		out += "(no5)";
	if (s.third == ChordThird::None)
		out += "(no3)";
	return out;
}

std::string jazz_suffix(const ChordSpec &s)
{
	if (s.power)
		return "5";
	const bool minor = s.third == ChordThird::Minor;
	const bool major = s.third == ChordThird::Major;
	if (s.seventh == ChordSeventh::None) {
		std::string out;
		const bool six = s.extensions & thirteen;
		bool nine_used = false;
		if (minor && s.fifth == ChordFifth::Flat)
			out = "°";
		else if (major && s.fifth == ChordFifth::Sharp)
			out = "+";
		else if (minor)
			out = "-";
		if (six) {
			out += "6";
			if (s.extensions & nine) {
				out += "/9";
				nine_used = true;
			}
		}
		out += sus_text(s.third);
		if ((s.extensions & nine) && !nine_used)
			out += "add9";
		if (s.extensions & eleven)
			out += "add11";
		ChordSpec copy = s;
		if (major && s.fifth == ChordFifth::Sharp)
			copy.fifth = ChordFifth::Perfect;
		std::string alt;
		append_alterations(alt, copy, !(minor && s.fifth == ChordFifth::Flat));
		if (!alt.empty())
			out += "(" + alt + ")";
		if (s.fifth == ChordFifth::None)
			out += "(no5)";
		return out;
	}
	if (s.seventh == ChordSeventh::Diminished) {
		std::string out = "°7";
		ChordSpec copy = s;
		copy.fifth = ChordFifth::Perfect;
		if (s.extensions & nine)
			out += "add9";
		append_alterations(out, copy, false);
		return out;
	}
	const std::string number = std::to_string(highest_natural(s.extensions));
	std::string out;
	ChordSpec copy = s;
	if (minor && s.fifth == ChordFifth::Flat && s.seventh == ChordSeventh::Minor) {
		out = "ø" + number;
		copy.fifth = ChordFifth::Perfect;
	} else if (minor && s.seventh == ChordSeventh::Minor) {
		out = "-" + number;
	} else if (minor) {
		out = "-Δ" + number;
	} else if (s.seventh == ChordSeventh::Major) {
		out = "Δ" + number;
	} else {
		out = number;
	}
	out += sus_text(s.third);
	append_alterations(out, copy, true);
	if (s.third == ChordThird::None)
		out += "(no3)";
	return out;
}

std::string note_with_symbols(std::string_view name)
{
	std::string out(name.substr(0, 1));
	if (name.size() > 1)
		out += name[1] == 'b' ? "♭" : "♯";
	return out;
}

std::string with_symbols(const std::string &text)
{
	std::string out;
	for (size_t i = 0; i < text.size(); ++i) {
		const char c = text[i];
		const bool before_digit = i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9';
		if (c == 'b' && before_digit)
			out += "♭";
		else if (c == '#')
			out += "♯";
		else
			out += c;
	}
	return out;
}

constexpr std::array<const char *, 12> sharp_names = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr std::array<const char *, 12> flat_names = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
// The spellings musicians use most for chord roots.
constexpr std::array<const char *, 12> auto_major = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
constexpr std::array<const char *, 12> auto_minor = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"};

} // namespace

bool ChordCandidate::same_chord(const ChordCandidate &other) const
{
	return root == other.root && bass == other.bass && spec == other.spec;
}

std::vector<ChordCandidate> detect_chords(std::span<const MidiNote> notes,
					  const std::optional<ChordCandidate> &previous)
{
	const auto set = note_set(notes);
	std::vector<ChordCandidate> result;
	const int count = std::popcount(static_cast<unsigned int>(set.mask));
	if (count < 2)
		return result;

	auto consider = [&](uint16_t mask, bool foreign) {
		for (int root = 0; root < 12; ++root) {
			if (!(mask & (1u << root)))
				continue;
			const auto analysis = analyze(rotate(mask, root));
			if (!analysis)
				continue;
			if (foreign && analysis->spec.power)
				continue;
			ChordCandidate candidate;
			candidate.root = root;
			candidate.bass = set.bass;
			candidate.spec = analysis->spec;
			candidate.suffix = international_suffix(analysis->spec);
			candidate.omitted_fifth = analysis->spec.fifth == ChordFifth::None;
			candidate.foreign_bass = foreign;
			candidate.score = analysis->score;
			if (root == set.bass)
				candidate.score += 10;
			if (foreign)
				candidate.score -= 8;
			if (previous && previous->root == root && previous->spec == analysis->spec)
				candidate.score += 12;
			result.push_back(std::move(candidate));
		}
	};

	consider(set.mask, false);
	// A bass note that is not doubled higher up may sit outside the chord ("D/C").
	if (set.bass_count == 1 && count >= 4)
		consider(static_cast<uint16_t>(set.mask & ~(1u << set.bass)), true);

	std::stable_sort(result.begin(), result.end(),
			 [](const auto &left, const auto &right) { return left.score > right.score; });
	return result;
}

std::string format_chord(const ChordCandidate &chord, const ChordFormat &format)
{
	if (chord.root < 0 || chord.root > 11 || chord.bass < 0 || chord.bass > 11)
		return {};
	const bool minor = chord.spec.third == ChordThird::Minor;
	const char *root_name = nullptr;
	const std::array<const char *, 12> *bass_names = nullptr;
	switch (format.accidentals) {
	case ChordAccidentals::Flats:
		root_name = flat_names[chord.root];
		bass_names = &flat_names;
		break;
	case ChordAccidentals::Auto: {
		root_name = (minor ? auto_minor : auto_major)[chord.root];
		const std::string_view name = root_name;
		const bool flat_side = name.find('b') != std::string_view::npos || name == "F" || name == "C";
		bass_names = flat_side ? &flat_names : &sharp_names;
		break;
	}
	default:
		root_name = sharp_names[chord.root];
		bass_names = &sharp_names;
		break;
	}

	std::string suffix;
	switch (format.style) {
	case ChordStyle::Brazilian:
		suffix = brazilian_suffix(chord.spec);
		break;
	case ChordStyle::Jazz:
		suffix = jazz_suffix(chord.spec);
		break;
	default:
		suffix = chord.suffix;
		break;
	}

	std::string result = format.symbols ? note_with_symbols(root_name) : std::string(root_name);
	result += format.symbols ? with_symbols(suffix) : suffix;
	if (format.show_bass && chord.bass != chord.root) {
		result += '/';
		const char *bass_name = (*bass_names)[chord.bass];
		result += format.symbols ? note_with_symbols(bass_name) : std::string(bass_name);
	}
	return result;
}

std::string format_chord(const ChordCandidate &chord, bool show_bass, bool prefer_flats)
{
	ChordFormat format;
	format.show_bass = show_bass;
	format.accidentals = prefer_flats ? ChordAccidentals::Flats : ChordAccidentals::Sharps;
	return format_chord(chord, format);
}

void ChordTracker::configure(uint64_t settle_ms, uint64_t clear_ms)
{
	const auto settle = std::min(settle_ms, uint64_t{500});
	const auto clear = std::min(clear_ms, uint64_t{2000});
	if (settle_ms_ == settle && clear_ms_ == clear)
		return;
	settle_ms_ = settle;
	clear_ms_ = clear;
	reset();
}

void ChordTracker::reset()
{
	current_.reset();
	pending_.reset();
	initialized_ = false;
	pending_active_ = false;
	clearing_ = false;
}

const std::optional<ChordCandidate> &ChordTracker::update(std::span<const MidiNote> notes, uint64_t now_ms)
{
	const auto set = note_set(notes);
	if (!initialized_ || set.mask != last_mask_ || set.bass != last_bass_) {
		initialized_ = true;
		last_mask_ = set.mask;
		last_bass_ = set.bass;
		changed_at_ = now_ms;
		auto candidates = detect_chords(notes, current_);
		if (candidates.empty()) {
			pending_.reset();
			pending_active_ = false;
			if (!clearing_) {
				clearing_ = true;
				clear_since_ = now_ms;
			}
		} else {
			clearing_ = false;
			pending_ = candidates.front();
			if (current_ && current_->same_chord(*pending_)) {
				pending_active_ = false;
			} else if (!pending_active_) {
				pending_active_ = true;
				pending_since_ = now_ms;
			}
		}
	}
	if (clearing_ && now_ms - clear_since_ >= clear_ms_)
		current_.reset();
	if (pending_active_ && (now_ms - changed_at_ >= settle_ms_ ||
				now_ms - pending_since_ >= std::max(uint64_t{160}, settle_ms_ * 2))) {
		current_ = pending_;
		pending_active_ = false;
	}
	return current_;
}
