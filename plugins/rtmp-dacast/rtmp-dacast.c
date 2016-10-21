#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <obs-module.h>
#include <obs-data.h>
#include <curl/curl.h>

#include "lookup-config.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("rtmp-dacast", "en-US")

#define warn(msg, ...) \
	blog(LOG_WARNING, "DaCast plugin: "msg, ##__VA_ARGS__)

#define RTMP_DACAST_USERAGENT "rtmp-dacast plugin (libobs " OBS_VERSION ")"

struct http_request {
	char *url;
	struct curl_slist *header;
};

struct http_response {
	struct dstr data;
	long status;
	char error[CURL_ERROR_SIZE];
};

struct rtmp_dacast {
	char *api_key;
	char *server;
	char *stream_key;
	char *username;
	char *password;
	bool use_auth;
};

static void add_slash(struct dstr *str)
{
	if (str && str->array && dstr_end(str) != '/') {
		dstr_cat_ch(str, '/');
	}
}

static void http_response_init(struct http_response *res)
{
	dstr_init(&res->data);
	res->status = 0;
	res->error[0] = 0;
}

static void http_response_destroy(struct http_response *res)
{
	if (res) {
		dstr_free(&res->data);
	}
}

static size_t http_response_write(uint8_t *ptr, size_t size, size_t nmemb, struct http_response *res)
{
	size_t total = size * nmemb;

	if (total > 0) {
		dstr_ncat(&res->data, (const char *)ptr, total);
	}

	return total;
}

static void http_request_init(struct http_request *req)
{
	if (req) {
		req->url = NULL;
		req->header = NULL;
	}
}

static void http_request_destroy(struct http_request *req)
{
	if (!req) {
		return;
	}

	if (req->url) {
		bfree(req->url);
		req->url = NULL;
	}

	if (req->header) {
		curl_slist_free_all(req->header);
		req->header = NULL;
	}
}

static bool http_request_send(const struct http_request *req, struct http_response *res)
{
	CURLcode code = CURLE_OK;
	long status = 0;

	http_response_init(res);

	CURL *curl = curl_easy_init();
	if (!curl) {
		warn("[http_request_send] Could not initialize Curl");
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, req->url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req->header);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, res->error);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_response_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, res);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15);

#if LIBCURL_VERSION_NUM >= 0x072400
	// A lot of servers don't yet support ALPN
	curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0);
#endif

	code = curl_easy_perform(curl);
	if (code != CURLE_OK) {
		warn("[http_request_send] Fetch of URL \"%s\" failed: %s", req->url, res->error);
		curl_easy_cleanup(curl);
		return false;
	}

	code = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);

	res->status = status;

	if (code != CURLE_OK) {
		warn("[http_request_send] Fetch of URL \"%s\" failed: %s", req->url, res->error);
		return false;
	}

	if (status >= 400 || status < 200) {
		warn("[http_request_send] Fetch of URL \"%s\" failed: HTTP/%ld", req->url, status);
		return false;
	}

	return true;
}

static void rtmp_dacast_set_error(obs_properties_t *props, obs_data_t *settings, const char *msg) {
	if (settings) {
		obs_data_set_string(settings, "error", msg);
	}

	if (props) {
		obs_property_t *p = obs_properties_get(props, "error");

		if (p) {
			// Display the error to the user
			obs_property_set_visible(p, !!msg);
		}
	}
}

static void fetch_channels(obs_properties_t *props, obs_data_t *settings, const char *api_key)
{
	if (!api_key || *api_key == 0) {
		obs_data_erase(settings, "channels");
		rtmp_dacast_set_error(props, settings, NULL);
		return;
	}

	struct http_request req;
	http_request_init(&req);
	req.header = curl_slist_append(req.header, "User-Agent: " RTMP_DACAST_USERAGENT);

	struct dstr url = {0};
	dstr_copy(&url, RTMP_DACAST_API_URL);
	add_slash(&url);
	dstr_cat(&url, "external/obs/channel?apikey=");
	dstr_cat(&url, api_key);
	req.url = url.array;

	struct http_response res;
	http_response_init(&res);

	if (!http_request_send(&req, &res)) {
		struct dstr error = {0};

		dstr_copy(&error, "Error retrieving channel list: ");
		if (res.status == 403) {
			dstr_cat(&error, "access denied.\n\nPlease make sure the Open Broadcaster Key is valid.");
		}
		else if (res.error[0]) {
			dstr_cat(&error, res.error);
		}
		else {
			dstr_cat(&error, "the request has failed.");
		}

		rtmp_dacast_set_error(props, settings, error.array);
		dstr_free(&error);

		http_request_destroy(&req);
		http_response_destroy(&res);

		return;
	}

	// obs_data_create_from_json does not support stringified JSON arrays as
	// root, so wrap the array inside an object here.
	dstr_insert(&res.data, 0, "{\"data\":");
	dstr_cat(&res.data, "}");

	obs_data_t *root = obs_data_create_from_json((const char *) res.data.array);

	http_request_destroy(&req);
	http_response_destroy(&res);

	if (!root) {
		rtmp_dacast_set_error(props, settings, "Error parsing JSON response.");
		return;
	}

	obs_data_array_t *channels = obs_data_get_array(root, "data");
	obs_data_release(root);

	if (!channels) {
		rtmp_dacast_set_error(props, settings, "Missing or invalid channels data in API response.");
		return;
	}

	obs_data_set_array(settings, "channels", channels);
	obs_data_array_release(channels);

	rtmp_dacast_set_error(props, settings, NULL);
}

