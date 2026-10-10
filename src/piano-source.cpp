#include "piano-source.h"
#include "midi-input.h"
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <algorithm>
#include <array>
#include <cmath>

void PianoSourceData::calculate_layout()
{
	if (range == last_range)
		return;
	layout = make_piano_layout(range);
	last_range = range;
}

static const char *piano_source_get_name(void *)
{
	return obs_module_text("PianoKeyboard");
}

static PianoRange normalize_range_settings(obs_data_t *settings)
{
	const auto range =
		normalize_piano_range(obs_data_get_int(settings, "first_note"), obs_data_get_int(settings, "num_keys"));
	obs_data_set_int(settings, "num_keys", range.num_keys);
	obs_data_set_int(settings, "first_note", range.first_note);
	return range;
}

static void piano_source_destroy(void *data)
{
	auto *ps = static_cast<PianoSourceData *>(data);
	delete ps;
}

static void piano_source_update(void *data, obs_data_t *settings)
{
	auto *ps = static_cast<PianoSourceData *>(data);

	ps->white_key_color = static_cast<uint32_t>(obs_data_get_int(settings, "white_key_color"));
	ps->black_key_color = static_cast<uint32_t>(obs_data_get_int(settings, "black_key_color"));
	ps->active_note_color = static_cast<uint32_t>(obs_data_get_int(settings, "active_note_color"));
	ps->outline_color = static_cast<uint32_t>(obs_data_get_int(settings, "outline_color"));
	ps->range = normalize_range_settings(settings);
	ps->velocity_sensitive = obs_data_get_bool(settings, "velocity_sensitive");
	ps->show_outline = obs_data_get_bool(settings, "show_outline");
	ps->outline_width = normalize_piano_outline_width(obs_data_get_int(settings, "outline_width"));
	obs_data_set_int(settings, "outline_width", ps->outline_width);

	const bool was_enabled = ps->trails_enabled;
	ps->trails_enabled = obs_data_get_bool(settings, "floating_notes");
	ps->trails_height =
		static_cast<int>(std::clamp<long long>(obs_data_get_int(settings, "notes_height"), 50, 2000));
	ps->trails_speed = static_cast<int>(std::clamp<long long>(obs_data_get_int(settings, "notes_speed"), 20, 1500));
	ps->trails_color = static_cast<uint32_t>(obs_data_get_int(settings, "notes_color"));
	ps->trails_fade = static_cast<int>(std::clamp<long long>(obs_data_get_int(settings, "notes_fade"), 0, 100));
	ps->trails_sustain = obs_data_get_bool(settings, "notes_sustain");
	if (!ps->trails_enabled || !was_enabled)
		ps->trails.clear();

	const char *device = obs_data_get_string(settings, "midi_device");

	std::string new_device = device ? device : "";
	if (new_device != ps->midi_device_name) {
		ps->midi_device_name = new_device;
		ps->midi_input.reset();
		if (!new_device.empty()) {
			ps->midi_input = MidiManager::instance().get_input(new_device);
		}
	}

	ps->calculate_layout();
}

static void *piano_source_create(obs_data_t *settings, obs_source_t *source)
{
	auto ps = std::make_unique<PianoSourceData>();
	ps->source = source;
	piano_source_update(ps.get(), settings);
	return ps.release();
}

static void piano_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "num_keys", 49);
	obs_data_set_default_int(settings, "first_note", 36);
	obs_data_set_default_int(settings, "white_key_color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "black_key_color", 0xFF1A1A1A);
	obs_data_set_default_int(settings, "active_note_color", 0xFFFF9933);
	obs_data_set_default_int(settings, "outline_color", 0xFF000000);
	obs_data_set_default_bool(settings, "velocity_sensitive", true);
	obs_data_set_default_bool(settings, "show_outline", true);
	obs_data_set_default_int(settings, "outline_width", 1);
	obs_data_set_default_string(settings, "midi_device", "");
	obs_data_set_default_bool(settings, "floating_notes", false);
	obs_data_set_default_int(settings, "notes_height", 400);
	obs_data_set_default_int(settings, "notes_speed", 200);
	obs_data_set_default_int(settings, "notes_color", 0xFF58A4E0);
	obs_data_set_default_int(settings, "notes_fade", 25);
	obs_data_set_default_bool(settings, "notes_sustain", true);
}

