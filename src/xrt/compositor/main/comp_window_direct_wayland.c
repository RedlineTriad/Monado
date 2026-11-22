// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Wayland direct mode code.
 * @author Drew DeVault <sir@cmpwn.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Simon Zeni <simon@bl4ckb0ne.ca>
 * @ingroup comp
 */

#include <errno.h>
#include <linux/input.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm-lease-v1-client-protocol.h"
#include "os/os_time.h"
#include "xrt/xrt_compiler.h"
#include "main/comp_window.h"
#include "main/comp_window_direct.h"
#include "util/u_debug.h"

#ifndef VK_EXT_acquire_drm_display
#error "Wayland direct requires the Vulkan extension VK_EXT_acquire_drm_display"
#endif

DEBUG_GET_ONCE_OPTION(requested_connector, "XRT_COMPOSITOR_WAYLAND_CONNECTOR", NULL)

static const int64_t retry_delay_ns = 250 * U_TIME_1MS_IN_NS;
static const int64_t lease_reply_timeout_ns = U_TIME_1S_IN_NS;
static const int64_t long_disconnect_ns = INT64_C(60) * U_TIME_1S_IN_NS;

struct direct_wayland_lease
{
	struct comp_window_direct_wayland *w;

	int leased_fd;
	bool finished;
	struct wp_drm_lease_v1 *lease;
};

struct direct_wayland_lease_connector
{
	struct comp_window_direct_wayland *w;

	uint32_t id;
	char *name, *description;
	struct wp_drm_lease_connector_v1 *connector;
	struct direct_wayland_lease_device *dev;
	struct direct_wayland_lease_connector *next;
};

struct direct_wayland_lease_device
{
	struct comp_window_direct_wayland *w;

	int drm_fd;
	char *path;
	bool done;
	struct wp_drm_lease_device_v1 *device;
	struct direct_wayland_lease_connector *connectors;
	struct direct_wayland_lease_device *next;
};

struct comp_window_direct_wayland
{
	struct comp_target_swapchain base;

	struct wl_display *display;
	struct wl_registry *registry;
	struct direct_wayland_lease_device *devices;
	struct direct_wayland_lease *lease;

	// The DRM device and connector ID for the current lease.
	struct direct_wayland_lease_device *selected_device;
	uint32_t selected_connector_id;

	VkDisplayKHR vk_display;
	char *connector_name;
	char *connector_description;
	char *device_path;
	uint32_t preferred_width, preferred_height;
	bool needs_reconnect;
	bool logged_long_disconnect;
	int64_t disconnected_since_ns;
	int64_t next_retry_ns;
	int64_t next_status_check_ns;
	int64_t lease_requested_ns;

	VkResult (*swapchain_acquire)(struct comp_target *, uint32_t *);
	VkResult (*swapchain_present)(
	    struct comp_target *, struct vk_bundle_queue *, uint32_t, uint64_t, int64_t, int64_t);
	void (*create_swapchain_images)(struct comp_target *,
	                                const struct comp_target_create_images_info *,
	                                struct vk_bundle_queue *);
};

static void
direct_wayland_lease_connector_destroy(struct direct_wayland_lease_connector *conn)
{
	wp_drm_lease_connector_v1_destroy(conn->connector);
	free(conn->name);
	free(conn->description);
	free(conn);
}

static void
direct_wayland_lease_device_destroy(struct direct_wayland_lease_device *dev)
{
	struct direct_wayland_lease_connector *conn = dev->connectors, *next_conn;
	while (conn) {
		next_conn = conn->next;

		direct_wayland_lease_connector_destroy(conn);

		conn = next_conn;
	}

	wp_drm_lease_device_v1_destroy(dev->device);
	if (dev->drm_fd >= 0) {
		close(dev->drm_fd);
	}

	free(dev->path);
	free(dev);
}

