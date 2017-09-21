/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Parts under HAVE_GPL are licensed under GNU General Public License.
 */

#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <assert.h>

#include "config.h"
#include "mpv_talloc.h"

#include "common/msg.h"
#include "common/encode.h"
#include "options/options.h"
#include "common/common.h"
#include "osdep/timer.h"

#include "audio/audio_buffer.h"
#include "audio/aconverter.h"
#include "audio/format.h"
#include "audio/decode/dec_audio.h"
#include "audio/out/ao.h"
#include "demux/demux.h"
#include "video/decode/dec_video.h"

#include "core.h"
#include "command.h"

enum {
    AD_OK = 0,
    AD_ERR = -1,
    AD_EOF = -2,
    AD_NEW_FMT = -3,
    AD_WAIT = -4,
    AD_NO_PROGRESS = -5,
    AD_STARVE = -6,
};

#if HAVE_LIBAF

#include "audio/audio.h"
#include "audio/filter/af.h"

// Use pitch correction only for speed adjustments by the user, not minor sync
// correction ones.
static int get_speed_method(struct MPContext *mpctx)
{
    return mpctx->opts->pitch_correction && mpctx->opts->playback_speed != 1.0
        ? AF_CONTROL_SET_PLAYBACK_SPEED : AF_CONTROL_SET_PLAYBACK_SPEED_RESAMPLE;
}

// Try to reuse the existing filters to change playback speed. If it works,
// return true; if filter recreation is needed, return false.
static bool update_speed_filters(struct MPContext *mpctx)
{
    struct af_stream *afs = mpctx->ao_chain->af;
    double speed = mpctx->audio_speed;

    if (afs->initialized < 1)
        return false;

    // Make sure only exactly one filter changes speed; resetting them all
    // and setting 1 filter is the easiest way to achieve this.
    af_control_all(afs, AF_CONTROL_SET_PLAYBACK_SPEED, &(double){1});
    af_control_all(afs, AF_CONTROL_SET_PLAYBACK_SPEED_RESAMPLE, &(double){1});

    if (speed == 1.0)
        return !af_find_by_label(afs, "playback-speed");

    // Compatibility: if the user uses --af=scaletempo, always use this
    // filter to change speed. Don't insert a second filter (any) either.
    if (!af_find_by_label(afs, "playback-speed") &&
        af_control_any_rev(afs, AF_CONTROL_SET_PLAYBACK_SPEED, &speed))
        return true;

    return !!af_control_any_rev(afs, get_speed_method(mpctx), &speed);
}

// Update speed, and insert/remove filters if necessary.
static void recreate_speed_filters(struct MPContext *mpctx)
{
    struct af_stream *afs = mpctx->ao_chain->af;

    if (update_speed_filters(mpctx))
        return;

    if (af_remove_by_label(afs, "playback-speed") < 0)
        goto fail;

    if (mpctx->audio_speed == 1.0)
        return;

    int method = get_speed_method(mpctx);
    char *filter = method == AF_CONTROL_SET_PLAYBACK_SPEED
                 ? "scaletempo" : "lavrresample";

    if (!af_add(afs, filter, "playback-speed", NULL))
        goto fail;

    if (!update_speed_filters(mpctx))
        goto fail;

    return;

fail:
    mpctx->opts->playback_speed = 1.0;
    mpctx->speed_factor_a = 1.0;
    mpctx->audio_speed = 1.0;
    mp_notify(mpctx, MP_EVENT_CHANGE_ALL, NULL);
}

static double db_gain(double db)
{
    return pow(10.0, db/20.0);
}

static float compute_replaygain(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct ao_chain *ao_c = mpctx->ao_chain;

    float rgain = 1.0;

    struct replaygain_data *rg = ao_c->af->replaygain_data;
    if (opts->rgain_mode && rg) {
        MP_VERBOSE(mpctx, "Replaygain: Track=%f/%f Album=%f/%f\n",
                   rg->track_gain, rg->track_peak,
                   rg->album_gain, rg->album_peak);

        float gain, peak;
        if (opts->rgain_mode == 1) {
            gain = rg->track_gain;
            peak = rg->track_peak;
        } else {
            gain = rg->album_gain;
            peak = rg->album_peak;
        }

        gain += opts->rgain_preamp;
        rgain = db_gain(gain);

        MP_VERBOSE(mpctx, "Applying replay-gain: %f\n", rgain);

        if (!opts->rgain_clip) { // clipping prevention
            rgain = MPMIN(rgain, 1.0 / peak);
            MP_VERBOSE(mpctx, "...with clipping prevention: %f\n", rgain);
        }
    } else if (opts->rgain_fallback) {
        rgain = db_gain(opts->rgain_fallback);
        MP_VERBOSE(mpctx, "Applying fallback gain: %f\n", rgain);
    }

    return rgain;
}

// Called when opts->softvol_volume or opts->softvol_mute were changed.
void audio_update_volume(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c || ao_c->af->initialized < 1)
        return;

    float gain = MPMAX(opts->softvol_volume / 100.0, 0);
    gain = pow(gain, 3);
    gain *= compute_replaygain(mpctx);
    if (opts->softvol_mute == 1)
        gain = 0.0;

    if (!af_control_any_rev(ao_c->af, AF_CONTROL_SET_VOLUME, &gain)) {
        if (gain == 1.0)
            return;
        MP_VERBOSE(mpctx, "Inserting volume filter.\n");
        char *args[] = {"warn", "no", NULL};
        if (!(af_add(ao_c->af, "volume", "softvol", args)
              && af_control_any_rev(ao_c->af, AF_CONTROL_SET_VOLUME, &gain)))
            MP_ERR(mpctx, "No volume control available.\n");
    }
}

