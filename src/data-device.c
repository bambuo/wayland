/*
 * Copyright © 2011 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "wayland-server.h"

static void
data_offer_accept(struct wl_client *client, struct wl_resource *resource,
		  uint32_t serial, const char *mime_type)
{
	struct wl_data_offer *offer = resource->data;

	/* FIXME: Check that client is currently focused by the input
	 * device that is currently dragging this data source.  Should
	 * this be a wl_data_device request? */

	if (offer->source)
		wl_data_source_send_target(&offer->source->resource,
					   mime_type);
}

static void
data_offer_receive(struct wl_client *client, struct wl_resource *resource,
		   const char *mime_type, int32_t fd)
{
	struct wl_data_offer *offer = resource->data;

	if (offer->source)
		wl_data_source_send_send(&offer->source->resource,
					 mime_type, fd);

	close(fd);
}

static void
data_offer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
destroy_data_offer(struct wl_resource *resource)
{
	struct wl_data_offer *offer = resource->data;

	wl_list_remove(&offer->source_destroy_listener.link);
	free(offer);
}

static const struct wl_data_offer_interface data_offer_interface = {
	data_offer_accept,
	data_offer_receive,
	data_offer_destroy,
};

static void
destroy_offer_data_source(struct wl_listener *listener, void *data)
{
	struct wl_data_offer *offer;

	offer = container_of(listener, struct wl_data_offer,
			     source_destroy_listener);

	offer->source = NULL;
}

static void
data_source_cancel(struct wl_data_source *source)
{
	wl_data_source_send_cancelled(&source->resource);
}

static struct wl_resource *
wl_data_source_send_offer(struct wl_data_source *source,
			  struct wl_resource *target)
{
	struct wl_data_offer *offer;
	char **p;

	offer = malloc(sizeof *offer);
	if (offer == NULL)
		return NULL;

	offer->resource.destroy = destroy_data_offer;
	offer->resource.object.id = 0;
	offer->resource.object.interface = &wl_data_offer_interface;
	offer->resource.object.implementation =
		(void (**)(void)) source->offer_interface;
	offer->resource.data = offer;
	wl_signal_init(&offer->resource.destroy_signal);

	offer->source = source;
	offer->source_destroy_listener.notify = destroy_offer_data_source;
	wl_signal_add(&source->resource.destroy_signal,
		      &offer->source_destroy_listener);

	wl_client_add_resource(target->client, &offer->resource);

	wl_data_device_send_data_offer(target, &offer->resource);

	wl_array_for_each(p, &source->mime_types)
		wl_data_offer_send_offer(&offer->resource, *p);

	return &offer->resource;
}

static void
data_source_offer(struct wl_client *client,
		  struct wl_resource *resource,
		  const char *type)
{
	struct wl_data_source *source = resource->data;
	char **p;

	p = wl_array_add(&source->mime_types, sizeof *p);
	if (p)
		*p = strdup(type);
	if (!p || !*p)
		wl_resource_post_no_memory(resource);
}

static void
data_source_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static struct wl_data_source_interface data_source_interface = {
	data_source_offer,
	data_source_destroy
};

static struct wl_resource *
find_resource(struct wl_list *list, struct wl_client *client)
{
	struct wl_resource *r;

	wl_list_for_each(r, list, link) {
		if (r->client == client)
			return r;
	}

	return NULL;
}

static void
destroy_drag_focus(struct wl_listener *listener, void *data)
{
	struct wl_input_device *device =
		container_of(listener, struct wl_input_device,
			     drag_focus_listener);

	device->drag_focus_resource = NULL;
}

static void
drag_grab_focus(struct wl_pointer_grab *grab,
		struct wl_surface *surface, int32_t x, int32_t y)
{
	struct wl_input_device *device =
		container_of(grab, struct wl_input_device, drag_grab);
	struct wl_resource *resource, *offer;
	struct wl_display *display;
	uint32_t serial;

	if (device->drag_focus_resource) {
		wl_data_device_send_leave(device->drag_focus_resource);
		wl_list_remove(&device->drag_focus_listener.link);
		device->drag_focus_resource = NULL;
		device->drag_focus = NULL;
	}