static inline struct vk_bundle *
get_vk(struct comp_window_direct_wayland *cww)
{
	return &cww->base.base.c->base.vk;
}

static bool
connector_has_status(struct direct_wayland_lease_device *device, uint32_t id, drmModeConnection status)
{
	drmModeConnectorPtr connector = drmModeGetConnectorCurrent(device->drm_fd, id);
	if (connector == NULL) {
		return false;
	}
	bool matches = connector->connection == status;
	drmModeFreeConnector(connector);
	return matches;
}

static void
mark_output_lost(struct comp_window_direct_wayland *w)
{
	w->needs_reconnect = true;
	if (w->disconnected_since_ns == 0) {
		w->disconnected_since_ns = os_monotonic_get_ns();
		COMP_WARN(w->base.base.c, "Headset output lost; keeping the XR session running");
	}
}

static void
release_vk_display(struct comp_window_direct_wayland *w)
{
	if (w->vk_display != VK_NULL_HANDLE) {
		struct vk_bundle *vk = get_vk(w);
		vk->vkReleaseDisplayEXT(vk->physical_device, w->vk_display);
		w->vk_display = VK_NULL_HANDLE;
	}
}

static void
release_lease(struct comp_window_direct_wayland *w)
{
	if (w->lease == NULL) {
		return;
	}

	if (w->lease->leased_fd >= 0) {
		close(w->lease->leased_fd);
	}
	wp_drm_lease_v1_destroy(w->lease->lease);
	free(w->lease);
	w->lease = NULL;
	w->selected_device = NULL;
}

static void
disconnect_wayland(struct comp_window_direct_wayland *w)
{
	release_lease(w);
	while (w->devices != NULL) {
		struct direct_wayland_lease_device *dev = w->devices;
		w->devices = dev->next;
		direct_wayland_lease_device_destroy(dev);
	}
	if (w->registry != NULL) {
		wl_registry_destroy(w->registry);
		w->registry = NULL;
	}
	if (w->display != NULL) {
		wl_display_disconnect(w->display);
		w->display = NULL;
	}
}

static void
comp_window_direct_wayland_destroy(struct comp_target *w)
{
	struct comp_window_direct_wayland *w_wayland = (struct comp_window_direct_wayland *)w;
	comp_target_swapchain_cleanup(&w_wayland->base);
	release_vk_display(w_wayland);
	disconnect_wayland(w_wayland);
	free(w_wayland->connector_name);
	free(w_wayland->connector_description);
	free(w_wayland->device_path);
	free(w_wayland);
}

static void
_lease_fd(void *data, struct wp_drm_lease_v1 *wp_drm_lease_v1, int32_t leased_fd)
{
	struct direct_wayland_lease *lease = data;
	COMP_DEBUG(lease->w->base.base.c, "Lease granted");

	lease->leased_fd = leased_fd;
}

static void
_lease_finished(void *data, struct wp_drm_lease_v1 *wp_drm_lease_v1)
{
	struct direct_wayland_lease *lease = data;
	COMP_DEBUG(lease->w->base.base.c, "Lease has been closed");
	lease->finished = true;
	if (lease->w->base.surface.handle != VK_NULL_HANDLE) {
		mark_output_lost(lease->w);
	}
}

static const struct wp_drm_lease_v1_listener lease_listener = {
    .lease_fd = _lease_fd,
    .finished = _lease_finished,
};