/* NOTE: Currently the balance code is seriously buggy: it always changes
 * the af_pan mapping between the first two input channels and first two
 * output channels to particular values. These values make sense for an
 * af_pan instance that was automatically inserted for balance control
 * only and is otherwise an identity transform, but if the filter was
 * there for another reason, then ignoring and overriding the original
 * values is completely wrong.
 */
void audio_update_balance(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c || ao_c->af->initialized < 1)
        return;

    float val = opts->balance;

    if (af_control_any_rev(ao_c->af, AF_CONTROL_SET_PAN_BALANCE, &val))
        return;

    if (val == 0)
        return;

    struct af_instance *af_pan_balance;
    if (!(af_pan_balance = af_add(ao_c->af, "pan", "autopan", NULL))) {
        MP_ERR(mpctx, "No balance control available.\n");
        return;
    }

    /* make all other channels pass through since by default pan blocks all */
    for (int i = 2; i < AF_NCH; i++) {
        float level[AF_NCH] = {0};
        level[i] = 1.f;
        af_control_ext_t arg_ext = { .ch = i, .arg = level };
        af_pan_balance->control(af_pan_balance, AF_CONTROL_SET_PAN_LEVEL,
                                &arg_ext);
    }

    af_pan_balance->control(af_pan_balance, AF_CONTROL_SET_PAN_BALANCE, &val);
}

static int recreate_audio_filters(struct MPContext *mpctx)
{
    assert(mpctx->ao_chain);

    struct af_stream *afs = mpctx->ao_chain->af;
    if (afs->initialized < 1 && af_init(afs) < 0)
        goto fail;

    recreate_speed_filters(mpctx);
    if (afs->initialized < 1 && af_init(afs) < 0)
        goto fail;

    if (mpctx->opts->softvol == SOFTVOL_NO)
        MP_ERR(mpctx, "--softvol=no is not supported anymore.\n");

    audio_update_volume(mpctx);
    audio_update_balance(mpctx);

    mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

    return 0;

fail:
    MP_ERR(mpctx, "Couldn't find matching filter/ao format!\n");
    return -1;
}

int reinit_audio_filters(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c)
        return 0;

    double delay = 0;
    if (ao_c->af->initialized > 0)
        delay = af_calc_delay(ao_c->af);

    af_uninit(ao_c->af);
    if (recreate_audio_filters(mpctx) < 0)
        return -1;

    // Only force refresh if the amount of dropped buffered data is going to
    // cause "issues" for the A/V sync logic.
    if (mpctx->audio_status == STATUS_PLAYING && delay > 0.2)
        issue_refresh_seek(mpctx, MPSEEK_EXACT);
    return 1;
}

#else /* HAVE_LIBAV */

void audio_update_volume(struct MPContext *mpctx) {}
void audio_update_balance(struct MPContext *mpctx) {}
int reinit_audio_filters(struct MPContext *mpctx) { return 0; }

#endif /* else HAVE_LIBAF */

// Call this if opts->playback_speed or mpctx->speed_factor_* change.
void update_playback_speed(struct MPContext *mpctx)
{
    mpctx->audio_speed = mpctx->opts->playback_speed * mpctx->speed_factor_a;
    mpctx->video_speed = mpctx->opts->playback_speed * mpctx->speed_factor_v;

#if HAVE_LIBAF
    if (!mpctx->ao_chain || mpctx->ao_chain->af->initialized < 1)
        return;

    if (!update_speed_filters(mpctx))
        recreate_audio_filters(mpctx);
#endif
}

static void ao_chain_reset_state(struct ao_chain *ao_c)
{
    ao_c->pts = MP_NOPTS_VALUE;
    ao_c->pts_reset = false;
    TA_FREEP(&ao_c->input_frame);
    TA_FREEP(&ao_c->output_frame);
#if HAVE_LIBAF
    af_seek_reset(ao_c->af);
#endif
    if (ao_c->conv)
        mp_aconverter_flush(ao_c->conv);
    mp_audio_buffer_clear(ao_c->ao_buffer);

    if (ao_c->audio_src)
        audio_reset_decoding(ao_c->audio_src);
}

void reset_audio_state(struct MPContext *mpctx)
{
    if (mpctx->ao_chain)
        ao_chain_reset_state(mpctx->ao_chain);
    mpctx->audio_status = mpctx->ao_chain ? STATUS_SYNCING : STATUS_EOF;
    mpctx->delay = 0;
    mpctx->audio_drop_throttle = 0;
    mpctx->audio_stat_start = 0;
    mpctx->audio_allow_second_chance_seek = false;
}

void uninit_audio_out(struct MPContext *mpctx)
{
    if (mpctx->ao) {
        // Note: with gapless_audio, stop_play is not correctly set
        if (mpctx->opts->gapless_audio || mpctx->stop_play == AT_END_OF_FILE)
            ao_drain(mpctx->ao);
        ao_uninit(mpctx->ao);

        mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);
    }
    mpctx->ao = NULL;
    talloc_free(mpctx->ao_decoder_fmt);
    mpctx->ao_decoder_fmt = NULL;
}

static void ao_chain_uninit(struct ao_chain *ao_c)
{
    struct track *track = ao_c->track;
    if (track) {
        assert(track->ao_c == ao_c);
        track->ao_c = NULL;
        assert(track->d_audio == ao_c->audio_src);
        track->d_audio = NULL;
        audio_uninit(ao_c->audio_src);
    }

    if (ao_c->filter_src)
        lavfi_set_connected(ao_c->filter_src, false);

#if HAVE_LIBAF
    af_destroy(ao_c->af);
#endif
    talloc_free(ao_c->conv);
    talloc_free(ao_c->input_frame);
    talloc_free(ao_c->input_format);
    talloc_free(ao_c->filter_input_format);
    talloc_free(ao_c->ao_buffer);
    talloc_free(ao_c);
}

void uninit_audio_chain(struct MPContext *mpctx)
{
    if (mpctx->ao_chain) {
        ao_chain_uninit(mpctx->ao_chain);
        mpctx->ao_chain = NULL;

        mpctx->audio_status = STATUS_EOF;

        mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);
    }
}