	if (surface)
		resource = find_resource(&device->drag_resource_list, 
					 surface->resource.client);
	if (surface && resource) {
		display = wl_client_get_display(resource->client);
		serial = wl_display_next_serial(display);

		offer = wl_data_source_send_offer(device->drag_data_source,
						  resource);

		wl_data_device_send_enter(resource, serial, &surface->resource,
					  x, y, offer);

		device->drag_focus = surface;
		device->drag_focus_listener.notify = destroy_drag_focus;
		wl_signal_add(&resource->destroy_signal,
			      &device->drag_focus_listener);
		device->drag_focus_resource = resource;
		grab->focus = surface;
	}
}

static void
drag_grab_motion(struct wl_pointer_grab *grab,
		 uint32_t time, int32_t x, int32_t y)
{
	struct wl_input_device *device =
		container_of(grab, struct wl_input_device, drag_grab);

	if (device->drag_focus_resource)
		wl_data_device_send_motion(device->drag_focus_resource,
					   time, x, y);
}

static void
data_device_end_drag_grab(struct wl_input_device *device)
{
	struct wl_resource *surface_resource;
	struct wl_surface_interface *implementation;

	if (device->drag_surface) {
		surface_resource = &device->drag_surface->resource;
		implementation = (struct wl_surface_interface *)
			surface_resource->object.implementation;

		implementation->attach(surface_resource->client,
				       surface_resource, NULL, 0, 0);
		wl_list_remove(&device->drag_icon_listener.link);
	}

	drag_grab_focus(&device->drag_grab, NULL, 0, 0);

	wl_input_device_end_pointer_grab(device);

	device->drag_data_source = NULL;
	device->drag_surface = NULL;
}

static void
drag_grab_button(struct wl_pointer_grab *grab,
		 uint32_t time, uint32_t button, int32_t state)
{
	struct wl_input_device *device =
		container_of(grab, struct wl_input_device, drag_grab);

	if (device->drag_focus_resource &&
	    device->grab_button == button && state == 0)
		wl_data_device_send_drop(device->drag_focus_resource);

	if (device->button_count == 0 && state == 0) {
		data_device_end_drag_grab(device);
		wl_list_remove(&device->drag_data_source_listener.link);
	}
}

static const struct wl_pointer_grab_interface drag_grab_interface = {
	drag_grab_focus,
	drag_grab_motion,
	drag_grab_button,
};

static void
destroy_data_device_source(struct wl_listener *listener, void *data)
{
	struct wl_input_device *device;

	device = container_of(listener, struct wl_input_device,
			      drag_data_source_listener);

	data_device_end_drag_grab(device);
}

static void
destroy_data_device_icon(struct wl_listener *listener, void *data)
{
	struct wl_input_device *device;

	device = container_of(listener, struct wl_input_device,
			      drag_icon_listener);

	device->drag_surface = NULL;
}

static void
data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
		       struct wl_resource *source_resource,
		       struct wl_resource *origin_resource,
		       struct wl_resource *icon_resource, uint32_t serial)
{
	struct wl_input_device *device = resource->data;

	/* FIXME: Check that client has implicit grab on the origin
	 * surface that matches the given time. */

	/* FIXME: Check that the data source type array isn't empty. */

	device->drag_grab.interface = &drag_grab_interface;

	device->drag_data_source = source_resource->data;
	device->drag_data_source_listener.notify = destroy_data_device_source;
	wl_signal_add(&source_resource->destroy_signal,
		      &device->drag_data_source_listener);

	if (icon_resource) {
		device->drag_surface = icon_resource->data;
		device->drag_icon_listener.notify = destroy_data_device_icon;
		wl_signal_add(&icon_resource->destroy_signal,
			      &device->drag_icon_listener);
		wl_signal_emit(&device->drag_icon_signal, icon_resource);
	}

	wl_input_device_start_pointer_grab(device, &device->drag_grab);
}

static void
destroy_selection_data_source(struct wl_listener *listener, void *data)
{
	struct wl_input_device *device =
		container_of(listener, struct wl_input_device,
			     selection_data_source_listener);
	struct wl_resource *data_device, *focus;

	device->selection_data_source = NULL;

	focus = device->keyboard_focus_resource;
	if (focus) {
		data_device = find_resource(&device->drag_resource_list,
					    focus->client);
		wl_data_device_send_selection(data_device, NULL);
	}
}

