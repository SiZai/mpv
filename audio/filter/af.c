/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "common/common.h"
#include "common/global.h"

#include "options/m_option.h"
#include "options/m_config.h"

#include "audio/audio_buffer.h"
#include "af.h"

// Static list of filters
extern const struct af_info af_info_format;
extern const struct af_info af_info_lavcac3enc;
extern const struct af_info af_info_lavrresample;
extern const struct af_info af_info_scaletempo;
extern const struct af_info af_info_lavfi;
extern const struct af_info af_info_lavfi_bridge;
extern const struct af_info af_info_rubberband;

static const struct af_info *const filter_list[] = {
    &af_info_format,
    &af_info_lavcac3enc,
    &af_info_lavrresample,
#if HAVE_RUBBERBAND
    &af_info_rubberband,
#endif
    &af_info_scaletempo,
    &af_info_lavfi,
    &af_info_lavfi_bridge,
    NULL
};

static bool get_desc(struct m_obj_desc *dst, int index)
{
    if (index >= MP_ARRAY_SIZE(filter_list) - 1)
        return false;
    const struct af_info *af = filter_list[index];
    *dst = (struct m_obj_desc) {
        .name = af->name,
        .description = af->info,
        .priv_size = af->priv_size,
        .priv_defaults = af->priv_defaults,
        .options = af->options,
        .set_defaults = af->set_defaults,
        .p = af,
    };
    return true;
}

const struct m_obj_list af_obj_list = {
    .get_desc = get_desc,
    .description = "audio filters",
    .allow_disable_entries = true,
    .allow_unknown_entries = true,
    .aliases = {
        {"force",     "format"},
        {0}
    },
};

static void af_forget_frames(struct af_instance *af)
{
    for (int n = 0; n < af->num_out_queued; n++)
        talloc_free(af->out_queued[n]);
    af->num_out_queued = 0;
}

static void af_chain_forget_frames(struct af_stream *s)
{
    for (struct af_instance *cur = s->first; cur; cur = cur->next)
        af_forget_frames(cur);
}

static void af_copy_unset_fields(struct mp_audio *dst, struct mp_audio *src)
{
    if (dst->format == AF_FORMAT_UNKNOWN)
        mp_audio_set_format(dst, src->format);
    if (dst->nch == 0)
        mp_audio_set_channels(dst, &src->channels);
    if (dst->rate == 0)
        dst->rate = src->rate;
}

static int input_control(struct af_instance* af, int cmd, void* arg)
{
    switch (cmd) {
    case AF_CONTROL_REINIT:
        assert(arg == &((struct af_stream *)af->priv)->input);
        return AF_OK;
    }
    return AF_UNKNOWN;
}

static int output_control(struct af_instance* af, int cmd, void* arg)
{
    struct af_stream *s = af->priv;
    struct mp_audio *output = &s->output;
    struct mp_audio *filter_output = &s->filter_output;

    switch (cmd) {
    case AF_CONTROL_REINIT: {
        struct mp_audio *in = arg;
        struct mp_audio orig_in = *in;

        *filter_output = *output;
        af_copy_unset_fields(filter_output, in);
        *in = *filter_output;
        return mp_audio_config_equals(in, &orig_in) ? AF_OK : AF_FALSE;
    }
    }
    return AF_UNKNOWN;
}

static int dummy_filter(struct af_instance *af, struct mp_audio *frame)
{
    af_add_output_frame(af, frame);
    return 0;
}