static void populate_devices(obs_property_t *list, const std::string &selected)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("None"), "");
	const auto devices = MidiManager::instance().list_devices();
	for (const auto &device : devices)
		obs_property_list_add_string(list, device.c_str(), device.c_str());
	if (!selected.empty() && std::find(devices.begin(), devices.end(), selected) == devices.end())
		obs_property_list_add_string(list, selected.c_str(), selected.c_str());
}

static const char *device_status(const PianoSourceData *ps)
{
	const char *key = !ps || ps->midi_device_name.empty()           ? "Keyboard.SelectDevice"
			  : ps->midi_input && ps->midi_input->is_open() ? "Keyboard.Connected"
									: "Keyboard.WaitingDevice";
	return obs_module_text(key);
}

static bool refresh_devices(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *ps = static_cast<PianoSourceData *>(data);
	auto *device_list = obs_properties_get(props, "midi_device");
	if (!device_list)
		return false;
	populate_devices(device_list, ps ? ps->midi_device_name : "");
	obs_property_set_description(obs_properties_get(props, "device_status"), device_status(ps));
	return true;
}

static bool range_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const auto range = normalize_range_settings(settings);
	obs_property_int_set_limits(obs_properties_get(props, "first_note"), 0, 128 - range.num_keys, 1);
	return true;
}