static char *audio_config_to_str_buf(char *buf, size_t buf_sz, int rate,
                                     int format, struct mp_chmap channels)
{
    char ch[128];
    mp_chmap_to_str_buf(ch, sizeof(ch), &channels);
    char *hr_ch = mp_chmap_to_str_hr(&channels);
    if (strcmp(hr_ch, ch) != 0)
        mp_snprintf_cat(ch, sizeof(ch), " (%s)", hr_ch);
    snprintf(buf, buf_sz, "%dHz %s %dch %s", rate,
             ch, channels.num, af_fmt_to_str(format));
    return buf;
}

static void reinit_audio_filters_and_output(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct ao_chain *ao_c = mpctx->ao_chain;
    assert(ao_c);
    struct track *track = ao_c->track;

    if (!mp_aframe_config_is_valid(ao_c->input_format)) {
        // We don't know the audio format yet - so configure it later as we're
        // resyncing. fill_audio_buffers() will call this function again.
        mp_wakeup_core(mpctx);
        return;
    }

    // Weak gapless audio: drain AO on decoder format changes
    if (mpctx->ao_decoder_fmt && mpctx->ao && opts->gapless_audio < 0 &&
        !mp_aframe_config_equals(mpctx->ao_decoder_fmt, ao_c->input_format))
    {
        uninit_audio_out(mpctx);
    }

    TA_FREEP(&ao_c->output_frame);

    int out_rate = 0;
    int out_format = 0;
    struct mp_chmap out_channels = {0};
    if (mpctx->ao) {
        ao_get_format(mpctx->ao, &out_rate, &out_format, &out_channels);
    } else if (af_fmt_is_pcm(mp_aframe_get_format(ao_c->input_format))) {
        out_rate = opts->force_srate;
        out_format = opts->audio_output_format;
        if (opts->audio_output_channels.num_chmaps == 1)
            out_channels = opts->audio_output_channels.chmaps[0];
    }

#if HAVE_LIBAF
    struct af_stream *afs = ao_c->af;

    struct mp_audio in_format;
    mp_audio_config_from_aframe(&in_format, ao_c->input_format);
    if (mpctx->ao && mp_audio_config_equals(&in_format, &afs->input))
        return;

    afs->output = (struct mp_audio){0};
    afs->output.rate = out_rate;
    mp_audio_set_format(&afs->output, out_format);
    mp_audio_set_channels(&afs->output, &out_channels);

    // filter input format: same as codec's output format:
    afs->input = in_format;

    // Determine what the filter chain outputs. recreate_audio_filters() also
    // needs this for testing whether playback speed is changed by resampling
    // or using a special filter.
    if (af_init(afs) < 0) {
        MP_ERR(mpctx, "Error at audio filter chain pre-init!\n");
        goto init_error;
    }

    out_rate = afs->output.rate;
    out_format = afs->output.format;
    out_channels = afs->output.channels;
#else
    if (mpctx->ao && ao_c->filter_input_format &&
        mp_aframe_config_equals(ao_c->filter_input_format, ao_c->input_format))
        return;

    TA_FREEP(&ao_c->filter_input_format);

    if (!out_rate)
        out_rate = mp_aframe_get_rate(ao_c->input_format);
    if (!out_format)
        out_format = mp_aframe_get_format(ao_c->input_format);
    if (!out_channels.num)
        mp_aframe_get_chmap(ao_c->input_format, &out_channels);
#endif

    if (!mpctx->ao) {
        int ao_flags = 0;
        bool spdif_fallback = af_fmt_is_spdif(out_format) &&
                              ao_c->spdif_passthrough;

        if (opts->ao_null_fallback && !spdif_fallback)
            ao_flags |= AO_INIT_NULL_FALLBACK;

        if (opts->audio_stream_silence)
            ao_flags |= AO_INIT_STREAM_SILENCE;

        if (opts->audio_exclusive)
            ao_flags |= AO_INIT_EXCLUSIVE;

        if (af_fmt_is_pcm(out_format)) {
            if (!opts->audio_output_channels.set ||
                opts->audio_output_channels.auto_safe)
                ao_flags |= AO_INIT_SAFE_MULTICHANNEL_ONLY;

            mp_chmap_sel_list(&out_channels,
                              opts->audio_output_channels.chmaps,
                              opts->audio_output_channels.num_chmaps);
        }

        mpctx->ao = ao_init_best(mpctx->global, ao_flags, mp_wakeup_core_cb,
                                 mpctx, mpctx->encode_lavc_ctx, out_rate,
                                 out_format, out_channels);
        ao_c->ao = mpctx->ao;

        int ao_rate = 0;
        int ao_format = 0;
        struct mp_chmap ao_channels = {0};
        if (mpctx->ao)
            ao_get_format(mpctx->ao, &ao_rate, &ao_format, &ao_channels);

        // Verify passthrough format was not changed.
        if (mpctx->ao && af_fmt_is_spdif(out_format)) {
            if (out_rate != ao_rate || out_format != ao_format ||
                !mp_chmap_equals(&out_channels, &ao_channels))
            {
                MP_ERR(mpctx, "Passthrough format unsupported.\n");
                ao_uninit(mpctx->ao);
                mpctx->ao = NULL;
                ao_c->ao = NULL;
            }
        }

        if (!mpctx->ao) {
            // If spdif was used, try to fallback to PCM.
            if (spdif_fallback && ao_c->audio_src) {
                MP_VERBOSE(mpctx, "Falling back to PCM output.\n");
                ao_c->spdif_passthrough = false;
                ao_c->spdif_failed = true;
                ao_c->audio_src->try_spdif = false;
                if (!audio_init_best_codec(ao_c->audio_src))
                    goto init_error;
                reset_audio_state(mpctx);
                mp_aframe_reset(ao_c->input_format);
                mp_wakeup_core(mpctx); // reinit with new format next time
                return;
            }

            MP_ERR(mpctx, "Could not open/initialize audio device -> no sound.\n");
            mpctx->error_playing = MPV_ERROR_AO_INIT_FAILED;
            goto init_error;
        }

        mp_audio_buffer_reinit_fmt(ao_c->ao_buffer, ao_format, &ao_channels,
                                   ao_rate);

#if HAVE_LIBAF
        afs->output = (struct mp_audio){0};
        afs->output.rate = ao_rate;
        mp_audio_set_format(&afs->output, ao_format);
        mp_audio_set_channels(&afs->output, &ao_channels);
        if (!mp_audio_config_equals(&afs->output, &afs->filter_output))
            afs->initialized = 0;
#else
        int in_rate = mp_aframe_get_rate(ao_c->input_format);
        int in_format = mp_aframe_get_format(ao_c->input_format);
        struct mp_chmap in_chmap = {0};
        mp_aframe_get_chmap(ao_c->input_format, &in_chmap);
        if (!mp_aconverter_reconfig(ao_c->conv, in_rate, in_format, in_chmap,
                                    ao_rate, ao_format, ao_channels))
        {
            MP_ERR(mpctx, "Cannot convert audio data for output.\n");
            goto init_error;
        }
        ao_c->filter_input_format = mp_aframe_new_ref(ao_c->input_format);
#endif

        mpctx->ao_decoder_fmt = mp_aframe_new_ref(ao_c->input_format);

        char tmp[80];
        MP_INFO(mpctx, "AO: [%s] %s\n", ao_get_name(mpctx->ao),
                audio_config_to_str_buf(tmp, sizeof(tmp), ao_rate, ao_format,
                                        ao_channels));
        MP_VERBOSE(mpctx, "AO: Description: %s\n", ao_get_description(mpctx->ao));
        update_window_title(mpctx, true);

        ao_c->ao_resume_time =
            opts->audio_wait_open > 0 ? mp_time_sec() + opts->audio_wait_open : 0;
    }

#if HAVE_LIBAF
    if (recreate_audio_filters(mpctx) < 0)
        goto init_error;
#endif

    update_playback_speed(mpctx);

    mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

    return;

init_error:
    uninit_audio_chain(mpctx);
    uninit_audio_out(mpctx);
    error_on_track(mpctx, track);
}