/* Function for creating a new filter of type name.The name may
contain the commandline parameters for the filter */
static struct af_instance *af_create(struct af_stream *s, char *name,
                                     char **args)
{
    const char *lavfi_name = NULL;
    char **lavfi_args = NULL;
    struct m_obj_desc desc;
    if (!m_obj_list_find(&desc, &af_obj_list, bstr0(name))) {
        if (!m_obj_list_find(&desc, &af_obj_list, bstr0("lavfi-bridge"))) {
            MP_ERR(s, "Couldn't find audio filter '%s'.\n", name);
            return NULL;
        }
        lavfi_name = name;
        lavfi_args = args;
        args = NULL;
        if (strncmp(lavfi_name, "lavfi-", 6) == 0)
            lavfi_name += 6;
    }
    MP_VERBOSE(s, "Adding filter %s \n", name);

    struct af_instance *af = talloc_zero(NULL, struct af_instance);
    *af = (struct af_instance) {
        .full_name = talloc_strdup(af, name),
        .info = desc.p,
        .data = talloc_zero(af, struct mp_audio),
        .log = mp_log_new(af, s->log, name),
        .opts = s->opts,
        .global = s->global,
        .out_pool = mp_audio_pool_create(af),
    };
    struct m_config *config =
        m_config_from_obj_desc_and_args(af, s->log, s->global, &desc,
                                        name, s->opts->af_defs, args);
    if (!config)
        goto error;
    if (lavfi_name) {
        // Pass the filter arguments as proper sub-options to the bridge filter.
        struct m_config_option *name_opt = m_config_get_co(config, bstr0("name"));
        assert(name_opt);
        assert(name_opt->opt->type == &m_option_type_string);
        if (m_config_set_option_raw(config, name_opt, &lavfi_name, 0) < 0)
            goto error;
        struct m_config_option *opts = m_config_get_co(config, bstr0("opts"));
        assert(opts);
        assert(opts->opt->type == &m_option_type_keyvalue_list);
        if (m_config_set_option_raw(config, opts, &lavfi_args, 0) < 0)
            goto error;
        af->full_name = talloc_asprintf(af, "%s (lavfi)", af->full_name);
    }
    af->priv = config->optstruct;

    // Initialize the new filter
    if (af->info->open(af) != AF_OK)
        goto error;

    return af;

error:
    MP_ERR(s, "Couldn't create or open audio filter '%s'\n", name);
    talloc_free(af);
    return NULL;
}

/* Create and insert a new filter of type name before the filter in the
   argument. This function can be called during runtime, the return
   value is the new filter */
static struct af_instance *af_prepend(struct af_stream *s,
                                      struct af_instance *af,
                                      char *name, char **args)
{
    if (!af)
        af = s->last;
    if (af == s->first)
        af = s->first->next;
    // Create the new filter and make sure it is OK
    struct af_instance *new = af_create(s, name, args);
    if (!new)
        return NULL;
    // Update pointers
    new->next = af;
    new->prev = af->prev;
    af->prev = new;
    new->prev->next = new;
    return new;
}

// Uninit and remove the filter "af"
static void af_remove(struct af_stream *s, struct af_instance *af)
{
    if (!af)
        return;

    if (af == s->first || af == s->last)
        return;

    // Print friendly message
    MP_VERBOSE(s, "Removing filter %s \n", af->info->name);

    // Detach pointers
    af->prev->next = af->next;
    af->next->prev = af->prev;

    if (af->uninit)
        af->uninit(af);
    af_forget_frames(af);
    talloc_free(af);
}

static void remove_auto_inserted_filters(struct af_stream *s)
{
repeat:
    for (struct af_instance *af = s->first; af; af = af->next) {
        if (af->auto_inserted) {
            af_remove(s, af);
            goto repeat;
        }
    }
}

static void af_print_filter_chain(struct af_stream *s, struct af_instance *at,
                                  int msg_level)
{
    MP_MSG(s, msg_level, "Audio filter chain:\n");

    struct af_instance *af = s->first;
    while (af) {
        char b[128] = {0};
        mp_snprintf_cat(b, sizeof(b), "  [%s] ", af->full_name);
        if (af->label)
            mp_snprintf_cat(b, sizeof(b), "\"%s\" ", af->label);
        if (af->data)
            mp_snprintf_cat(b, sizeof(b), "%s", mp_audio_config_to_str(af->data));
        if (af->auto_inserted)
            mp_snprintf_cat(b, sizeof(b), " [a]");
        if (af == at)
            mp_snprintf_cat(b, sizeof(b), " <-");
        MP_MSG(s, msg_level, "%s\n", b);

        af = af->next;
    }

    MP_MSG(s, msg_level, "  [ao] %s\n", mp_audio_config_to_str(&s->output));
}

static void reset_formats(struct af_stream *s)
{
    struct mp_audio none = {0};
    for (struct af_instance *af = s->first; af; af = af->next) {
        if (af != s->first && af != s->last)
            mp_audio_copy_config(af->data, &none);
    }
}