static obs_properties_t *piano_source_properties(void *data)
{
	auto *ps = static_cast<PianoSourceData *>(data);
	auto *props = obs_properties_create();

	auto *device_list = obs_properties_add_list(props, "midi_device", obs_module_text("MIDIDevice"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	populate_devices(device_list, ps ? ps->midi_device_name : "");
	obs_properties_add_text(props, "device_status", device_status(ps), OBS_TEXT_INFO);

	obs_properties_add_button2(props, "refresh_devices", obs_module_text("RefreshDevices"), refresh_devices, ps);

	auto *num_keys = obs_properties_add_int_slider(props, "num_keys", obs_module_text("NumKeys"), 25, 88, 1);
	auto *first_note = obs_properties_add_int(props, "first_note", obs_module_text("FirstNote"), 0,
						  128 - (ps ? ps->range.num_keys : 49), 1);
	obs_property_set_modified_callback(num_keys, range_changed);
	obs_property_set_modified_callback(first_note, range_changed);

	auto *color_group = obs_properties_create();
	obs_properties_add_color(color_group, "white_key_color", obs_module_text("WhiteKeyColor"));
	obs_properties_add_color(color_group, "black_key_color", obs_module_text("BlackKeyColor"));
	obs_properties_add_color(color_group, "active_note_color", obs_module_text("ActiveNoteColor"));
	obs_properties_add_color(color_group, "outline_color", obs_module_text("OutlineColor"));
	obs_properties_add_bool(color_group, "show_outline", obs_module_text("ShowOutline"));
	obs_properties_add_int_slider(color_group, "outline_width", obs_module_text("OutlineWidth"), 1, 4, 1);

	obs_properties_add_group(props, "appearance", obs_module_text("Appearance"), OBS_GROUP_NORMAL, color_group);

	obs_properties_add_bool(props, "velocity_sensitive", obs_module_text("VelocitySensitive"));

	auto *notes_group = obs_properties_create();
	obs_properties_add_int_slider(notes_group, "notes_height", obs_module_text("Notes.Height"), 50, 2000, 10);
	obs_properties_add_int_slider(notes_group, "notes_speed", obs_module_text("Notes.Speed"), 20, 1500, 10);
	obs_properties_add_color(notes_group, "notes_color", obs_module_text("Notes.Color"));
	obs_properties_add_int_slider(notes_group, "notes_fade", obs_module_text("Notes.Fade"), 0, 100, 1);
	obs_properties_add_bool(notes_group, "notes_sustain", obs_module_text("Notes.Sustain"));
	obs_properties_add_group(props, "floating_notes", obs_module_text("Notes.Group"), OBS_GROUP_CHECKABLE,
				 notes_group);

	return props;
}

static void piano_source_tick(void *data, float seconds)
{
	auto *ps = static_cast<PianoSourceData *>(data);
	if (!ps)
		return;
	ps->clock += static_cast<double>(seconds);
	if (!ps->trails_enabled)
		return;
	const double max_age = static_cast<double>(ps->trails_height) / std::max(ps->trails_speed, 1) + 0.5;
	if (ps->midi_input)
		ps->trails.update(ps->midi_input->snapshot(-1, ps->trails_sustain).notes, ps->clock, max_age);
	else
		ps->trails.update({}, ps->clock, max_age);
}

static void draw_rect(gs_eparam_t *color_param, const vec4 &color, float x, float y, float w, float h)
{
	if (w <= 0.0f || h <= 0.0f || color.w <= 0.0f)
		return;
	gs_effect_set_vec4(color_param, &color);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);
	gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
}

static void draw_trails(PianoSourceData *ps, gs_eparam_t *color_param)
{
	const auto &trails = ps->trails.trails();
	if (trails.empty())
		return;

	// Map each MIDI note to its key rectangle.
	std::array<const KeyRect *, 128> keys{};
	std::array<bool, 128> black{};
	for (const auto &k : ps->layout.white_keys)
		if (k.midi_note >= 0 && k.midi_note < 128)
			keys[k.midi_note] = &k;
	for (const auto &k : ps->layout.black_keys)
		if (k.midi_note >= 0 && k.midi_note < 128) {
			keys[k.midi_note] = &k;
			black[k.midi_note] = true;
		}

	const float area = static_cast<float>(ps->trails_height);
	const float speed = static_cast<float>(ps->trails_speed);
	const float fade_zone = area * static_cast<float>(ps->trails_fade) / 100.0f;
	constexpr int fade_steps = 12;

	vec4 base;
	vec4_from_rgba(&base, ps->trails_color);

	for (const auto &t : trails) {
		const KeyRect *key = keys[t.note];
		if (!key)
			continue;

		float top = area - static_cast<float>(ps->clock - t.start) * speed;
		float bottom = t.open ? area : area - static_cast<float>(ps->clock - t.end) * speed;
		if (bottom <= 0.0f)
			continue;
		top = std::max(top, 0.0f);
		if (bottom - top < 2.0f)
			top = bottom - 2.0f;

		const bool is_black = black[t.note];
		const float gap = is_black ? 1.0f : 2.0f;
		const float x = key->x + gap;
		const float w = key->w - gap * 2.0f;

		vec4 color = base;
		if (is_black) {
			color.x *= 0.78f;
			color.y *= 0.78f;
			color.z *= 0.78f;
		}
		if (ps->velocity_sensitive && t.velocity > 0)
			color.w *= 0.35f + 0.65f * (t.velocity / 127.0f);

		// Solid part below the fade zone.
		const float solid_top = std::max(top, fade_zone);
		if (bottom > solid_top)
			draw_rect(color_param, color, x, solid_top, w, bottom - solid_top);

		// Gradient part inside the fade zone, drawn in slices.
		if (fade_zone > 0.0f && top < fade_zone) {
			const float step = fade_zone / fade_steps;
			const float seg_bottom = std::min(bottom, fade_zone);
			for (int i = 0; i < fade_steps; ++i) {
				const float s0 = std::max(top, i * step);
				const float s1 = std::min(seg_bottom, (i + 1) * step);
				if (s1 <= s0)
					continue;
				vec4 c = color;
				c.w *= (i + 0.5f) / fade_steps;
				draw_rect(color_param, c, x, s0, w, s1 - s0);
			}
		}
	}
}

static void piano_source_render(void *data, gs_effect_t *)
{
	auto *ps = static_cast<PianoSourceData *>(data);
	if (!ps)
		return;

	auto active_notes = ps->midi_input ? ps->midi_input->get_active_notes() : std::map<int, uint8_t>();

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (!solid)
		return;

	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
	if (!color_param)
		return;

	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	if (!tech)
		return;

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	if (ps->trails_enabled)
		draw_trails(ps, color_param);

	gs_matrix_push();
	gs_matrix_translate3f(0.0f, ps->keyboard_offset(), 0.0f);

	// Draw white keys
	for (const auto &key : ps->layout.white_keys) {
		auto it = active_notes.find(key.midi_note);
		struct vec4 color, oclr;

		if (it != active_notes.end()) {
			vec4_from_rgba(&color, ps->active_note_color);
			if (ps->velocity_sensitive && it->second > 0) {
				float t = it->second / 127.0f;
				color.w *= t;
			}
		} else {
			vec4_from_rgba(&color, ps->white_key_color);
		}

		vec4_from_rgba(&oclr, ps->outline_color);

		gs_matrix_push();
		gs_matrix_translate3f(key.x, key.y, 0.0f);

		if (ps->show_outline) {
			gs_effect_set_vec4(color_param, &oclr);
			gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(key.w), static_cast<uint32_t>(key.h));

			float ow = static_cast<float>(ps->outline_width);
			gs_matrix_translate3f(ow, ow, 0.0f);
			gs_effect_set_vec4(color_param, &color);
			gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(key.w - ow * 2.0f),
				       static_cast<uint32_t>(key.h - ow * 2.0f));
		} else {
			gs_effect_set_vec4(color_param, &color);
			gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(key.w), static_cast<uint32_t>(key.h));
		}

		gs_matrix_pop();
	}

	// Draw black keys
	for (const auto &key : ps->layout.black_keys) {
		auto it = active_notes.find(key.midi_note);
		struct vec4 color, oclr;

		if (it != active_notes.end()) {
			vec4_from_rgba(&color, ps->active_note_color);
			if (ps->velocity_sensitive && it->second > 0) {
				float t = it->second / 127.0f;
				color.w *= t;
			}
		} else {
			vec4_from_rgba(&color, ps->black_key_color);
		}

		vec4_from_rgba(&oclr, ps->outline_color);

		gs_matrix_push();
		gs_matrix_translate3f(key.x, key.y, 0.0f);

		if (ps->show_outline) {
			gs_effect_set_vec4(color_param, &oclr);
			gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(key.w), static_cast<uint32_t>(key.h));

			float ow = static_cast<float>(ps->outline_width);
			gs_matrix_translate3f(ow, ow, 0.0f);
			gs_effect_set_vec4(color_param, &color);
			gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(key.w - ow * 2.0f),
				       static_cast<uint32_t>(key.h - ow * 2.0f));
		} else {
			gs_effect_set_vec4(color_param, &color);
			gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(key.w), static_cast<uint32_t>(key.h));
		}

		gs_matrix_pop();
	}

	gs_matrix_pop();

	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

static uint32_t piano_source_width(void *data)
{
	auto *ps = static_cast<PianoSourceData *>(data);
	if (!ps)
		return 1;
	return std::max(ps->layout.width, 1u);
}

static uint32_t piano_source_height(void *data)
{
	auto *ps = static_cast<PianoSourceData *>(data);
	if (!ps)
		return 1;
	return std::max(ps->layout.height + static_cast<uint32_t>(ps->keyboard_offset()), 1u);
}

struct obs_source_info piano_keyboard_source_info = {
	.id = "piano_keyboard_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = piano_source_get_name,
	.create = piano_source_create,
	.destroy = piano_source_destroy,
	.get_width = piano_source_width,
	.get_height = piano_source_height,
	.get_defaults = piano_source_defaults,
	.get_properties = piano_source_properties,
	.update = piano_source_update,
	.video_tick = piano_source_tick,
	.video_render = piano_source_render,
};