static void update_settings_for_channel(obs_data_t *settings, obs_data_t *channel)
{
	obs_data_t *channel_config = obs_data_get_obj(channel, "config");
	if (!channel_config) {
		warn("[update_settings_for_channel] missing channel config");
		return;
	}

	bool html5 = obs_data_get_bool(channel, "html5");
	const char *server      = obs_data_get_string(channel_config, "publishing_point_primary");
	const char *stream_key  = obs_data_get_string(channel_config, "stream_name");
	const char *username    = obs_data_get_string(channel_config, "login");
	const char *password    = obs_data_get_string(channel_config, "password");

	obs_data_release(channel_config);

	if (!server || *server == 0) {
		warn("[update_settings_for_channel] missing channel server");
		return;
	}

	if (!stream_key || *stream_key == 0) {
		warn("[update_settings_for_channel] missing channel stream_key");
	}

	obs_data_set_string(settings, "server", server);
	obs_data_set_string(settings, "stream_key", stream_key);
	obs_data_set_string(settings, "username", username);
	obs_data_set_string(settings, "password", password);
	obs_data_set_bool(settings, "use_auth", html5);
}

static void rtmp_dacast_channel_id(struct dstr *dstr, obs_data_t *channel)
{
	const char *id_str = obs_data_get_string(channel, "id");
	long long id_int = obs_data_get_int(channel, "id");

	if (id_str && *id_str) {
		dstr_copy(dstr, id_str);
	} else if (id_int != 0) {
		dstr_printf(dstr, "%lld", id_int);
	}
	else {
		dstr_resize(dstr, 0);
	}
}

static void build_channel_list(obs_property_t *prop, obs_data_t *settings)
{
	obs_property_list_clear(prop);

	struct dstr id_dstr = {0};
	obs_data_array_t *channels = obs_data_get_array(settings, "channels");
	obs_data_t *channel = obs_data_array_item(channels, 0);

	for (size_t i = 0; channel; channel = obs_data_array_item(channels, ++i)) {
		const char *title = obs_data_get_string(channel, "title");
		rtmp_dacast_channel_id(&id_dstr, channel);

		obs_data_release(channel);

		if (dstr_is_empty(&id_dstr)) {
			warn("[build_channel_list] channel has no id");
			continue;
		}

		if (!title) {
			warn("[build_channel_list] channel has no title");
			continue;
		}

		obs_property_list_add_string(prop, title, id_dstr.array);
	}

	obs_data_array_release(channels);
	dstr_free(&id_dstr);
}

static const char *rtmp_dacast_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ServiceName");
}

static void rtmp_dacast_update(void *data, obs_data_t *settings)
{
	struct rtmp_dacast *plugin = data;

	bfree(plugin->api_key);
	bfree(plugin->server);
	bfree(plugin->stream_key);
	bfree(plugin->username);
	bfree(plugin->password);

	plugin->api_key    = bstrdup(obs_data_get_string(settings, "api_key"));
	plugin->server     = bstrdup(obs_data_get_string(settings, "server"));
	plugin->stream_key = bstrdup(obs_data_get_string(settings, "stream_key"));
	plugin->username   = bstrdup(obs_data_get_string(settings, "username"));
	plugin->password   = bstrdup(obs_data_get_string(settings, "password"));
	plugin->use_auth   = obs_data_get_bool(settings, "use_auth");
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
	bfree(plugin->server);
	bfree(plugin->stream_key);
	bfree(plugin->username);
	bfree(plugin->password);
	bfree(plugin);
}

static bool handle_channel_selected(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	const char *id = obs_data_get_string(settings, "channel");
	if (!id || !*id) {
		return false;
	}

	struct dstr cur_id = {0};
	obs_data_array_t *channels = obs_data_get_array(settings, "channels");
	obs_data_t *channel = obs_data_array_item(channels, 0);

	for (size_t i = 0; channel; channel = obs_data_array_item(channels, ++i)) {
		rtmp_dacast_channel_id(&cur_id, channel);
		obs_data_release(channel);

		if (!dstr_is_empty(&cur_id) && dstr_cmp(&cur_id, id) == 0) {
			break;
		}
	}

	obs_data_array_release(channels);
	dstr_free(&cur_id);

	if (channel) {
		update_settings_for_channel(settings, channel);
	}

	return false;
}

static bool handle_refresh_channels_clicked(obs_properties_t *props, obs_property_t *prop, void *obj, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(obj);

	const char *api_key = obs_data_get_string(settings, "api_key");
	fetch_channels(props, settings, api_key);

	obs_property_t *p = obs_properties_get(props, "channel");
	handle_channel_selected(props, p, settings);

	if (p) {
		build_channel_list(p, settings);
	}

	return true;
}

static obs_properties_t *rtmp_dacast_properties(void *unused, obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	obs_property_t   *p;

	{
		obs_properties_add_text(props, "api_key",
			obs_module_text("ApiKey"),
			OBS_TEXT_PASSWORD
		);
	}

	{
		p = obs_properties_add_list(props, "channel",
			obs_module_text("Channel"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING
		);

		build_channel_list(p, settings);

		obs_property_set_modified_callback(p, handle_channel_selected);
	}

	{
		obs_properties_add_button(props, "refresh_channels",
			obs_module_text("RefreshChannels"),
			handle_refresh_channels_clicked
		);
	}

	{
		p = obs_properties_add_text(props, "error", NULL, OBS_TEXT_MULTILINE);
		obs_property_set_enabled(p, false);
		obs_property_set_visible(p, false);
	}

	{
		p = obs_properties_add_text(props, "server", "URL", OBS_TEXT_DEFAULT);
		obs_property_set_enabled(p, false);

		p = obs_properties_add_text(props, "stream_key", obs_module_text("StreamKey"), OBS_TEXT_PASSWORD);
		obs_property_set_enabled(p, false);
	}

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