static int filter_reinit(struct af_instance *af)
{
    struct af_instance *prev = af->prev;
    assert(prev);

    // Check if this is the first filter
    struct mp_audio in = *prev->data;
    // Reset just in case...
    mp_audio_set_null_data(&in);

    if (!mp_audio_config_valid(&in))
        return AF_ERROR;

    af->fmt_in = in;
    int rv = af->control(af, AF_CONTROL_REINIT, &in);
    if (rv == AF_OK && !mp_audio_config_equals(&in, prev->data))
        rv = AF_FALSE; // conversion filter needed
    if (rv == AF_FALSE)
        af->fmt_in = in;

    if (rv == AF_OK) {
        if (!mp_audio_config_valid(af->data))
            return AF_ERROR;
        af->fmt_out = *af->data;
    }

    return rv;
}

static int filter_reinit_with_conversion(struct af_stream *s, struct af_instance *af)
{
    int rv = filter_reinit(af);

    // Conversion filter is needed
    if (rv == AF_FALSE) {
        // First try if we can change the output format of the previous
        // filter to the input format the current filter is expecting.
        struct mp_audio in = af->fmt_in;
        if (af->prev != s->first && !mp_audio_config_equals(af->prev->data, &in)) {
            // This should have been successful (because it succeeded
            // before), even if just reverting to the old output format.
            mp_audio_copy_config(af->prev->data, &in);
            rv = filter_reinit(af->prev);
            if (rv != AF_OK)
                return rv;
        }
        if (!mp_audio_config_equals(af->prev->data, &in)) {
            // Retry with conversion filter added.
            char *opts[] = {"deprecation-warning", "no", NULL};
            struct af_instance *new =
                af_prepend(s, af, "lavrresample", opts);
            if (!new)
                return AF_ERROR;
            new->auto_inserted = true;
            mp_audio_copy_config(new->data, &in);
            rv = filter_reinit(new);
            if (rv != AF_OK)
                af_remove(s, new);
        }
        if (rv == AF_OK)
            rv = filter_reinit(af);
    }

    return rv;
}

static int af_find_output_conversion(struct af_stream *s, struct mp_audio *cfg)
{
    assert(mp_audio_config_valid(&s->output));
    assert(s->initialized > 0);

    if (mp_chmap_equals_reordered(&s->input.channels, &s->output.channels))
        return AF_ERROR;

    // Heuristic to detect point of conversion. If it looks like something
    // more complicated is going on, better bail out.
    // We expect that the last filter converts channels.
    struct af_instance *conv = s->last->prev;
    if (!conv->auto_inserted)
        return AF_ERROR;
    if (!(mp_chmap_equals_reordered(&conv->fmt_in.channels, &s->input.channels) &&
          mp_chmap_equals_reordered(&conv->fmt_out.channels, &s->output.channels)))
        return AF_ERROR;
    // Also, should be the only one which does auto conversion.
    for (struct af_instance *af = s->first->next; af != s->last; af = af->next)
    {
        if (af != conv && af->auto_inserted &&
            !mp_chmap_equals_reordered(&af->fmt_in.channels, &af->fmt_out.channels))
            return AF_ERROR;
    }
    // And not if it's the only filter.
    if (conv->prev == s->first && conv->next == s->last)
        return AF_ERROR;

    *cfg = s->output;
    return AF_OK;
}