int init_audio_decoder(struct MPContext *mpctx, struct track *track)
{
    assert(!track->d_audio);
    if (!track->stream)
        goto init_error;

    track->d_audio = talloc_zero(NULL, struct dec_audio);
    struct dec_audio *d_audio = track->d_audio;
    d_audio->log = mp_log_new(d_audio, mpctx->log, "!ad");
    d_audio->global = mpctx->global;
    d_audio->opts = mpctx->opts;
    d_audio->header = track->stream;
    d_audio->codec = track->stream->codec;

    d_audio->try_spdif = true;

    if (!audio_init_best_codec(d_audio))
        goto init_error;

    return 1;

init_error:
    if (track->sink)
        lavfi_set_connected(track->sink, false);
    track->sink = NULL;
    audio_uninit(track->d_audio);
    track->d_audio = NULL;
    error_on_track(mpctx, track);
    return 0;
}

void reinit_audio_chain(struct MPContext *mpctx)
{
    struct track *track = NULL;
    track = mpctx->current_track[0][STREAM_AUDIO];
    if (!track || !track->stream) {
        uninit_audio_out(mpctx);
        error_on_track(mpctx, track);
        return;
    }
    reinit_audio_chain_src(mpctx, track);
}

// (track=NULL creates a blank chain, used for lavfi-complex)
void reinit_audio_chain_src(struct MPContext *mpctx, struct track *track)
{
    assert(!mpctx->ao_chain);

    mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

    struct ao_chain *ao_c = talloc_zero(NULL, struct ao_chain);
    mpctx->ao_chain = ao_c;
    ao_c->log = mpctx->log;
#if HAVE_LIBAF
    ao_c->af = af_new(mpctx->global);
    if (track && track->stream)
        ao_c->af->replaygain_data = track->stream->codec->replaygain_data;
#else
    ao_c->conv = mp_aconverter_create(mpctx->global, mpctx->log, NULL);
#endif
    ao_c->spdif_passthrough = true;
    ao_c->pts = MP_NOPTS_VALUE;
    ao_c->ao_buffer = mp_audio_buffer_create(NULL);
    ao_c->ao = mpctx->ao;
    ao_c->input_format = mp_aframe_create();

    if (track) {
        ao_c->track = track;
        track->ao_c = ao_c;
        if (!init_audio_decoder(mpctx, track))
            goto init_error;
        ao_c->audio_src = track->d_audio;
    }

    reset_audio_state(mpctx);

    if (mpctx->ao) {
        int rate;
        int format;
        struct mp_chmap channels;
        ao_get_format(mpctx->ao, &rate, &format, &channels);
        mp_audio_buffer_reinit_fmt(ao_c->ao_buffer, format, &channels, rate);
    }

    mp_wakeup_core(mpctx);
    return;

init_error:
    uninit_audio_chain(mpctx);
    uninit_audio_out(mpctx);
    error_on_track(mpctx, track);
}

// Return pts value corresponding to the end point of audio written to the
// ao so far.
double written_audio_pts(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c)
        return MP_NOPTS_VALUE;

    // first calculate the end pts of audio that has been output by decoder
    double a_pts = ao_c->pts;
    if (a_pts == MP_NOPTS_VALUE)
        return MP_NOPTS_VALUE;

    // Data buffered in audio filters, measured in seconds of "missing" output
    double buffered_output = 0;

#if HAVE_LIBAF
    if (ao_c->af->initialized < 1)
        return MP_NOPTS_VALUE;

    buffered_output += af_calc_delay(ao_c->af);