static VkResult
comp_window_direct_wayland_create_surface(struct comp_window_direct_wayland *w,
                                          VkSurfaceKHR *surface,
                                          uint32_t width,
                                          uint32_t height)
{
	struct vk_bundle *vk = get_vk(w);
	w->vk_display = VK_NULL_HANDLE;
	VkDisplayKHR display = VK_NULL_HANDLE;

	if (w->selected_device == NULL || w->lease == NULL || w->lease->finished) {
		COMP_ERROR(w->base.base.c, "Connector was disconnected before it could be acquired");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	VkResult ret =
	    vk->vkGetDrmDisplayEXT(vk->physical_device, w->selected_device->drm_fd, w->selected_connector_id, &display);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(w->base.base.c, "vkGetDrmDisplayEXT failed: %s", vk_str_result(ret));
		return ret;
	}

	ret = vk->vkAcquireDrmDisplayEXT(vk->physical_device, w->lease->leased_fd, display);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(w->base.base.c, "vkAcquireDrmDisplayEXT failed: %s", vk_str_result(ret));
		return ret;
	}

	// Mesa closes the lease fd when releasing the acquired display.
	w->lease->leased_fd = -1;
	w->vk_display = display;
	ret = comp_window_direct_create_surface(&w->base, w->vk_display, width, height);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(w->base.base.c, "Failed to create surface: %s", vk_str_result(ret));
	}

	return ret;
}

static bool
comp_window_direct_wayland_init_swapchain(struct comp_target *w, uint32_t width, uint32_t height)
{
	struct comp_window_direct_wayland *w_wayland = (struct comp_window_direct_wayland *)w;
	w_wayland->preferred_width = width;
	w_wayland->preferred_height = height;

	VkResult ret =
	    comp_window_direct_wayland_create_surface(w_wayland, &w_wayland->base.surface.handle, width, height);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(w->c, "Failed to create surface!");
		return false;
	}

	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)w_wayland;
	cts->display = w_wayland->vk_display;

	return true;
}

