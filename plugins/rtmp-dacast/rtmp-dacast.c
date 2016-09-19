#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <obs-module.h>
#include <obs-data.h>
#include <curl/curl.h>
#include <jansson.h>

#include "lookup-config.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("rtmp-dacast", "en-US")

#define warn(msg, ...) \
	blog(LOG_WARNING, "DaCast plugin: "msg, ##__VA_ARGS__)

struct rtmp_dacast {
	char *api_key;
	char *channel_id;
	char *channel_name;
	char *server;
	char *stream_key;
	char *username;
	char *password;
	bool use_auth;
};

static const char *rtmp_dacast_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ServiceName");
}

static void rtmp_dacast_update(void *data, obs_data_t *settings)
{
	struct rtmp_dacast *plugin = data;

	bfree(plugin->api_key);
	bfree(plugin->channel_id);
	bfree(plugin->channel_name);
	bfree(plugin->server);
	bfree(plugin->stream_key);
	bfree(plugin->username);
	bfree(plugin->password);

	plugin->api_key      = bstrdup(obs_data_get_string(settings, "api_key"));
	plugin->channel_id   = bstrdup(obs_data_get_string(settings, "channel_id"));
	plugin->channel_name = bstrdup(obs_data_get_string(settings, "channel_name"));
	plugin->server       = bstrdup(obs_data_get_string(settings, "server"));
	plugin->stream_key   = bstrdup(obs_data_get_string(settings, "stream_key"));
	plugin->username     = bstrdup(obs_data_get_string(settings, "username"));
	plugin->password     = bstrdup(obs_data_get_string(settings, "password"));
	plugin->use_auth     = obs_data_get_bool(settings, "use_auth");
}

static void *rtmp_dacast_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(service);

	struct rtmp_dacast *plugin = bzalloc(sizeof(struct rtmp_dacast));
	rtmp_dacast_update(plugin, settings);

	return plugin;
}

static void rtmp_dacast_destroy(void *data)
{
	struct rtmp_dacast *plugin = data;

	bfree(plugin->api_key);
	bfree(plugin->channel_id);
	bfree(plugin->channel_name);
	bfree(plugin->server);
	bfree(plugin->stream_key);
	bfree(plugin->username);
	bfree(plugin->password);
	bfree(plugin);
}

static obs_properties_t *rtmp_dacast_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t   *props = obs_properties_create();
	obs_property_t     *p;

	p = obs_properties_add_text(props, "api_key", obs_module_text("ApiKey"),
			OBS_TEXT_PASSWORD);

	p = obs_properties_add_list(props, "channel_id_select",
			obs_module_text("Channel"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	return props;
}

static const char *rtmp_dacast_url(void *data)
{
	struct rtmp_dacast *plugin = data;
	return plugin->server;
}

static const char *rtmp_dacast_stream_key(void *data)
{
	struct rtmp_dacast *plugin = data;
	return plugin->stream_key;
}

static const char *rtmp_dacast_username(void *data)
{
	struct rtmp_dacast *plugin = data;
	if (!plugin->use_auth) {
		return NULL;
	}
	return plugin->username;
}

static const char *rtmp_dacast_password(void *data)
{
	struct rtmp_dacast *plugin = data;
	if (!plugin->use_auth) {
		return NULL;
	}
	return plugin->password;
}

struct obs_service_info rtmp_dacast_service = {
	.id             = "rtmp_dacast",
	.get_name       = rtmp_dacast_getname,
	.create         = rtmp_dacast_create,
	.destroy        = rtmp_dacast_destroy,
	.update         = rtmp_dacast_update,
	.get_properties = rtmp_dacast_properties,
	.get_url        = rtmp_dacast_url,
	.get_key        = rtmp_dacast_stream_key,
	.get_username   = rtmp_dacast_username,
	.get_password   = rtmp_dacast_password,
};

bool obs_module_load(void)
{
	obs_register_service(&rtmp_dacast_service);
	return true;
}

void obs_module_unload(void)
{
}