#endif

    if (ao_c->conv)
        buffered_output += mp_aconverter_get_latency(ao_c->conv);

    if (ao_c->output_frame)
        buffered_output += mp_aframe_duration(ao_c->output_frame);

    // Data that was ready for ao but was buffered because ao didn't fully
    // accept everything to internal buffers yet
    buffered_output += mp_audio_buffer_seconds(ao_c->ao_buffer);

    // Filters divide audio length by audio_speed, so multiply by it
    // to get the length in original units without speedup or slowdown
    a_pts -= buffered_output * mpctx->audio_speed;

    return a_pts;
}

// Return pts value corresponding to currently playing audio.
double playing_audio_pts(struct MPContext *mpctx)
{
    double pts = written_audio_pts(mpctx);
    if (pts == MP_NOPTS_VALUE || !mpctx->ao)
        return pts;
    return pts - mpctx->audio_speed * ao_get_delay(mpctx->ao);
}

static int write_to_ao(struct MPContext *mpctx, uint8_t **planes, int samples,
                       int flags)
{
    if (mpctx->paused)
        return 0;
    struct ao *ao = mpctx->ao;
    int samplerate;
    int format;
    struct mp_chmap channels;
    ao_get_format(ao, &samplerate, &format, &channels);
#if HAVE_ENCODING
    encode_lavc_set_audio_pts(mpctx->encode_lavc_ctx, playing_audio_pts(mpctx));
#endif
    if (samples == 0)
        return 0;
    double real_samplerate = samplerate / mpctx->audio_speed;
    int played = ao_play(mpctx->ao, (void **)planes, samples, flags);
    assert(played <= samples);
    if (played > 0) {
        mpctx->shown_aframes += played;
        mpctx->delay += played / real_samplerate;
        mpctx->written_audio += played / (double)samplerate;
        return played;
    }
    return 0;
}

static void dump_audio_stats(struct MPContext *mpctx)
{
    if (!mp_msg_test(mpctx->log, MSGL_STATS))
        return;
    if (mpctx->audio_status != STATUS_PLAYING || !mpctx->ao || mpctx->paused) {
        mpctx->audio_stat_start = 0;
        return;
    }

    double delay = ao_get_delay(mpctx->ao);
    if (!mpctx->audio_stat_start) {
        mpctx->audio_stat_start = mp_time_us();
        mpctx->written_audio = delay;
    }
    double current_audio = mpctx->written_audio - delay;
    double current_time = (mp_time_us() - mpctx->audio_stat_start) / 1e6;
    MP_STATS(mpctx, "value %f ao-dev", current_audio - current_time);
}

// Return the number of samples that must be skipped or prepended to reach the
// target audio pts after a seek (for A/V sync or hr-seek).
// Return value (*skip):
//   >0: skip this many samples
//   =0: don't do anything
//   <0: prepend this many samples of silence
// Returns false if PTS is not known yet.
static bool get_sync_samples(struct MPContext *mpctx, int *skip)
{
    struct MPOpts *opts = mpctx->opts;
    *skip = 0;

    if (mpctx->audio_status != STATUS_SYNCING)
        return true;

    int ao_rate;
    int ao_format;
    struct mp_chmap ao_channels;
    ao_get_format(mpctx->ao, &ao_rate, &ao_format, &ao_channels);

    double play_samplerate = ao_rate / mpctx->audio_speed;

    if (!opts->initial_audio_sync) {
        mpctx->audio_status = STATUS_FILLING;
        return true;
    }

    double written_pts = written_audio_pts(mpctx);
    if (written_pts == MP_NOPTS_VALUE &&
        !mp_audio_buffer_samples(mpctx->ao_chain->ao_buffer))
        return false; // no audio read yet

    bool sync_to_video = mpctx->vo_chain && !mpctx->vo_chain->is_coverart &&
                         mpctx->video_status != STATUS_EOF;

    double sync_pts = MP_NOPTS_VALUE;
    if (sync_to_video) {
        if (mpctx->video_status < STATUS_READY)
            return false; // wait until we know a video PTS
        if (mpctx->video_pts != MP_NOPTS_VALUE)
            sync_pts = mpctx->video_pts - opts->audio_delay;
    } else if (mpctx->hrseek_active) {
        sync_pts = mpctx->hrseek_pts;
    } else {
        // If audio-only is enabled mid-stream during playback, sync accordingly.
        sync_pts = mpctx->playback_pts;
    }
    if (sync_pts == MP_NOPTS_VALUE) {
        mpctx->audio_status = STATUS_FILLING;
        return true; // syncing disabled
    }

    double ptsdiff = written_pts - sync_pts;
    // Missing timestamp, or PTS reset, or just broken.
    if (written_pts == MP_NOPTS_VALUE) {
        MP_WARN(mpctx, "Failed audio resync.\n");
        mpctx->audio_status = STATUS_FILLING;
        return true;
    }
    ptsdiff = MPCLAMP(ptsdiff, -3600, 3600);

    // Heuristic: if audio is "too far" ahead, and one of them is a separate
    // track, allow a refresh seek to the correct position to fix it.
    if (ptsdiff > 0.2 && mpctx->audio_allow_second_chance_seek && sync_to_video) {
        struct ao_chain *ao_c = mpctx->ao_chain;
        if (ao_c && ao_c->track && mpctx->vo_chain && mpctx->vo_chain->track &&
            ao_c->track->demuxer != mpctx->vo_chain->track->demuxer)
        {
            struct track *track = ao_c->track;
            double pts = mpctx->video_pts;
            if (pts != MP_NOPTS_VALUE)
                pts += get_track_seek_offset(mpctx, track);
            // (disable it first to make it take any effect)
            demuxer_select_track(track->demuxer, track->stream, pts, false);
            demuxer_select_track(track->demuxer, track->stream, pts, true);
            reset_audio_state(mpctx);
            MP_VERBOSE(mpctx, "retrying audio seek\n");
            return false;
        }
    }
    mpctx->audio_allow_second_chance_seek = false;

    int align = af_format_sample_alignment(ao_format);
    *skip = (int)(-ptsdiff * play_samplerate) / align * align;
    return true;
}


