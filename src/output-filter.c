#include <obs-module.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include "plugin-macros.generated.h"

struct output_filter_s
{
	obs_source_t *context;
	obs_output_t *output;

	video_t *video_output;
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;

	// properties
	volatile bool auto_start;
	volatile bool start_requested;

	// controls
	bool active;
	volatile bool need_restart;
};

static const char *get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("OutputFilter");
}

static void of_render(void *data, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);

	struct output_filter_s *filter = data;

	uint32_t width = gs_stagesurface_get_width(filter->stagesurface);
	uint32_t height = gs_stagesurface_get_height(filter->stagesurface);

	if (!gs_texrender_begin(filter->texrender, width, height))
		return;

	obs_source_t *parent = obs_filter_get_parent(filter->context);
	if (!parent)
		return;

	struct vec4 background;
	vec4_zero(&background);

	gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	obs_source_skip_video_filter(filter->context);

	gs_blend_state_pop();
	gs_texrender_end(filter->texrender);

	struct video_frame output_frame;
	if (!video_output_lock_frame(filter->video_output, &output_frame, 1, obs_get_video_frame_time()))
		return;

	gs_stage_texture(filter->stagesurface, gs_texrender_get_texture(filter->texrender));

	uint8_t *video_data;
	uint32_t video_linesize;
	if (!gs_stagesurface_map(filter->stagesurface, &video_data, &video_linesize))
		return;

	uint32_t linesize = output_frame.linesize[0];

	for (uint32_t i = 0; i < height; i++) {
		uint32_t dst_offset = linesize * i;
		uint32_t src_offset = video_linesize * i;
		memcpy(output_frame.data[0] + dst_offset, video_data + src_offset, linesize);
	}

	gs_stagesurface_unmap(filter->stagesurface);
	video_output_unlock_frame(filter->video_output);
}

static void of_stop(void *data);

static void of_start(void *data)
{
	struct output_filter_s *filter = data;

	if (filter->active)
		return;

	if (!obs_source_enabled(filter->context))
		return;

	obs_source_t *parent = obs_filter_get_target(filter->context);
	uint32_t width = obs_source_get_base_width(parent);
	uint32_t height = obs_source_get_base_height(parent);

	if (!width || !height)
		return;

	obs_data_t *settings = obs_source_get_settings(filter->context);

	const char *id = obs_data_get_string(settings, ID_PREFIX "id");
	filter->output = obs_output_create(id, "output_filter", settings, NULL);

	obs_data_release(settings);

	if (!filter->output) {
		blog(LOG_ERROR, "Failed to create output");
	}

	obs_enter_graphics();
	filter->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	filter->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
	obs_leave_graphics();

	const struct video_output_info *main_voi = video_output_get_info(obs_get_video());

	struct video_output_info vi = {0};
	vi.format = VIDEO_FORMAT_BGRA;
	vi.width = width;
	vi.height = height;
	vi.fps_den = main_voi->fps_den;
	vi.fps_num = main_voi->fps_num;
	vi.cache_size = 16;
	vi.colorspace = main_voi->colorspace;
	vi.range = main_voi->range;
	vi.name = obs_source_get_name(filter->context);

	video_output_open(&filter->video_output, &vi);
	obs_output_set_media(filter->output, filter->video_output, obs_get_audio());

	bool started = obs_output_start(filter->output);
	filter->active = true;

	if (!started)
		of_stop(filter);
	else
		obs_add_main_render_callback(of_render, filter);
}

static void of_stop(void *data)
{
	struct output_filter_s *filter = data;

	if (!filter->active)
		return;

	obs_remove_main_render_callback(of_render, filter);

	obs_output_stop(filter->output);
	obs_output_release(filter->output);
	video_output_stop(filter->video_output);

	obs_enter_graphics();
	gs_stagesurface_destroy(filter->stagesurface);
	gs_texrender_destroy(filter->texrender);
	obs_leave_graphics();
	filter->stagesurface = NULL;
	filter->texrender = NULL;

	video_output_close(filter->video_output);
	filter->video_output = NULL;

	filter->active = false;
}

static void list_add_output_types(obs_property_t *prop)
{
	const char *id;
	for (size_t idx = 0; obs_enum_output_types(idx, &id); idx++) {
		uint32_t flags = obs_get_output_flags(id);
		if (flags & OBS_OUTPUT_ENCODED)
			continue;

		const char *name = obs_output_get_display_name(id);
		obs_property_list_add_string(prop, name, id);
	}
}

static bool id_modified(void *data, obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	const char *id = obs_data_get_string(settings, ID_PREFIX "id");
	if (!id || !id[0])
		return false;

	obs_properties_t *output_props = obs_get_output_properties(id);
	if (!output_props)
		return false;

	const char *name = obs_output_get_display_name(id);
	obs_properties_add_group(props, ID_PREFIX "output_props", name, OBS_GROUP_NORMAL, output_props);

	obs_property_set_enabled(prop, false);

	return true;
}

static bool start_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	struct output_filter_s *s = data;
	s->start_requested = true;
	return false;
}

static bool stop_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	struct output_filter_s *s = data;
	s->start_requested = false;
	return false;
}

static obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;

	prop = obs_properties_add_list(props, ID_PREFIX "id", obs_module_text("OutputID"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("NoSelection"), "");
	list_add_output_types(prop);
	obs_property_set_modified_callback2(prop, id_modified, data);

	obs_properties_add_bool(props, ID_PREFIX "auto_start", obs_module_text("AutoStart"));
	obs_properties_add_button2(props, ID_PREFIX "start_button", obs_module_text("StartButton"), start_clicked,
				   data);
	obs_properties_add_button2(props, ID_PREFIX "stop_button", obs_module_text("StopButton"), stop_clicked, data);

	return props;
}

static void update(void *data, obs_data_t *settings)
{
	struct output_filter_s *s = data;

	s->auto_start = obs_data_get_bool(settings, ID_PREFIX "auto_start");
}

#ifdef HAVE_FRONTEND
static void frontend_event(enum obs_frontend_event event, void *data)
{
	struct output_filter_s *s = data;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		of_start(s);
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		of_stop(s);
		break;
	}
}
#endif // HAVE_FRONTEND

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct output_filter_s *s = bzalloc(sizeof(struct output_filter_s));

	s->context = source;

#ifdef HAVE_FRONTEND
	obs_frontend_add_event_callback(frontend_event, s);
#endif

	update(s, settings);

	blog(LOG_INFO, "create: source=%p data=%p", source, s);

	return s;
}

static void destroy(void *data)
{
	struct output_filter_s *s = data;

#ifdef HAVE_FRONTEND
	obs_frontend_remove_event_callback(frontend_event, s);
#endif
	of_stop(s);

	bfree(s);
}

static bool should_active(struct output_filter_s *s)
{
	if (!obs_source_enabled(s->context))
		return false;

	if (s->auto_start || s->start_requested)
		return true;

	return false;
}

static void video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct output_filter_s *s = data;

	if (s->texrender)
		gs_texrender_reset(s->texrender);

	bool need_start = s->need_restart;
	bool need_stop = s->need_restart;
	s->need_restart = false;

	if (!s->active && should_active(s))
		need_start = true;
	else if (s->active && !should_active(s))
		need_stop = true;

	if (need_stop)
		of_stop(s);
	if (need_start)
		obs_queue_task(OBS_TASK_UI, of_start, s, false);
}

const struct obs_source_info output_filter = {
	.id = ID_PREFIX "filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.update = update,
	.get_properties = get_properties,
	.video_tick = video_tick,
};