// Return AF_OK on success or AF_ERROR on failure.
static int af_do_reinit(struct af_stream *s, bool second_pass)
{
    struct mp_audio convert_early = {0};
    if (second_pass) {
        // If a channel conversion happens, and it is done by an auto-inserted
        // filter, then insert a filter to convert it early. Otherwise, do
        // nothing and return immediately.
        if (af_find_output_conversion(s, &convert_early) != AF_OK)
            return AF_OK;
    }

    remove_auto_inserted_filters(s);
    af_chain_forget_frames(s);
    reset_formats(s);
    s->first->fmt_in = s->first->fmt_out = s->input;

    if (mp_audio_config_valid(&convert_early)) {
        char *opts[] = {"deprecation-warning", "no", NULL};
        struct af_instance *new = af_prepend(s, s->first, "lavrresample", opts);
        if (!new)
            return AF_ERROR;
        new->auto_inserted = true;
        mp_audio_copy_config(new->data, &convert_early);
        int rv = filter_reinit(new);
        if (rv != AF_DETACH && rv != AF_OK)
            return AF_ERROR;
        MP_VERBOSE(s, "Moving up output conversion.\n");
    }

    // Start with the second filter, as the first filter is the special input
    // filter which needs no initialization.
    struct af_instance *af = s->first->next;
    while (af) {
        int rv = filter_reinit_with_conversion(s, af);

        switch (rv) {
        case AF_OK:
            af = af->next;
            break;
        case AF_FALSE: {
            // If the format conversion is (probably) caused by spdif, then
            // (as a feature) drop the filter, instead of failing hard.
            int fmt_in1 = af->prev->data->format;
            int fmt_in2 = af->fmt_in.format;
            if (af_fmt_is_valid(fmt_in1) && af_fmt_is_valid(fmt_in2)) {
                bool spd1 = af_fmt_is_spdif(fmt_in1);
                bool spd2 = af_fmt_is_spdif(fmt_in2);
                if (spd1 != spd2 && af->next) {
                    MP_WARN(af, "Filter %s apparently cannot be used due to "
                                "spdif passthrough - removing it.\n",
                                af->info->name);
                    struct af_instance *aft = af->prev;
                    af_remove(s, af);
                    af = aft->next;
                    continue;
                }
            }
            goto negotiate_error;
        }
        case AF_DETACH: { // Filter is redundant and wants to be unloaded
            struct af_instance *aft = af->prev; // never NULL
            af_remove(s, af);
            af = aft->next;
            break;
        }
        default:
            MP_ERR(s, "Reinitialization did not work, "
                   "audio filter '%s' returned error code %i\n",
                   af->info->name, rv);
            goto error;
        }
    }

    /* Set previously unset fields in s->output to those of the filter chain
     * output. This is used to make the output format fixed, and even if you
     * insert new filters or change the input format, the output format won't
     * change. (Audio outputs generally can't change format at runtime.) */
    af_copy_unset_fields(&s->output, &s->filter_output);
    if (mp_audio_config_equals(&s->output, &s->filter_output)) {
        s->initialized = 1;
        af_print_filter_chain(s, NULL, MSGL_V);
        return AF_OK;
    }

    goto error;

negotiate_error:
    MP_ERR(s, "Unable to convert audio input format to output format.\n");
error:
    s->initialized = -1;
    af_print_filter_chain(s, af, MSGL_ERR);
    return AF_ERROR;
}

static int af_reinit(struct af_stream *s)
{
    int r = af_do_reinit(s, false);
    if (r == AF_OK && mp_audio_config_valid(&s->output)) {
        r = af_do_reinit(s, true);
        if (r != AF_OK) {
            MP_ERR(s, "Failed second pass filter negotiation.\n");
            r = af_do_reinit(s, false);
        }
    }
    return r;
}

// Uninit and remove all filters
void af_uninit(struct af_stream *s)
{
    while (s->first->next && s->first->next != s->last)
        af_remove(s, s->first->next);
    af_chain_forget_frames(s);
    s->initialized = 0;
}

struct af_stream *af_new(struct mpv_global *global)
{
    struct af_stream *s = talloc_zero(NULL, struct af_stream);
    s->log = mp_log_new(s, global->log, "!af");

    static const struct af_info in = { .name = "in" };
    s->first = talloc(s, struct af_instance);
    *s->first = (struct af_instance) {
        .full_name = "in",
        .info = &in,
        .log = s->log,
        .control = input_control,
        .filter_frame = dummy_filter,
        .priv = s,
        .data = &s->input,
    };

    static const struct af_info out = { .name = "out" };
    s->last = talloc(s, struct af_instance);
    *s->last = (struct af_instance) {
        .full_name = "out",
        .info = &out,
        .log = s->log,
        .control = output_control,
        .filter_frame = dummy_filter,
        .priv = s,
        .data = &s->filter_output,
    };

    s->first->next = s->last;
    s->last->prev = s->first;
    s->opts = global->opts;
    s->global = global;
    return s;
}

void af_destroy(struct af_stream *s)
{
    af_uninit(s);
    talloc_free(s);
}