static bool copy_output(struct MPContext *mpctx, struct ao_chain *ao_c,
                        int minsamples, double endpts, bool eof, bool *seteof)
{
    struct mp_audio_buffer *outbuf = ao_c->ao_buffer;

    int ao_rate;
    int ao_format;
    struct mp_chmap ao_channels;
    ao_get_format(ao_c->ao, &ao_rate, &ao_format, &ao_channels);

    while (mp_audio_buffer_samples(outbuf) < minsamples) {
        int cursamples = mp_audio_buffer_samples(outbuf);
        int maxsamples = INT_MAX;
        if (endpts != MP_NOPTS_VALUE) {
            double rate = ao_rate / mpctx->audio_speed;
            double curpts = written_audio_pts(mpctx);
            if (curpts != MP_NOPTS_VALUE) {
                double remaining =
                    (endpts - curpts - mpctx->opts->audio_delay) * rate;
                maxsamples = MPCLAMP(remaining, 0, INT_MAX);
            }
        }

        if (!ao_c->output_frame || !mp_aframe_get_size(ao_c->output_frame)) {
            TA_FREEP(&ao_c->output_frame);
#if HAVE_LIBAF
            struct af_stream *afs = mpctx->ao_chain->af;
            if (af_output_frame(afs, eof) < 0)
                return true; // error, stop doing stuff
            struct mp_audio *mpa = af_read_output_frame(afs);
            ao_c->output_frame = mp_audio_to_aframe(mpa);
            talloc_free(mpa);
#else
            if (eof)
                mp_aconverter_write_input(ao_c->conv, NULL);
            mp_aconverter_set_speed(ao_c->conv, mpctx->audio_speed);
            bool got_eof;
            ao_c->output_frame = mp_aconverter_read_output(ao_c->conv, &got_eof);
#endif
        }

        if (!ao_c->output_frame)
            return false; // out of data

        if (cursamples + mp_aframe_get_size(ao_c->output_frame) > maxsamples) {
            if (cursamples < maxsamples) {
                uint8_t **data = mp_aframe_get_data_ro(ao_c->output_frame);
                mp_audio_buffer_append(outbuf, (void **)data,
                                       maxsamples - cursamples);
                mp_aframe_skip_samples(ao_c->output_frame,
                                       maxsamples - cursamples);
            }
            *seteof = true;
            return true;
        }

        uint8_t **data = mp_aframe_get_data_ro(ao_c->output_frame);
        mp_audio_buffer_append(outbuf, (void **)data,
                               mp_aframe_get_size(ao_c->output_frame));
        TA_FREEP(&ao_c->output_frame);
    }
    return true;
}

static int decode_new_frame(struct ao_chain *ao_c)
{
    if (ao_c->input_frame)
        return AD_OK;

    int res = DATA_EOF;
    if (ao_c->filter_src) {
        res = lavfi_request_frame_a(ao_c->filter_src, &ao_c->input_frame);
    } else if (ao_c->audio_src) {
        audio_work(ao_c->audio_src);
        res = audio_get_frame(ao_c->audio_src, &ao_c->input_frame);
    }

    if (ao_c->input_frame)
        mp_aframe_config_copy(ao_c->input_format, ao_c->input_frame);

    switch (res) {
    case DATA_OK:       return AD_OK;
    case DATA_WAIT:     return AD_WAIT;
    case DATA_AGAIN:    return AD_NO_PROGRESS;
    case DATA_STARVE:   return AD_STARVE;
    case DATA_EOF:      return AD_EOF;
    default:            abort();
    }
}

/* Try to get at least minsamples decoded+filtered samples in outbuf
 * (total length including possible existing data).
 * Return 0 on success, or negative AD_* error code.
 * In the former case outbuf has at least minsamples buffered on return.
 * In case of EOF/error it might or might not be. */
static int filter_audio(struct MPContext *mpctx, struct mp_audio_buffer *outbuf,
                        int minsamples)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
#if HAVE_LIBAF
    struct af_stream *afs = ao_c->af;
    if (afs->initialized < 1)
        return AD_ERR;
#else
    if (!ao_c->filter_input_format)
        return AD_ERR;
#endif

    MP_STATS(ao_c, "start audio");

    double endpts = get_play_end_pts(mpctx);

    bool eof = false;
    int res;
    while (1) {
        res = 0;

        if (copy_output(mpctx, ao_c, minsamples, endpts, false, &eof))
            break;

        res = decode_new_frame(ao_c);
        if (res == AD_NO_PROGRESS)
            continue;
        if (res == AD_WAIT || res == AD_STARVE)
            break;
        if (res < 0) {
            // drain filters first (especially for true EOF case)
            copy_output(mpctx, ao_c, minsamples, endpts, true, &eof);
            break;
        }

        // On format change, make sure to drain the filter chain.
#if HAVE_LIBAF
        struct mp_audio in_format;
        mp_audio_config_from_aframe(&in_format, ao_c->input_format);
        if (!mp_audio_config_equals(&afs->input, &in_format)) {
            copy_output(mpctx, ao_c, minsamples, endpts, true, &eof);
            res = AD_NEW_FMT;
            break;
        }
#else
        if (!mp_aframe_config_equals(ao_c->filter_input_format,
                                     ao_c->input_format))
        {
            copy_output(mpctx, ao_c, minsamples, endpts, true, &eof);
            res = AD_NEW_FMT;
            break;
        }
#endif

        double pts = mp_aframe_get_pts(ao_c->input_frame);
        if (pts == MP_NOPTS_VALUE) {
            ao_c->pts = MP_NOPTS_VALUE;
        } else {
            // Attempt to detect jumps in PTS. Even for the lowest sample rates
            // and with worst container rounded timestamp, this should be a
            // margin more than enough.
            double desync = pts - ao_c->pts;
            if (ao_c->pts != MP_NOPTS_VALUE && fabs(desync) > 0.1) {
                MP_WARN(ao_c, "Invalid audio PTS: %f -> %f\n",
                        ao_c->pts, pts);
                if (desync >= 5)
                    ao_c->pts_reset = true;
            }
            ao_c->pts = mp_aframe_end_pts(ao_c->input_frame);
        }

#if HAVE_LIBAF
        struct mp_audio *mpa = mp_audio_from_aframe(ao_c->input_frame);
        talloc_free(ao_c->input_frame);
        ao_c->input_frame = NULL;
        if (!mpa)
            abort();
        if (af_filter_frame(afs, mpa) < 0)
            return AD_ERR;
#else
        if (mp_aconverter_write_input(ao_c->conv, ao_c->input_frame))
            ao_c->input_frame = NULL;
#endif
    }

    if (res == 0 && mp_audio_buffer_samples(outbuf) < minsamples && eof)
        res = AD_EOF;

    MP_STATS(ao_c, "end audio");

    return res;
}