WL_EXPORT void
wl_input_device_set_selection(struct wl_input_device *device,
			      struct wl_data_source *source, uint32_t serial)
{
	struct wl_resource *data_device, *focus, *offer;

	if (device->selection_data_source &&
	    device->selection_serial - serial < UINT32_MAX / 2)
		return;

	if (device->selection_data_source) {
		device->selection_data_source->cancel(device->selection_data_source);
		wl_list_remove(&device->selection_data_source_listener.link);
		device->selection_data_source = NULL;
	}

	device->selection_data_source = source;
	device->selection_serial = serial;

	focus = device->keyboard_focus_resource;
	if (focus) {
		data_device = find_resource(&device->drag_resource_list,
					    focus->client);
		if (data_device) {
			offer = wl_data_source_send_offer(device->selection_data_source,
							  data_device);
			wl_data_device_send_selection(data_device, offer);
		}
	}

	wl_signal_emit(&device->selection_signal, device);

	device->selection_data_source_listener.notify =
		destroy_selection_data_source;
	wl_signal_add(&source->resource.destroy_signal,
		      &device->selection_data_source_listener);
}

static void
data_device_set_selection(struct wl_client *client,
			  struct wl_resource *resource,
			  struct wl_resource *source_resource, uint32_t serial)
{
	if (!source_resource)
		return;

	/* FIXME: Store serial and check against incoming serial here. */
	wl_input_device_set_selection(resource->data, source_resource->data,
				      serial);
}

static const struct wl_data_device_interface data_device_interface = {
	data_device_start_drag,
	data_device_set_selection,
};

static void
destroy_data_source(struct wl_resource *resource)
{
	struct wl_data_source *source =
		container_of(resource, struct wl_data_source, resource);
	char **p;

	wl_array_for_each(p, &source->mime_types)
		free(*p);

	wl_array_release(&source->mime_types);

	source->resource.object.id = 0;
}

static void
create_data_source(struct wl_client *client,
		   struct wl_resource *resource, uint32_t id)
{
	struct wl_data_source *source;

	source = malloc(sizeof *source);
	if (source == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	source->resource.destroy = destroy_data_source;
	source->resource.object.id = id;
	source->resource.object.interface = &wl_data_source_interface;
	source->resource.object.implementation =
		(void (**)(void)) &data_source_interface;
	source->resource.data = source;
	wl_signal_init(&source->resource.destroy_signal);

	source->offer_interface = &data_offer_interface;
	source->cancel = data_source_cancel;

	wl_array_init(&source->mime_types);
	wl_client_add_resource(client, &source->resource);
}

static void unbind_data_device(struct wl_resource *resource)
{
	wl_list_remove(&resource->link);
	free(resource);
}

static void
get_data_device(struct wl_client *client,
		struct wl_resource *manager_resource,
		uint32_t id, struct wl_resource *input_device)
{
	struct wl_input_device *device = input_device->data;
	struct wl_resource *resource;

	resource =
		wl_client_add_object(client, &wl_data_device_interface,
				     &data_device_interface, id, device);
				     
	wl_list_insert(&device->drag_resource_list, &resource->link);
	resource->destroy = unbind_data_device;
}

static const struct wl_data_device_manager_interface manager_interface = {
	create_data_source,
	get_data_device
};

static void
bind_manager(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	wl_client_add_object(client, &wl_data_device_manager_interface,
			     &manager_interface, id, NULL);
}

WL_EXPORT void
wl_data_device_set_keyboard_focus(struct wl_input_device *device)
{
	struct wl_resource *data_device, *focus, *offer;
	struct wl_data_source *source;

	focus = device->keyboard_focus_resource;
	if (!focus)
		return;

	data_device = find_resource(&device->drag_resource_list,
				    focus->client);
	if (!data_device)
		return;

	source = device->selection_data_source;
	if (source) {
		offer = wl_data_source_send_offer(source, data_device);
		wl_data_device_send_selection(data_device, offer);
	}
}

WL_EXPORT int
wl_data_device_manager_init(struct wl_display *display)
{
	if (wl_display_add_global(display,
				  &wl_data_device_manager_interface,
				  NULL, bind_manager) == NULL)
		return -1;

	return 0;
}