/* Initialize the stream "s". This function creates a new filter list
   if necessary according to the values set in input and output. Input
   and output should contain the format of the current movie and the
   format of the preferred output respectively. The function is
   reentrant i.e. if called with an already initialized stream the
   stream will be reinitialized.
   If one of the preferred output parameters is 0 the one that needs
   no conversion is used (i.e. the output format in the last filter).
   The return value is 0 if success and -1 if failure */
int af_init(struct af_stream *s)
{
    // Precaution in case caller is misbehaving
    mp_audio_set_null_data(&s->input);
    mp_audio_set_null_data(&s->output);

    // Check if this is the first call
    if (s->first->next == s->last) {
        // Add all filters in the list (if there are any)
        struct m_obj_settings *list = s->opts->af_settings;
        for (int i = 0; list && list[i].name; i++) {
            if (!list[i].enabled)
                continue;
            struct af_instance *af =
                af_prepend(s, s->last, list[i].name, list[i].attribs);
            if (!af) {
                af_uninit(s);
                s->initialized = -1;
                return -1;
            }
            af->label = talloc_strdup(af, list[i].label);
        }
    }

    if (af_reinit(s) != AF_OK) {
        // Something is stuffed audio out will not work
        MP_ERR(s, "Could not create audio filter chain.\n");
        return -1;
    }
    return 0;
}

/* Add filter during execution. This function adds the filter "name"
   to the stream s. The filter will be inserted somewhere nice in the
   list of filters. The return value is a pointer to the new filter,
   If the filter couldn't be added the return value is NULL. */
struct af_instance *af_add(struct af_stream *s, char *name, char *label,
                           char **args)
{
    assert(label);

    if (af_find_by_label(s, label))
        return NULL;

    struct af_instance *new = af_prepend(s, s->last, name, args);
    if (!new)
        return NULL;
    new->label = talloc_strdup(new, label);

    // Reinitialize the filter list
    if (af_reinit(s) != AF_OK) {
        af_remove_by_label(s, label);
        return NULL;
    }
    return af_find_by_label(s, label);
}

struct af_instance *af_find_by_label(struct af_stream *s, char *label)
{
    for (struct af_instance *af = s->first; af; af = af->next) {
        if (af->label && strcmp(af->label, label) == 0)
            return af;
    }
    return NULL;
}

/* Remove the first filter that matches this name. Return number of filters
 * removed (0, 1), or a negative error code if reinit after removing failed.
 */
int af_remove_by_label(struct af_stream *s, char *label)
{
    struct af_instance *af = af_find_by_label(s, label);
    if (!af)
        return 0;
    af_remove(s, af);
    if (af_reinit(s) != AF_OK) {
        af_uninit(s);
        af_init(s);
        return -1;
    }
    return 1;
}

/* Calculate the total delay [seconds of output] caused by the filters */
double af_calc_delay(struct af_stream *s)
{
    struct af_instance *af = s->first;
    double delay = 0.0;
    while (af) {
        delay += af->delay;
        for (int n = 0; n < af->num_out_queued; n++)
            delay += af->out_queued[n]->samples / (double)af->data->rate;
        af = af->next;
    }
    return delay;
}

/* Send control to all filters, starting with the last until one accepts the
 * command with AF_OK. Return the accepting filter. */
struct af_instance *af_control_any_rev(struct af_stream *s, int cmd, void *arg)
{
    int res = AF_UNKNOWN;
    struct af_instance *filt = s->last;
    while (filt) {
        res = filt->control(filt, cmd, arg);
        if (res == AF_OK)
            return filt;
        filt = filt->prev;
    }
    return NULL;
}

/* Send control to all filters. Never stop, even if a filter returns AF_OK. */
void af_control_all(struct af_stream *s, int cmd, void *arg)
{
    for (struct af_instance *af = s->first; af; af = af->next)
        af->control(af, cmd, arg);
}

int af_control_by_label(struct af_stream *s, int cmd, void *arg, bstr label)
{
    char *label_str = bstrdup0(NULL, label);
    struct af_instance *cur = af_find_by_label(s, label_str);
    talloc_free(label_str);
    if (cur) {
        return cur->control ? cur->control(cur, cmd, arg) : CONTROL_NA;
    } else {
        return CONTROL_UNKNOWN;
    }
}