void reload_audio_output(struct MPContext *mpctx)
{
    if (!mpctx->ao)
        return;

    ao_reset(mpctx->ao);
    uninit_audio_out(mpctx);
    reinit_audio_filters(mpctx); // mostly to issue refresh seek

    // Whether we can use spdif might have changed. If we failed to use spdif
    // in the previous initialization, try it with spdif again (we'll fallback
    // to PCM again if necessary).
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (ao_c) {
        struct dec_audio *d_audio = ao_c->audio_src;
        if (d_audio && ao_c->spdif_failed) {
            ao_c->spdif_passthrough = true;
            ao_c->spdif_failed = false;
            d_audio->try_spdif = true;
#if HAVE_LIBAF
            ao_c->af->initialized = 0;
#endif
            TA_FREEP(&ao_c->filter_input_format);
            if (!audio_init_best_codec(d_audio)) {
                MP_ERR(mpctx, "Error reinitializing audio.\n");
                error_on_track(mpctx, ao_c->track);
            }
        }
    }

    mp_wakeup_core(mpctx);
}

void fill_audio_out_buffers(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    bool was_eof = mpctx->audio_status == STATUS_EOF;

    dump_audio_stats(mpctx);

    if (mpctx->ao && ao_query_and_reset_events(mpctx->ao, AO_EVENT_RELOAD))
        reload_audio_output(mpctx);

    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c)
        return;

    bool is_initialized = !!ao_c->filter_input_format;
#if HAVE_LIBAF
    is_initialized = ao_c->af->initialized == 1;