static void
comp_window_direct_wayland_flush(struct comp_target *w)
{
	struct comp_window_direct_wayland *w_wayland = (struct comp_window_direct_wayland *)w;
	if (w_wayland->display == NULL) {
		return;
	}

	while (wl_display_prepare_read(w_wayland->display) != 0) {
		if (wl_display_dispatch_pending(w_wayland->display) < 0) {
			mark_output_lost(w_wayland);
			return;
		}
	}

	if (wl_display_flush(w_wayland->display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(w_wayland->display);
		mark_output_lost(w_wayland);
		return;
	}

	struct pollfd fds[] = {
	    {
	        .fd = wl_display_get_fd(w_wayland->display),
	        .events = POLLIN,
	    },
	};

	if (poll(fds, 1, 0) > 0) {
		if (wl_display_read_events(w_wayland->display) < 0 ||
		    wl_display_dispatch_pending(w_wayland->display) < 0) {
			mark_output_lost(w_wayland);
		}
	} else {
		wl_display_cancel_read(w_wayland->display);
	}

	// A compositor can keep the lease alive after the DRM connector disconnects.
	int64_t now = os_monotonic_get_ns();
	if (!w_wayland->needs_reconnect && w_wayland->selected_device != NULL &&
	    now >= w_wayland->next_status_check_ns) {
		w_wayland->next_status_check_ns = now + retry_delay_ns;
		if (connector_has_status(w_wayland->selected_device, w_wayland->selected_connector_id,
		                         DRM_MODE_DISCONNECTED)) {
			mark_output_lost(w_wayland);
		}
	}
}

static void
_lease_connector_name(void *data, struct wp_drm_lease_connector_v1 *wp_drm_lease_connector_v1, const char *name)
{
	struct direct_wayland_lease_connector *conn = data;
	conn->name = strdup(name);
}

static void
_lease_connector_description(void *data,
                             struct wp_drm_lease_connector_v1 *wp_drm_lease_connector_v1,
                             const char *description)
{
	struct direct_wayland_lease_connector *conn = data;
	if (conn->description) {
		free(conn->description);
	}
	conn->description = strdup(description);
}

static void
_lease_connector_connector_id(void *data,
                              struct wp_drm_lease_connector_v1 *wp_drm_lease_connector_v1,
                              uint32_t connector_id)
{
	struct direct_wayland_lease_connector *conn = data;
	conn->id = connector_id;
}

static void
_lease_connector_done(void *data, struct wp_drm_lease_connector_v1 *wp_drm_lease_connector_v1)
{
	struct direct_wayland_lease_connector *conn = data;
	COMP_INFO(conn->w->base.base.c, "[%s] connector %s (%s) id: %d", conn->dev->path, conn->name, conn->description,
	          conn->id);
}

static void
_lease_connector_withdrawn(void *data, struct wp_drm_lease_connector_v1 *wp_drm_lease_connector_v1)
{
	struct direct_wayland_lease_connector *conn = data;
	COMP_INFO(conn->w->base.base.c, "Connector %s has been withdrawn by the compositor", conn->name);
	struct comp_window_direct_wayland *w = conn->w;
	// Compositors can withdraw an offer while a working lease still exists.
	if (conn->dev == w->selected_device && conn->id == w->selected_connector_id &&
	    w->base.surface.handle != VK_NULL_HANDLE &&
	    connector_has_status(conn->dev, conn->id, DRM_MODE_DISCONNECTED)) {
		mark_output_lost(w);
	}
	struct direct_wayland_lease_connector **link = &conn->dev->connectors;
	while (*link != conn) {
		link = &(*link)->next;
	}
	*link = conn->next;
	direct_wayland_lease_connector_destroy(conn);
}

static const struct wp_drm_lease_connector_v1_listener lease_connector_listener = {
    .name = _lease_connector_name,
    .description = _lease_connector_description,
    .connector_id = _lease_connector_connector_id,
    .done = _lease_connector_done,
    .withdrawn = _lease_connector_withdrawn,
};

static void
_drm_lease_device_drm_fd(void *data, struct wp_drm_lease_device_v1 *drm_lease_device, int fd)
{
	struct direct_wayland_lease_device *dev = data;
	dev->drm_fd = fd;
	dev->path = drmGetDeviceNameFromFd2(fd);
	COMP_INFO(dev->w->base.base.c, "Available DRM lease device: %s", dev->path);
}

static void
_drm_lease_device_connector(void *data,
                            struct wp_drm_lease_device_v1 *wp_drm_lease_device_v1,
                            struct wp_drm_lease_connector_v1 *wp_drm_lease_connector_v1)
{
	struct direct_wayland_lease_device *dev = data;

	struct direct_wayland_lease_connector *conn = calloc(1, sizeof(struct direct_wayland_lease_connector));
	conn->connector = wp_drm_lease_connector_v1;
	conn->dev = dev;
	conn->w = dev->w;
	wp_drm_lease_connector_v1_add_listener(conn->connector, &lease_connector_listener, conn);

	conn->next = dev->connectors;
	dev->connectors = conn;
}

static void
_drm_lease_device_done(void *data, struct wp_drm_lease_device_v1 *wp_drm_lease_device_v1)
{
	struct direct_wayland_lease_device *dev = data;
	dev->done = true;
}

static void
_drm_lease_device_released(void *data, struct wp_drm_lease_device_v1 *wp_drm_lease_device_v1)
{
	struct direct_wayland_lease_device *dev = data;
	COMP_ERROR(dev->w->base.base.c, "Releasing lease device %s", dev->path);
	struct comp_window_direct_wayland *w = dev->w;
	if (dev == w->selected_device) {
		w->selected_device = NULL;
		if (w->base.surface.handle != VK_NULL_HANDLE) {
			mark_output_lost(w);
		}
	}
	struct direct_wayland_lease_device **link = &w->devices;
	while (*link != dev) {
		link = &(*link)->next;
	}
	*link = dev->next;
	direct_wayland_lease_device_destroy(dev);
}

static const struct wp_drm_lease_device_v1_listener drm_lease_device_listener = {
    .drm_fd = _drm_lease_device_drm_fd,
    .connector = _drm_lease_device_connector,
    .done = _drm_lease_device_done,
    .released = _drm_lease_device_released,
};

static void
_registry_global_remove_cb(void *data, struct wl_registry *registry, uint32_t name)
{}

static void
_registry_global_cb(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct comp_window_direct_wayland *w = data;
	if (strcmp(interface, wp_drm_lease_device_v1_interface.name) == 0) {
		struct direct_wayland_lease_device *dev = calloc(1, sizeof(struct direct_wayland_lease_device));
		dev->device = (struct wp_drm_lease_device_v1 *)wl_registry_bind(registry, name,
		                                                                &wp_drm_lease_device_v1_interface, 1);
		dev->w = w;
		dev->drm_fd = -1;
		wp_drm_lease_device_v1_add_listener(dev->device, &drm_lease_device_listener, dev);

		dev->next = w->devices;
		w->devices = dev;
	}
}

static const struct wl_registry_listener registry_listener = {
    .global = _registry_global_cb,
    .global_remove = _registry_global_remove_cb,
};

static struct direct_wayland_lease_connector *
find_connector(struct comp_window_direct_wayland *w, const char *name, bool reconnect)
{
	struct direct_wayland_lease_connector *first = NULL;
	for (struct direct_wayland_lease_device *dev = w->devices; dev != NULL; dev = dev->next) {
		if (!dev->done || dev->path == NULL || (reconnect && strcmp(dev->path, w->device_path) != 0)) {
			continue;
		}
		for (struct direct_wayland_lease_connector *conn = dev->connectors; conn != NULL; conn = conn->next) {
			if (conn->name == NULL) {
				continue;
			}
			if (reconnect && w->connector_description != NULL &&
			    (conn->description == NULL || strcmp(conn->description, w->connector_description) != 0)) {
				continue;
			}
			if (name == NULL || strcmp(conn->name, name) == 0) {
				return conn;
			}
			if (first == NULL) {
				first = conn;
			}
		}
	}
	return reconnect ? NULL : first;
}

static bool
connect_wayland(struct comp_window_direct_wayland *w)
{
	w->display = wl_display_connect(NULL);
	if (w->display == NULL) {
		return false;
	}
	w->registry = wl_display_get_registry(w->display);
	if (w->registry == NULL) {
		disconnect_wayland(w);
		return false;
	}
	wl_registry_add_listener(w->registry, &registry_listener, w);
	return true;
}

static bool
request_lease(struct comp_window_direct_wayland *w, struct direct_wayland_lease_connector *conn)
{
	struct direct_wayland_lease *lease = calloc(1, sizeof(struct direct_wayland_lease));
	if (lease == NULL) {
		return false;
	}
	struct wp_drm_lease_request_v1 *request = wp_drm_lease_device_v1_create_lease_request(conn->dev->device);
	if (request == NULL) {
		free(lease);
		return false;
	}
	wp_drm_lease_request_v1_request_connector(request, conn->connector);
	lease->lease = wp_drm_lease_request_v1_submit(request);
	if (lease->lease == NULL) {
		free(lease);
		return false;
	}
	lease->w = w;
	lease->leased_fd = -1;
	w->selected_device = conn->dev;
	w->selected_connector_id = conn->id;
	w->lease = lease;
	w->lease_requested_ns = os_monotonic_get_ns();
	wp_drm_lease_v1_add_listener(lease->lease, &lease_listener, lease);
	return true;
}

static bool
comp_window_direct_wayland_init(struct comp_target *w)
{
	struct comp_window_direct_wayland *wayland = (struct comp_window_direct_wayland *)w;
	if (!connect_wayland(wayland) || wl_display_roundtrip(wayland->display) < 0 ||
	    wl_display_roundtrip(wayland->display) < 0) {
		COMP_ERROR(w->c, "Failed to discover Wayland DRM lease devices");
		return false;
	}

	if (wayland->devices == NULL) {
		COMP_ERROR(w->c, "Compositor is missing drm-lease support");
		return false;
	}

	const char *name = debug_get_option_requested_connector();
	struct direct_wayland_lease_connector *conn = find_connector(wayland, name, false);
	if (conn == NULL) {
		COMP_ERROR(w->c, "Found no connectors available for direct mode");
		return false;
	}
	if (name != NULL && strcmp(name, conn->name) != 0) {
		COMP_WARN(w->c, "Could not find requested connector %s, using %s", name, conn->name);
	}
	COMP_INFO(w->c, "Using DRM node %s, connector %s", conn->dev->path, conn->name);
	wayland->connector_name = strdup(conn->name);
	wayland->device_path = strdup(conn->dev->path);
	if (conn->description != NULL) {
		wayland->connector_description = strdup(conn->description);
	}
	if (wayland->connector_name == NULL || wayland->device_path == NULL ||
	    (conn->description != NULL && wayland->connector_description == NULL) || !request_lease(wayland, conn)) {
		return false;
	}

	struct direct_wayland_lease *lease = wayland->lease;
	while (lease->leased_fd < 0 && !lease->finished) {
		if (wl_display_dispatch(wayland->display) == -1) {
			COMP_ERROR(w->c, "Wayland lease dispatch failed");
			return false;
		}
	}

	if (lease->finished) {
		COMP_ERROR(w->c, "Failed to lease connector");
		return false;
	}

	return true;
}

static bool
comp_window_direct_wayland_check_ready(struct comp_target *ct)
{
	struct comp_window_direct_wayland *w = (struct comp_window_direct_wayland *)ct;
	return !w->needs_reconnect && w->base.surface.handle != VK_NULL_HANDLE;
}

static void
comp_window_direct_wayland_reconnect(struct comp_target *ct)
{
	struct comp_window_direct_wayland *w = (struct comp_window_direct_wayland *)ct;
	int64_t now = os_monotonic_get_ns();
	if (w->disconnected_since_ns != 0 && !w->logged_long_disconnect &&
	    now - w->disconnected_since_ns >= long_disconnect_ns) {
		COMP_WARN(ct->c, "Headset output absent for 60 seconds; still waiting for %s", w->connector_name);
		w->logged_long_disconnect = true;
	}
	if (w->base.surface.handle != VK_NULL_HANDLE || w->vk_display != VK_NULL_HANDLE) {
		comp_target_swapchain_reset_output(&w->base);
		release_vk_display(w);
		release_lease(w);
	}
	if (wl_display_get_error(w->display) != 0 || now < w->next_retry_ns) {
		return;
	}
	if (w->lease == NULL) {
		struct direct_wayland_lease_connector *conn = find_connector(w, w->connector_name, true);
		if (conn != NULL && connector_has_status(conn->dev, conn->id, DRM_MODE_CONNECTED)) {
			request_lease(w, conn);
		}
		w->next_retry_ns = now + retry_delay_ns;
		return;
	}

	if (!w->lease->finished) {
		if (w->lease->leased_fd >= 0) {
			if (comp_window_direct_wayland_init_swapchain(ct, w->preferred_width, w->preferred_height)) {
				w->needs_reconnect = false;
				return;
			}
		} else if (now - w->lease_requested_ns < lease_reply_timeout_ns) {
			return;
		}
	}
	release_vk_display(w);
	release_lease(w);
	w->next_retry_ns = now + retry_delay_ns;
}

static bool
surface_lost(VkResult ret)
{
	return ret == VK_ERROR_SURFACE_LOST_KHR || ret == VK_ERROR_OUT_OF_DATE_KHR;
}

static VkResult
comp_window_direct_wayland_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct comp_window_direct_wayland *w = (struct comp_window_direct_wayland *)ct;
	VkResult ret = w->swapchain_acquire(ct, out_index);
	if (surface_lost(ret)) {
		mark_output_lost(w);
	}
	return ret;
}

