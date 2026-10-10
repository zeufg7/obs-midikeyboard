#pragma once
#include "note-trails.h"
#include "piano-layout.h"
#include <obs-module.h>
#include <memory>
#include <string>

class MidiInput;

struct PianoSourceData {
	obs_source_t *source = nullptr;
	std::shared_ptr<MidiInput> midi_input;

	std::string midi_device_name;
	PianoRange range;

	uint32_t white_key_color = 0xFFFFFFFF;
	uint32_t black_key_color = 0xFF1A1A1A;
	uint32_t active_note_color = 0xFFFF9933;
	uint32_t outline_color = 0xFF000000;
	bool velocity_sensitive = true;
	bool show_outline = true;
	int outline_width = 1;

	// Floating notes
	bool trails_enabled = false;
	int trails_height = 400;
	int trails_speed = 200;
	uint32_t trails_color = 0xFF58A4E0;
	int trails_fade = 25;
	bool trails_sustain = true;
	NoteTrails trails;
	double clock = 0.0;

	PianoRange last_range{-1, 0};
	PianoLayout layout;

	void calculate_layout();
	float keyboard_offset() const { return trails_enabled ? static_cast<float>(trails_height) : 0.0f; }
};