#endif

    if (!is_initialized || !mpctx->ao) {
        // Probe the initial audio format. Returns AD_OK (and does nothing) if
        // the format is already known.
        int r = AD_NO_PROGRESS;
        while (r == AD_NO_PROGRESS)
            r = decode_new_frame(mpctx->ao_chain);
        if (r == AD_WAIT)
            return; // continue later when new data is available
        if (r == AD_EOF) {
            mpctx->audio_status = STATUS_EOF;
            return;
        }
        reinit_audio_filters_and_output(mpctx);
        mp_wakeup_core(mpctx);
        return; // try again next iteration
    }

    if (ao_c->ao_resume_time > mp_time_sec()) {
        double remaining = ao_c->ao_resume_time - mp_time_sec();
        mp_set_timeout(mpctx, remaining);
        return;
    }

    if (mpctx->vo_chain && ao_c->pts_reset) {
        MP_VERBOSE(mpctx, "Reset playback due to audio timestamp reset.\n");
        reset_playback_state(mpctx);
        mp_wakeup_core(mpctx);
        return;
    }

    int ao_rate;
    int ao_format;
    struct mp_chmap ao_channels;
    ao_get_format(mpctx->ao, &ao_rate, &ao_format, &ao_channels);
    double play_samplerate = ao_rate / mpctx->audio_speed;
    int align = af_format_sample_alignment(ao_format);

    // If audio is infinitely fast, somehow try keeping approximate A/V sync.
    if (mpctx->audio_status == STATUS_PLAYING && ao_untimed(mpctx->ao) &&
        mpctx->video_status != STATUS_EOF && mpctx->delay > 0)
        return;

    int playsize = ao_get_space(mpctx->ao);

    int skip = 0;
    bool sync_known = get_sync_samples(mpctx, &skip);
    if (skip > 0) {
        playsize = MPMIN(skip + 1, MPMAX(playsize, 2500)); // buffer extra data
    } else if (skip < 0) {
        playsize = MPMAX(1, playsize + skip); // silence will be prepended
    }

    int skip_duplicate = 0; // >0: skip, <0: duplicate
    double drop_limit =
        (opts->sync_max_audio_change + opts->sync_max_video_change) / 100;
    if (mpctx->display_sync_active && opts->video_sync == VS_DISP_ADROP &&
        fabs(mpctx->last_av_difference) >= opts->sync_audio_drop_size &&
        mpctx->audio_drop_throttle < drop_limit &&
        mpctx->audio_status == STATUS_PLAYING)
    {
        int samples = ceil(opts->sync_audio_drop_size * play_samplerate);
        samples = (samples + align / 2) / align * align;

        skip_duplicate = mpctx->last_av_difference >= 0 ? -samples : samples;

        playsize = MPMAX(playsize, samples);

        mpctx->audio_drop_throttle += 1 - drop_limit - samples / play_samplerate;
    }

    playsize = playsize / align * align;

    int status = mpctx->audio_status >= STATUS_DRAINING ? AD_EOF : AD_OK;
    bool working = false;
    if (playsize > mp_audio_buffer_samples(ao_c->ao_buffer)) {
        status = filter_audio(mpctx, ao_c->ao_buffer, playsize);
        if (status == AD_WAIT)
            return;
        if (status == AD_NO_PROGRESS || status == AD_STARVE) {
            mp_wakeup_core(mpctx);
            return;
        }
        if (status == AD_NEW_FMT) {
            /* The format change isn't handled too gracefully. A more precise
             * implementation would require draining buffered old-format audio
             * while displaying video, then doing the output format switch.
             */
            if (mpctx->opts->gapless_audio < 1)
                uninit_audio_out(mpctx);
            reinit_audio_filters_and_output(mpctx);
            mp_wakeup_core(mpctx);
            return; // retry on next iteration
        }
        if (status == AD_ERR)
            mp_wakeup_core(mpctx);
        working = true;
    }

    // If EOF was reached before, but now something can be decoded, try to
    // restart audio properly. This helps with video files where audio starts
    // later. Retrying is needed to get the correct sync PTS.
    if (mpctx->audio_status >= STATUS_DRAINING &&
        mp_audio_buffer_samples(ao_c->ao_buffer) > 0)
    {
        mpctx->audio_status = STATUS_SYNCING;
        return; // retry on next iteration
    }

    bool end_sync = false;
    if (skip >= 0) {
        int max = mp_audio_buffer_samples(ao_c->ao_buffer);
        mp_audio_buffer_skip(ao_c->ao_buffer, MPMIN(skip, max));
        // If something is left, we definitely reached the target time.
        end_sync |= sync_known && skip < max;
        working |= skip > 0;
    } else if (skip < 0) {
        if (-skip > playsize) { // heuristic against making the buffer too large
            ao_reset(mpctx->ao); // some AOs repeat data on underflow
            mpctx->audio_status = STATUS_DRAINING;
            mpctx->delay = 0;
            return;
        }
        mp_audio_buffer_prepend_silence(ao_c->ao_buffer, -skip);
        end_sync = true;
    }

    if (skip_duplicate) {
        int max = mp_audio_buffer_samples(ao_c->ao_buffer);
        if (abs(skip_duplicate) > max)
            skip_duplicate = skip_duplicate >= 0 ? max : -max;
        mpctx->last_av_difference += skip_duplicate / play_samplerate;
        if (skip_duplicate >= 0) {
            mp_audio_buffer_skip(ao_c->ao_buffer, skip_duplicate);
            MP_STATS(mpctx, "drop-audio");
        } else {
            mp_audio_buffer_duplicate(ao_c->ao_buffer, -skip_duplicate);
            MP_STATS(mpctx, "duplicate-audio");
        }
        MP_VERBOSE(mpctx, "audio skip_duplicate=%d\n", skip_duplicate);
    }

    if (mpctx->audio_status == STATUS_SYNCING) {
        if (end_sync)
            mpctx->audio_status = STATUS_FILLING;
        if (status != AD_OK && !mp_audio_buffer_samples(ao_c->ao_buffer))
            mpctx->audio_status = STATUS_EOF;
        if (working || end_sync)
            mp_wakeup_core(mpctx);
        return; // continue on next iteration
    }

    assert(mpctx->audio_status >= STATUS_FILLING);

    // We already have as much data as the audio device wants, and can start
    // writing it any time.
    if (mpctx->audio_status == STATUS_FILLING)
        mpctx->audio_status = STATUS_READY;

    // Even if we're done decoding and syncing, let video start first - this is
    // required, because sending audio to the AO already starts playback.
    if (mpctx->audio_status == STATUS_READY) {
        if (mpctx->vo_chain && !mpctx->vo_chain->is_coverart &&
            mpctx->video_status <= STATUS_READY)
            return;
        MP_VERBOSE(mpctx, "starting audio playback\n");
    }

    bool audio_eof = status == AD_EOF;
    bool partial_fill = false;
    int playflags = 0;

    if (playsize > mp_audio_buffer_samples(ao_c->ao_buffer)) {
        playsize = mp_audio_buffer_samples(ao_c->ao_buffer);
        partial_fill = true;
    }

    audio_eof &= partial_fill;

    // With gapless audio, delay this to ao_uninit. There must be only
    // 1 final chunk, and that is handled when calling ao_uninit().
    if (audio_eof && !opts->gapless_audio)
        playflags |= AOPLAY_FINAL_CHUNK;

    uint8_t **planes;
    int samples;
    mp_audio_buffer_peek(ao_c->ao_buffer, &planes, &samples);
    if (audio_eof || samples >= align)
        samples = samples / align * align;
    samples = MPMIN(samples, mpctx->paused ? 0 : playsize);
    int played = write_to_ao(mpctx, planes, samples, playflags);
    assert(played >= 0 && played <= samples);
    mp_audio_buffer_skip(ao_c->ao_buffer, played);

    mpctx->audio_drop_throttle =
        MPMAX(0, mpctx->audio_drop_throttle - played / play_samplerate);

    dump_audio_stats(mpctx);

    mpctx->audio_status = STATUS_PLAYING;
    if (audio_eof && !playsize) {
        mpctx->audio_status = STATUS_DRAINING;
        // Wait until the AO has played all queued data. In the gapless case,
        // we trigger EOF immediately, and let it play asynchronously.
        if (ao_eof_reached(mpctx->ao) || opts->gapless_audio) {
            mpctx->audio_status = STATUS_EOF;
            if (!was_eof) {
                MP_VERBOSE(mpctx, "audio EOF reached\n");
                mp_wakeup_core(mpctx);
            }
        }
    }
}

// Drop data queued for output, or which the AO is currently outputting.
void clear_audio_output_buffers(struct MPContext *mpctx)
{
    if (mpctx->ao)
        ao_reset(mpctx->ao);
}