int af_send_command(struct af_stream *s, char *label, char *cmd, char *arg)
{
    char *args[2] = {cmd, arg};
    if (strcmp(label, "all") == 0) {
        af_control_all(s, AF_CONTROL_COMMAND, args);
        return 0;
    } else {
        return af_control_by_label(s, AF_CONTROL_COMMAND, args, bstr0(label));
    }
}

// Used by filters to add a filtered frame to the output queue.
// Ownership of frame is transferred from caller to the filter chain.
void af_add_output_frame(struct af_instance *af, struct mp_audio *frame)
{
    if (frame) {
        assert(mp_audio_config_equals(&af->fmt_out, frame));
        MP_TARRAY_APPEND(af, af->out_queued, af->num_out_queued, frame);
    }
}

static bool af_has_output_frame(struct af_instance *af)
{
    if (!af->num_out_queued && af->filter_out) {
        if (af->filter_out(af) < 0)
            MP_ERR(af, "Error filtering frame.\n");
    }
    return af->num_out_queued > 0;
}

static struct mp_audio *af_dequeue_output_frame(struct af_instance *af)
{
    struct mp_audio *res = NULL;
    if (af_has_output_frame(af)) {
        res = af->out_queued[0];
        MP_TARRAY_REMOVE_AT(af->out_queued, af->num_out_queued, 0);
    }
    return res;
}

static void read_remaining(struct af_instance *af)
{
    int num_frames;
    do {
        num_frames = af->num_out_queued;
        if (!af->filter_out || af->filter_out(af) < 0)
            break;
    } while (num_frames != af->num_out_queued);
}

static int af_do_filter(struct af_instance *af, struct mp_audio *frame)
{
    if (frame)
        assert(mp_audio_config_equals(&af->fmt_in, frame));
    int r = af->filter_frame(af, frame);
    if (r < 0)
        MP_ERR(af, "Error filtering frame.\n");
    return r;
}

// Input a frame into the filter chain. Ownership of frame is transferred.
// Return >= 0 on success, < 0 on failure (even if output frames were produced)
int af_filter_frame(struct af_stream *s, struct mp_audio *frame)
{
    assert(frame);
    if (s->initialized < 1) {
        talloc_free(frame);
        return -1;
    }
    return af_do_filter(s->first, frame);
}

// Output the next queued frame (if any) from the full filter chain.
// The frame can be retrieved with af_read_output_frame().
//  eof: if set, assume there's no more input i.e. af_filter_frame() will
//       not be called (until reset) - flush all internally delayed frames
//  returns: -1: error, 0: no output, 1: output available
int af_output_frame(struct af_stream *s, bool eof)
{
    if (s->last->num_out_queued)
        return 1;
    if (s->initialized < 1)
        return -1;
    while (1) {
        struct af_instance *last = NULL;
        for (struct af_instance * cur = s->first; cur; cur = cur->next) {
            // Flush remaining frames on EOF, but do that only if the previous
            // filters have been flushed (i.e. they have no more output).
            if (eof && !last) {
                read_remaining(cur);
                int r = af_do_filter(cur, NULL);
                if (r < 0)
                    return r;
            }
            if (af_has_output_frame(cur))
                last = cur;
        }
        if (!last)
            return 0;
        if (!last->next)
            return 1;
        int r = af_do_filter(last->next, af_dequeue_output_frame(last));
        if (r < 0)
            return r;
    }
}

struct mp_audio *af_read_output_frame(struct af_stream *s)
{
    if (!s->last->num_out_queued)
        af_output_frame(s, false);
    return af_dequeue_output_frame(s->last);
}

void af_unread_output_frame(struct af_stream *s, struct mp_audio *frame)
{
    struct af_instance *af = s->last;
    MP_TARRAY_INSERT_AT(af, af->out_queued, af->num_out_queued, 0, frame);
}

// Make sure the caller can change data referenced by the frame.
// Return negative error code on failure (i.e. you can't write).
int af_make_writeable(struct af_instance *af, struct mp_audio *frame)
{
    return mp_audio_pool_make_writeable(af->out_pool, frame);
}

void af_seek_reset(struct af_stream *s)
{
    af_control_all(s, AF_CONTROL_RESET, NULL);
    af_chain_forget_frames(s);
}
