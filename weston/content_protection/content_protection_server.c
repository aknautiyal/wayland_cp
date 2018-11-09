/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "compositor.h"
#include "compositor/weston.h"
#include "content-protection-server-protocol.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"

#define OBSERVE_TIME_USEC 1000
#define WAIT_FOR_DISABLE_SEC 1
#define MAX_USER_RETRY 3
struct content_protection_t *cp;
extern int32_t weston_set_cp(struct weston_compositor *compositor,
			       int enable, int32_t content_type);
extern int32_t weston_get_cp(struct weston_compositor *compositor,
			       bool *enable, int32_t content_type);
enum cp_request_status {
	CP_UNDESIRED,
	CP_DESIRED,
	CP_ENABLED,
	CP_FAILED
};

struct content_protection_t {
	struct weston_compositor *compositor;
	int protection_status;
	int protection_required;
	enum cp_request_status status;
	enum content_protection_type type;
	struct wl_event_source *observe_event_source;
	struct wl_event_source *retry_event_source;
	struct wl_resource *resource;
	int retries_left;
	int num_sec_elapsed;
};

#define EBUSY	16

static int
request_cp_state(struct weston_compositor *wc, int cp_request, int32_t type)
{
	int ret, retry = 1;

	do {
		ret = weston_set_cp(wc, cp_request, type);
		if (ret == -EBUSY) {
			sleep(1);
		} else if (ret < 0) {
			weston_log("weston_set_cp failed %d\n", ret);
			return ret;
		} else {
			break;
		}
	} while (--retry);
	return ret;
}
static int retry_cp(void *data)
{
	struct content_protection_t *cp = data;
	bool enable = true;
	int ret;

	if (cp->retries_left <= 0) {
		weston_log("%d retries finished\n", MAX_USER_RETRY);
		return -1;
	}
	ret = weston_get_cp(cp->compositor, &enable, cp->type);
	if (ret < 0) {
		cp->status = CP_FAILED;
		weston_log("Failed at get_cp. %d\n", ret);
		return -1;
	}
	if (enable) {
		cp->status = CP_ENABLED;
		content_protection_send_status_changed(cp->resource, cp->type);
		wl_event_source_timer_update(cp->observe_event_source,
					     OBSERVE_TIME_USEC);
	} else if (cp->num_sec_elapsed < (MAX_USER_RETRY * 5)) {
		cp->num_sec_elapsed++;
		wl_event_source_timer_update(cp->retry_event_source, 1000);
	} else {
		weston_log("CP::Retry #%d\n", MAX_USER_RETRY - cp->retries_left);
		ret = request_cp_state(cp->compositor, CP_DESIRED, cp->type);
		if (ret < 0)
			return -1;
		cp->retries_left--;
		cp->num_sec_elapsed = 0;
		wl_event_source_timer_update(cp->retry_event_source, 1000);
	}
	return 0;
}
static int observe_cp_state(void *data)
{
	struct content_protection_t *cp = data;
	int ret;
	bool enable = true;

	ret = weston_get_cp(cp->compositor, &enable, cp->type);
	if (ret < 0) {
		weston_log("Failed to get CP status. %d\n", ret);
		return -1;
	}
	if (cp->status == CP_ENABLED) {
		if (enable)
			wl_event_source_timer_update(cp->observe_event_source,
						     OBSERVE_TIME_USEC);
		else {
			/**Content-Protection Failed due to runtime error or
			 * hotplug events
			 */
			cp->status = CP_FAILED;
			content_protection_send_status_changed(cp->resource, CONTENT_PROTECTION_TYPE_UNPROTECTED);
		}
	}
	return 0;
}

void desired(struct wl_client *client, struct wl_resource *resource,
	     int32_t content_type)
{
	int32_t ret;
	bool status;

	cp->resource = resource;
	if ((cp->status == CP_DESIRED || cp->status == CP_ENABLED) &&
	   cp->type == (enum content_protection_type)content_type)
		return;

	weston_log("Content-Protection Desired\n");
	ret = weston_get_cp(cp->compositor, &status, content_type);
	if (ret < 0) {
		weston_log("get_cp failed %d\n", ret);
		return;
	}

	if (status) {
		weston_log("Already Enabled\n");
		cp->status = CP_ENABLED;
		content_protection_send_status_changed(cp->resource, cp->type);
		wl_event_source_timer_update(cp->observe_event_source,
					     OBSERVE_TIME_USEC);
		return;
	}

	/* disarm the observer timer */
	wl_event_source_timer_update(cp->observe_event_source, 0);
	ret = request_cp_state(cp->compositor, CP_DESIRED, content_type);
	if (ret < 0)
		return;

	cp->status = CP_DESIRED;
	cp->type = content_type;
	cp->retries_left = MAX_USER_RETRY;
	/* start the retry timer */
	wl_event_source_timer_update(cp->retry_event_source, 100);
}
void disable(struct wl_client *client, struct wl_resource *resource)
{
	int32_t ret;
	bool status = true;

	cp->resource = resource;
	if (cp->status == CP_UNDESIRED)
		return;

	ret = weston_get_cp(cp->compositor, &status, 0);
	if (ret < 0) {
		weston_log("get_cp failed %d\n", ret);
		return;
	}

	weston_log("Content-Protection Disable Requested\n");
	if (!status) {
		weston_log("Already Disabled\n");
		cp->status = CP_UNDESIRED;
		return;
	}

	ret = request_cp_state(cp->compositor, CP_UNDESIRED, 0);
	if (ret < 0) {
		weston_log("CP Disable request failed %d\n", ret);
		return;
	}

	cp->status = CP_UNDESIRED;
	weston_log("Disabling content-protection...\n");
	content_protection_send_status_changed(cp->resource, CONTENT_PROTECTION_TYPE_UNPROTECTED);
	wl_event_source_timer_update(cp->observe_event_source, 0);
	wl_event_source_timer_update(cp->retry_event_source, 0);
	usleep(WAIT_FOR_DISABLE_SEC);
}

static const struct content_protection_interface cp_implementation = {
	desired,
	disable,
};

static void bind_cp_module(struct wl_client *client, void *data,
			   uint32_t version, uint32_t id)
{
	struct content_protection_t *cp = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &content_protection_interface,
				      1, id);
	wl_resource_set_implementation(resource, &cp_implementation, cp, NULL);
}

WL_EXPORT int
wet_module_init(struct weston_compositor *ec, int *argc, char *argv[])
{
	struct wl_event_loop *loop_observe_cp, *loop_retry_cp;

	cp = zalloc(sizeof(*cp));
	if (cp == NULL)
		return -1;
	cp->compositor = ec;
	cp->status = CP_FAILED;

	if (wl_global_create(ec->wl_display, &content_protection_interface, 1,
				cp, bind_cp_module) == NULL)
		return -1;
	loop_observe_cp = wl_display_get_event_loop(ec->wl_display);
	loop_retry_cp = wl_display_get_event_loop(ec->wl_display);
	cp->observe_event_source = wl_event_loop_add_timer(loop_observe_cp,
							   observe_cp_state,
							   cp);
	cp->retry_event_source = wl_event_loop_add_timer(loop_retry_cp,
							 retry_cp, cp);
	cp->retries_left = MAX_USER_RETRY;
	return 0;
}
