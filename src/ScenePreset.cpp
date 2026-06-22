// SPDX-License-Identifier: GPL-2.0-or-later

#include "ScenePreset.hpp"

#include <obs.h>

ScenePreset preset_load(obs_source_t *scene)
{
	ScenePreset p;
	if (!scene)
		return p;

	obs_data_t *priv = obs_source_get_private_settings(scene);
	obs_data_t *d = obs_data_get_obj(priv, kPresetKey);
	if (d) {
		// Provide sane defaults so a partially-written object never
		// yields zero-sized resolutions etc.
		obs_data_set_default_int(d, "base_cx", p.base_cx);
		obs_data_set_default_int(d, "base_cy", p.base_cy);
		obs_data_set_default_int(d, "output_cx", p.output_cx);
		obs_data_set_default_int(d, "output_cy", p.output_cy);
		obs_data_set_default_int(d, "fps", p.fps);
		obs_data_set_default_int(d, "audio_tracks", p.audio_tracks);

		p.enabled = obs_data_get_bool(d, "enabled");

		p.use_base_res = obs_data_get_bool(d, "use_base_res");
		p.base_cx = (uint32_t)obs_data_get_int(d, "base_cx");
		p.base_cy = (uint32_t)obs_data_get_int(d, "base_cy");

		p.use_output_res = obs_data_get_bool(d, "use_output_res");
		p.output_cx = (uint32_t)obs_data_get_int(d, "output_cx");
		p.output_cy = (uint32_t)obs_data_get_int(d, "output_cy");

		p.use_fps = obs_data_get_bool(d, "use_fps");
		p.fps = (uint32_t)obs_data_get_int(d, "fps");

		p.use_rec_path = obs_data_get_bool(d, "use_rec_path");
		p.rec_path = obs_data_get_string(d, "rec_path");

		p.use_rec_format = obs_data_get_bool(d, "use_rec_format");
		p.rec_format = obs_data_get_string(d, "rec_format");

		p.use_audio_tracks = obs_data_get_bool(d, "use_audio_tracks");
		p.audio_tracks = (uint32_t)obs_data_get_int(d, "audio_tracks");

		p.restart_recording = obs_data_get_bool(d, "restart_recording");

		obs_data_release(d);
	}
	obs_data_release(priv);
	return p;
}

void preset_save(obs_source_t *scene, const ScenePreset &p)
{
	if (!scene)
		return;

	obs_data_t *priv = obs_source_get_private_settings(scene);
	obs_data_t *d = obs_data_create();

	obs_data_set_bool(d, "enabled", p.enabled);

	obs_data_set_bool(d, "use_base_res", p.use_base_res);
	obs_data_set_int(d, "base_cx", p.base_cx);
	obs_data_set_int(d, "base_cy", p.base_cy);

	obs_data_set_bool(d, "use_output_res", p.use_output_res);
	obs_data_set_int(d, "output_cx", p.output_cx);
	obs_data_set_int(d, "output_cy", p.output_cy);

	obs_data_set_bool(d, "use_fps", p.use_fps);
	obs_data_set_int(d, "fps", p.fps);

	obs_data_set_bool(d, "use_rec_path", p.use_rec_path);
	obs_data_set_string(d, "rec_path", p.rec_path.c_str());

	obs_data_set_bool(d, "use_rec_format", p.use_rec_format);
	obs_data_set_string(d, "rec_format", p.rec_format.c_str());

	obs_data_set_bool(d, "use_audio_tracks", p.use_audio_tracks);
	obs_data_set_int(d, "audio_tracks", p.audio_tracks);

	obs_data_set_bool(d, "restart_recording", p.restart_recording);

	// Store back into private_settings; OBS serializes this with the scene
	// collection on save and copies it on scene duplication.
	obs_data_set_obj(priv, kPresetKey, d);

	obs_data_release(d);
	obs_data_release(priv);
}