static VkResult
comp_window_direct_wayland_present(struct comp_target *ct,
                                   struct vk_bundle_queue *queue,
                                   uint32_t index,
                                   uint64_t semaphore_value,
                                   int64_t desired_time_ns,
                                   int64_t slop_ns)
{
	struct comp_window_direct_wayland *w = (struct comp_window_direct_wayland *)ct;
	VkResult ret = w->swapchain_present(ct, queue, index, semaphore_value, desired_time_ns, slop_ns);
	if (surface_lost(ret)) {
		mark_output_lost(w);
	} else if ((ret == VK_SUCCESS || ret == VK_SUBOPTIMAL_KHR) && w->disconnected_since_ns != 0) {
		COMP_INFO(ct->c, "Headset output presenting again on %s after %.2f seconds", w->connector_name,
		          time_ns_to_ms_f(os_monotonic_get_ns() - w->disconnected_since_ns) / 1000.0);
		w->disconnected_since_ns = 0;
		w->logged_long_disconnect = false;
	}
	return ret;
}

static void
comp_window_direct_wayland_create_images(struct comp_target *ct,
                                         const struct comp_target_create_images_info *info,
                                         struct vk_bundle_queue *queue)
{
	struct comp_window_direct_wayland *w = (struct comp_window_direct_wayland *)ct;
	w->create_swapchain_images(ct, info, queue);
	if (!comp_target_has_images(ct)) {
		mark_output_lost(w);
	}
}

