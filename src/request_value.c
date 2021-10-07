/*
 * Copyright (C) 2021 Robin Gareus <robin@gareus.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define REQVAL_URI "http://gareus.org/oss/lv2/request_value"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/patch/patch.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/extensions/ui/ui.h>
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>

/* custom extension */
#define LV2_DIALOGMESSAGE_URI "http://ardour.org/lv2/dialog_message"

typedef struct {
  void (*free_msg)(char const* msg);

  char const* msg;
  bool requires_return;

} LV2_Dialog_Message;

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Object;
	LV2_URID atom_URID;
	LV2_URID atom_Float;
	LV2_URID atom_Bool;
	LV2_URID patch_Set;
	LV2_URID patch_property;
	LV2_URID patch_value;
	LV2_URID m_bool_test;
	LV2_URID m_ack_test;
} ReqValURIs;

static void non_free (char const* msg)
{
#if 0 // statically allocated message
	free (msg);
#endif
}

typedef struct {
	/* ports */
	const LV2_Atom_Sequence* control;

	float const* p_in;
	float*       p_out;

	/* LV2 Output */
	LV2_Log_Log*   log;
	LV2_Log_Logger logger;

	/* request param */
	LV2UI_Request_Value* request_value;
	LV2_Feature**        features;
	LV2_Feature          dialog_feature;
	LV2_Dialog_Message   dialog_message;

	/* settings, config */
	ReqValURIs uris;
	double     sample_rate;

	/* state */
	uint64_t sample_cnt;
	bool     request_sent;

} ReqVal;

static void
map_uris (LV2_URID_Map* map, ReqValURIs* uris)
{
	uris->atom_Blank     = map->map (map->handle, LV2_ATOM__Blank);
	uris->atom_Object    = map->map (map->handle, LV2_ATOM__Object);
	uris->atom_URID      = map->map (map->handle, LV2_ATOM__URID);
	uris->atom_Float     = map->map (map->handle, LV2_ATOM__Float);
	uris->atom_Bool      = map->map (map->handle, LV2_ATOM__Bool);
	uris->patch_Set      = map->map (map->handle, LV2_PATCH__Set);
	uris->patch_property = map->map (map->handle, LV2_PATCH__property);
	uris->patch_value    = map->map (map->handle, LV2_PATCH__value);
	uris->m_bool_test    = map->map (map->handle, REQVAL_URI "#booltest");
	uris->m_ack_test     = map->map (map->handle, REQVAL_URI "#acktest");
}

static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	ReqVal*       self = (ReqVal*)calloc (1, sizeof (ReqVal));
	LV2_URID_Map* map  = NULL;

	int i;
	for (i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_URID__map)) {
			map = (LV2_URID_Map*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_LOG__log)) {
			self->log = (LV2_Log_Log*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_UI__requestValue)) {
			self->request_value = (LV2UI_Request_Value*)features[i]->data;
		}
	}

	/* Initialise logger (if map is unavailable, will fallback to printf) */
	lv2_log_logger_init (&self->logger, map, self->log);

	if (!self->request_value) {
		lv2_log_error (&self->logger, "ReqVal.lv2: Host does not support ui:request_value\n");
		free (self);
		return NULL;
	}

	if (!map) {
		lv2_log_error (&self->logger, "ReqVal.lv2: Host does not support urid:map\n");
		free (self);
		return NULL;
	}

	map_uris (map, &self->uris);

	self->sample_rate  = rate;
	self->sample_cnt   = 0;
	self->request_sent = false;

	self->dialog_message.msg = NULL;
	self->dialog_message.requires_return = true;
	self->dialog_message.free_msg = non_free;

	self->dialog_feature.URI  = LV2_DIALOGMESSAGE_URI;
	self->dialog_feature.data = &self->dialog_message;

	self->features = (LV2_Feature**)calloc(2, sizeof(LV2_Feature*));
	self->features[0] = &self->dialog_feature;

	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance,
              uint32_t   port,
              void*      data)
{
	ReqVal* self = (ReqVal*)instance;

	switch (port) {
		case 0:
			self->control = (const LV2_Atom_Sequence*)data;
			break;
		case 1:
			self->p_in = (const float*)data;
			break;
		case 2:
			self->p_out = (float*)data;
			break;
		default:
			break;
	}
}

