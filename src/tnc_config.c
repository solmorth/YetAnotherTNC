#include "tnc_config.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#define TNC_CONFIG_BEACON_MIN_S     10
#define TNC_CONFIG_BEACON_MAX_S     3600
#define TNC_CONFIG_BEACON_DEFAULT_S 60
#define TNC_CONFIG_COMMENT_MAX_LEN  32

static struct {
	ax25_address_t callsign;
	uint32_t beacon_interval_s;
	char comment[TNC_CONFIG_COMMENT_MAX_LEN + 1];
	bool fixed_pos_enabled;
	float fixed_lat; /* NAN until SET LAT is received - see fixed_pos_configured */
	float fixed_lon; /* NAN until SET LON is received */
} cfg = {
	.beacon_interval_s = TNC_CONFIG_BEACON_DEFAULT_S,
	.fixed_lat = NAN,
	.fixed_lon = NAN,
};

static int tnc_config_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "call") == 0 && len == sizeof(cfg.callsign)) {
		return read_cb(cb_arg, &cfg.callsign, sizeof(cfg.callsign)) < 0 ? -EINVAL : 0;
	}
	if (strcmp(name, "beacon") == 0 && len == sizeof(cfg.beacon_interval_s)) {
		return read_cb(cb_arg, &cfg.beacon_interval_s, sizeof(cfg.beacon_interval_s)) < 0 ? -EINVAL : 0;
	}
	if (strcmp(name, "comment") == 0 && len == sizeof(cfg.comment)) {
		return read_cb(cb_arg, &cfg.comment, sizeof(cfg.comment)) < 0 ? -EINVAL : 0;
	}
	if (strcmp(name, "fixedpos") == 0 && len == sizeof(cfg.fixed_pos_enabled)) {
		return read_cb(cb_arg, &cfg.fixed_pos_enabled, sizeof(cfg.fixed_pos_enabled)) < 0
			       ? -EINVAL
			       : 0;
	}
	if (strcmp(name, "fixedlat") == 0 && len == sizeof(cfg.fixed_lat)) {
		return read_cb(cb_arg, &cfg.fixed_lat, sizeof(cfg.fixed_lat)) < 0 ? -EINVAL : 0;
	}
	if (strcmp(name, "fixedlon") == 0 && len == sizeof(cfg.fixed_lon)) {
		return read_cb(cb_arg, &cfg.fixed_lon, sizeof(cfg.fixed_lon)) < 0 ? -EINVAL : 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(tnc_config, "tnc", NULL, tnc_config_settings_set, NULL, NULL);

void tnc_config_init(void)
{
	ax25_parse_callsign("N0CALL", &cfg.callsign);

	int err = settings_subsys_init();
	if (err) {
		printk("[CFG] settings_subsys_init failed: %d, using defaults\n", err);
		return;
	}

	settings_load_subtree("tnc");

	char buf[16];
	ax25_format_callsign(&cfg.callsign, buf, sizeof(buf));
	printk("[CFG] Loaded: CALL=%s BEACON=%us COMMENT=%s\n", buf, cfg.beacon_interval_s, cfg.comment);
}

const ax25_address_t *tnc_config_get_callsign(void)
{
	return &cfg.callsign;
}

uint32_t tnc_config_get_beacon_interval_s(void)
{
	return cfg.beacon_interval_s;
}

const char *tnc_config_get_comment(void)
{
	return cfg.comment;
}

bool tnc_config_get_fixed_pos_enabled(void)
{
	return cfg.fixed_pos_enabled;
}

bool tnc_config_get_fixed_pos_configured(void)
{
	return !isnan(cfg.fixed_lat) && !isnan(cfg.fixed_lon);
}

float tnc_config_get_fixed_lat(void)
{
	return cfg.fixed_lat;
}

float tnc_config_get_fixed_lon(void)
{
	return cfg.fixed_lon;
}

int tnc_config_handle_command(const uint8_t *data, size_t len, char *resp, size_t resp_len)
{
	char line[64];
	size_t n = MIN(len, sizeof(line) - 1);

	memcpy(line, data, n);
	line[n] = '\0';
	while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) {
		line[--n] = '\0';
	}

	if (strncmp(line, "SET CALL=", 9) == 0) {
		ax25_address_t addr;

		if (ax25_parse_callsign(line + 9, &addr) != 0 || addr.callsign[0] == '\0') {
			snprintf(resp, resp_len, "ERR bad callsign");
			return 0;
		}
		cfg.callsign = addr;
		settings_save_one("tnc/call", &cfg.callsign, sizeof(cfg.callsign));

		char buf[16];
		ax25_format_callsign(&cfg.callsign, buf, sizeof(buf));
		snprintf(resp, resp_len, "OK CALL=%s", buf);
		return 0;
	}

	if (strncmp(line, "SET BEACON=", 11) == 0) {
		long v = strtol(line + 11, NULL, 10);

		if (v < TNC_CONFIG_BEACON_MIN_S) {
			v = TNC_CONFIG_BEACON_MIN_S;
		} else if (v > TNC_CONFIG_BEACON_MAX_S) {
			v = TNC_CONFIG_BEACON_MAX_S;
		}
		cfg.beacon_interval_s = (uint32_t)v;
		settings_save_one("tnc/beacon", &cfg.beacon_interval_s, sizeof(cfg.beacon_interval_s));
		snprintf(resp, resp_len, "OK BEACON=%u", cfg.beacon_interval_s);
		return 0;
	}

	if (strncmp(line, "SET COMMENT=", 12) == 0) {
		const char *val = line + 12;
		size_t val_len = strlen(val);

		if (val_len > TNC_CONFIG_COMMENT_MAX_LEN) {
			val_len = TNC_CONFIG_COMMENT_MAX_LEN;
		}
		memcpy(cfg.comment, val, val_len);
		cfg.comment[val_len] = '\0';
		settings_save_one("tnc/comment", &cfg.comment, sizeof(cfg.comment));
		snprintf(resp, resp_len, "OK COMMENT=%s", cfg.comment);
		return 0;
	}

	if (strncmp(line, "SET FIXEDPOS=", 13) == 0) {
		const char *val = line + 13;
		bool enabled;

		if (strcmp(val, "ON") == 0) {
			enabled = true;
		} else if (strcmp(val, "OFF") == 0) {
			enabled = false;
		} else {
			snprintf(resp, resp_len, "ERR bad fixedpos");
			return 0;
		}
		cfg.fixed_pos_enabled = enabled;
		settings_save_one("tnc/fixedpos", &cfg.fixed_pos_enabled, sizeof(cfg.fixed_pos_enabled));
		snprintf(resp, resp_len, "OK FIXEDPOS=%s", enabled ? "ON" : "OFF");
		return 0;
	}

	if (strncmp(line, "SET LAT=", 8) == 0) {
		char *end;
		float v = strtof(line + 8, &end);

		if (end == line + 8 || v < -90.0f || v > 90.0f) {
			snprintf(resp, resp_len, "ERR bad lat");
			return 0;
		}
		cfg.fixed_lat = v;
		settings_save_one("tnc/fixedlat", &cfg.fixed_lat, sizeof(cfg.fixed_lat));
		snprintf(resp, resp_len, "OK LAT=%.5f", (double)cfg.fixed_lat);
		return 0;
	}

	if (strncmp(line, "SET LON=", 8) == 0) {
		char *end;
		float v = strtof(line + 8, &end);

		if (end == line + 8 || v < -180.0f || v > 180.0f) {
			snprintf(resp, resp_len, "ERR bad lon");
			return 0;
		}
		cfg.fixed_lon = v;
		settings_save_one("tnc/fixedlon", &cfg.fixed_lon, sizeof(cfg.fixed_lon));
		snprintf(resp, resp_len, "OK LON=%.5f", (double)cfg.fixed_lon);
		return 0;
	}

	if (strcmp(line, "GET") == 0) {
		char buf[16];
		char lat_buf[16] = "";
		char lon_buf[16] = "";

		if (!isnan(cfg.fixed_lat)) {
			snprintf(lat_buf, sizeof(lat_buf), "%.5f", (double)cfg.fixed_lat);
		}
		if (!isnan(cfg.fixed_lon)) {
			snprintf(lon_buf, sizeof(lon_buf), "%.5f", (double)cfg.fixed_lon);
		}

		ax25_format_callsign(&cfg.callsign, buf, sizeof(buf));
		/* COMMENT stays last: it's the only free-text field, and the
		 * configurator app parses it out with a greedy to-end-of-line
		 * regex (safe only because nothing follows it here).
		 */
		snprintf(resp, resp_len, "CALL=%s BEACON=%u FIXEDPOS=%s LAT=%s LON=%s COMMENT=%s",
			 buf, cfg.beacon_interval_s, cfg.fixed_pos_enabled ? "ON" : "OFF", lat_buf,
			 lon_buf, cfg.comment);
		return 0;
	}

	snprintf(resp, resp_len, "ERR unknown command");
	return 0;
}