static void
_update_window_title(struct comp_target *ct, const char *title)
{
	/* Not required in direct mode */
}

struct comp_target *
comp_window_direct_wayland_create(struct comp_compositor *c)
{
	struct comp_window_direct_wayland *w = U_TYPED_CALLOC(struct comp_window_direct_wayland);

	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);
	w->base.acquire_timeout_ns = 100 * U_TIME_1MS_IN_NS;

	w->swapchain_acquire = w->base.base.acquire;
	w->swapchain_present = w->base.base.present;
	w->create_swapchain_images = w->base.base.create_images;
	w->base.base.acquire = comp_window_direct_wayland_acquire;
	w->base.base.present = comp_window_direct_wayland_present;
	w->base.base.create_images = comp_window_direct_wayland_create_images;
	w->base.base.check_ready = comp_window_direct_wayland_check_ready;
	w->base.base.reconnect = comp_window_direct_wayland_reconnect;

	w->base.base.name = "wayland-direct";
	w->base.display = VK_NULL_HANDLE;
	w->base.base.destroy = comp_window_direct_wayland_destroy;
	w->base.base.flush = comp_window_direct_wayland_flush;
	w->base.base.init_pre_vulkan = comp_window_direct_wayland_init;
	w->base.base.init_post_vulkan = comp_window_direct_wayland_init_swapchain;
	w->base.base.set_title = _update_window_title;
	w->base.base.c = c;

	return &w->base.base;
}


/*
 *
 * Factory
 *
 */

static const char *instance_extensions[] = {
    VK_KHR_DISPLAY_EXTENSION_NAME,             //
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,     //
    VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME, //

#ifdef VK_EXT_acquire_drm_display
    VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME,
#endif
};

static bool
detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	return false;
}

static bool
create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_target *ct = comp_window_direct_wayland_create(c);
	if (ct == NULL) {
		return false;
	}

	*out_ct = ct;

	return true;
}

const struct comp_target_factory comp_target_factory_direct_wayland = {
    .name = "Wayland Direct-Mode",
    .identifier = "direct_wayland",
    .requires_vulkan_for_create = false,
    .is_deferred = false,
    .required_instance_version = 0,
    .required_instance_extensions = instance_extensions,
    .required_instance_extension_count = ARRAY_SIZE(instance_extensions),
    .optional_device_extensions = NULL,
    .optional_device_extension_count = 0,
    .detect = detect,
    .create_target = create_target,
};