static inline bool
parse_property (ReqVal* self, const LV2_Atom_Object* obj)
{
	const LV2_Atom* property = NULL;
	lv2_atom_object_get (obj, self->uris.patch_property, &property, 0);

	/* Get property URI.
	 *
	 * Note: Real world code would only call
	 *  if (!property || property->type != self->uris.atom_URID) { return; }
	 * However this is example and test code, so..
	 */
	if (!property) {
		lv2_log_error (&self->logger, "ReqVal.lv2: Malformed set message has no body.\n");
		return false;
	} else if (property->type != self->uris.atom_URID) {
		lv2_log_error (&self->logger, "ReqVal.lv2: Malformed set message has non-URID property.\n");
		return false;
	}

	/* Get value */
	const LV2_Atom* val = NULL;
	lv2_atom_object_get (obj, self->uris.patch_value, &val, 0);
	if (!val) {
		lv2_log_error (&self->logger, "ReqVal.lv2: Malformed set message has no value.\n");
		return false;
	}

	/* NOTE: This code errs towards the verbose side
	 *  - the type is usually implicit and does not need to be checked.
	 *  - consolidate code e.g.
	 *
	 *    const LV2_URID urid = (LV2_Atom_URID*)property)->body
	 *    ReqValURIs* urid    = self->uris;
	 *
	 *  - no need to lv2_log warnings or errors
	 */

	if (((LV2_Atom_URID*)property)->body == self->uris.m_bool_test) {
		if (val->type != self->uris.atom_Bool) {
			lv2_log_error (&self->logger, "ReqVal.lv2: Invalid property type, expected 'bool'.\n");
			return false;
		}
		bool b = *((bool*)(val + 1));
		lv2_log_note (&self->logger, "ReqVal.lv2: Received boolean = %d\n", b);
	} else {
		lv2_log_error (&self->logger, "ReqVal.lv2: Set message for unknown property.\n");
		return false;
	}
	return true;
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	ReqVal* self = (ReqVal*)instance;

	/* just forward all audio */
	if (self->p_out != self->p_in) {
		memcpy (self->p_out, self->p_in, sizeof (float) * n_samples);
	}

	if (!self->control) {
		return;
	}

	/* process control events */
	LV2_ATOM_SEQUENCE_FOREACH (self->control, ev)
	{
		if (ev->body.type != self->uris.atom_Object) {
			continue;
		}
		const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
		if (obj->body.otype == self->uris.patch_Set) {
			parse_property (self, obj);
		}
	}

	if (!self->request_sent && self->sample_cnt > 2 * self->sample_rate) {
		self->request_sent = true;
		self->dialog_message.msg = "FOO BAR!";
		self->dialog_message.requires_return = false;
		self->request_value->request (self->request_value->handle, self->uris.m_bool_test, self->uris.atom_Bool, (const LV2_Feature * const*)self->features);
	}

	self->sample_cnt += n_samples;
}

static void
cleanup (LV2_Handle instance)
{
	free (instance);
}

static const void*
extension_data (const char* uri)
{
	return NULL;
}

static const LV2_Descriptor descriptor = {
	REQVAL_URI,
	instantiate,
	connect_port,
	NULL,
	run,
	NULL,
	cleanup,
	extension_data
};

/* clang-format off */
#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
# define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
# define LV2_SYMBOL_EXPORT __attribute__ ((visibility ("default")))
#endif
/* clang-format on */
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	switch (index) {
		case 0:
			return &descriptor;
		default:
			return NULL;
	}
}
