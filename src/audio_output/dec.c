/*****************************************************************************
 * dec.c : audio output API towards decoders
 *****************************************************************************
 * Copyright (C) 2002-2007 the VideoLAN team
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>

#include <vlc_aout.h>
#include <vlc_input.h>

#include "aout_internal.h"
#include "libvlc.h"

/**
 * Creates an audio output
 */
int aout_DecNew( audio_output_t *p_aout,
                 const audio_sample_format_t *p_format,
                 const audio_replay_gain_t *p_replay_gain,
                 const aout_request_vout_t *p_request_vout )
{
    /* Sanitize audio format */
    if( p_format->i_channels > 32 )
    {
        msg_Err( p_aout, "too many audio channels (%u)",
                 p_format->i_channels );
        return -1;
    }
    if( p_format->i_channels <= 0 )
    {
        msg_Err( p_aout, "no audio channels" );
        return -1;
    }
    if( p_format->i_channels != aout_FormatNbChannels( p_format ) )
    {
        msg_Err( p_aout, "incompatible audio channels count with layout mask" );
        return -1;
    }

    if( p_format->i_rate > 192000 )
    {
        msg_Err( p_aout, "excessive audio sample frequency (%u)",
                 p_format->i_rate );
        return -1;
    }
    if( p_format->i_rate < 4000 )
    {
        msg_Err( p_aout, "too low audio sample frequency (%u)",
                 p_format->i_rate );
        return -1;
    }

    aout_input_t *p_input = calloc( 1, sizeof(aout_input_t));
    if( !p_input )
        return -1;

    p_input->b_error = true;

    if( p_replay_gain )
        p_input->replay_gain = *p_replay_gain;

    /* We can only be called by the decoder, so no need to lock
     * p_input->lock. */
    aout_owner_t *owner = aout_owner(p_aout);
    aout_lock( p_aout );
    assert (owner->input == NULL);

    var_Destroy( p_aout, "audio-device" );
    var_Destroy( p_aout, "audio-channels" );

    /* Recreate the output using the new format. */
    owner->input_format = *p_format;
    if( aout_OutputNew( p_aout, p_format ) < 0 )
        goto error;

    assert (owner->volume.mixer == NULL);
    owner->volume.mixer = aout_MixerNew (p_aout, owner->mixer_format.i_format);

    date_Init (&owner->sync.date, owner->mixer_format.i_rate, 1);
    date_Set (&owner->sync.date, VLC_TS_INVALID);

    owner->input = p_input;
    aout_InputNew( p_aout, p_format, &owner->mixer_format, p_input,
                   p_request_vout );
    aout_unlock( p_aout );
    return 0;
error:
    aout_unlock( p_aout );
    free( p_input );
    return -1;
}

/*****************************************************************************
 * aout_DecDelete : delete a decoder
 *****************************************************************************/
void aout_DecDelete( audio_output_t * p_aout )
{
    aout_owner_t *owner = aout_owner (p_aout);
    aout_input_t *input;
    struct audio_mixer *mixer;

    aout_lock( p_aout );
    /* Remove the input. */
    input = owner->input;
    aout_InputDelete (p_aout, input);
    owner->input = NULL;

    aout_OutputDelete( p_aout );
    mixer = owner->volume.mixer;
    owner->volume.mixer = NULL;
    var_Destroy( p_aout, "audio-device" );
    var_Destroy( p_aout, "audio-channels" );

    aout_unlock( p_aout );

    aout_MixerDelete (mixer);
    free (input);
}

static void aout_CheckRestart (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);
    aout_input_t *input = owner->input;

    aout_assert_locked (aout);

    if (likely(!owner->need_restart))
        return;
    owner->need_restart = false;

    /* Reinitializes the output */
    aout_InputDelete (aout, owner->input);
    aout_MixerDelete (owner->volume.mixer);
    owner->volume.mixer = NULL;
    aout_OutputDelete (aout);

    if (aout_OutputNew (aout, &owner->input_format))
    {
        input->b_error = true;
        return; /* we are officially screwed */
    }

    owner->volume.mixer = aout_MixerNew (aout, owner->mixer_format.i_format);

    if (aout_InputNew (aout, &owner->input_format, &owner->mixer_format, input,
                       &input->request_vout))
        assert (input->b_error);
    else
        assert (!input->b_error);
}

/**
 * Restarts the audio filter chain if needed.
 */
static void aout_InputCheckAndRestart (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);
    aout_input_t *input = owner->input;

    aout_assert_locked (aout);

    if (!input->b_restart)
        return;
    input->b_restart = false;

    aout_InputDelete (aout, input);
    aout_InputNew (aout, &owner->input_format, &owner->mixer_format,
                   input, &input->request_vout);
}

/**
 * This function will safely mark aout input to be restarted as soon as
 * possible to take configuration changes into account
 */
void aout_InputRequestRestart (audio_output_t *aout)
{
    aout_lock (aout);
    if (aout_owner (aout)->input != NULL)
        aout_owner (aout)->input->b_restart = true;
    aout_unlock (aout);
}


/*
 * Buffer management
 */

/*****************************************************************************
 * aout_DecNewBuffer : ask for a new empty buffer
 *****************************************************************************/
block_t *aout_DecNewBuffer (audio_output_t *aout, size_t samples)
{
    /* NOTE: the caller is responsible for serializing input change */
    aout_owner_t *owner = aout_owner (aout);

    size_t length = samples * owner->input_format.i_bytes_per_frame
                            / owner->input_format.i_frame_length;
    block_t *block = block_Alloc( length );
    if( likely(block != NULL) )
    {
        block->i_nb_samples = samples;
        block->i_pts = block->i_length = 0;
    }
    return block;
}

/*****************************************************************************
 * aout_DecDeleteBuffer : destroy an undecoded buffer
 *****************************************************************************/
void aout_DecDeleteBuffer (audio_output_t *aout, block_t *block)
{
    (void) aout;
    aout_BufferFree (block);
}

/*****************************************************************************
 * aout_DecPlay : filter & mix the decoded buffer
 *****************************************************************************/
int aout_DecPlay (audio_output_t *p_aout, block_t *p_buffer, int i_input_rate)
{
    aout_owner_t *owner = aout_owner (p_aout);
    aout_input_t *p_input = owner->input;

    assert( i_input_rate >= INPUT_RATE_DEFAULT / AOUT_MAX_INPUT_RATE &&
            i_input_rate <= INPUT_RATE_DEFAULT * AOUT_MAX_INPUT_RATE );
    assert( p_buffer->i_pts > 0 );

    p_buffer->i_length = (mtime_t)p_buffer->i_nb_samples * 1000000
                                / owner->input_format.i_rate;

    aout_lock( p_aout );
    if( p_input->b_error )
    {
        aout_unlock( p_aout );
        aout_BufferFree( p_buffer );
        return -1;
    }

    aout_CheckRestart( p_aout );
    aout_InputCheckAndRestart (p_aout);

    /* Input */
    p_buffer = aout_InputPlay (p_aout, p_input, p_buffer, i_input_rate,
                               &owner->sync.date);
    if( p_buffer != NULL )
    {
        date_Increment (&owner->sync.date, p_buffer->i_nb_samples);

        /* Mixer */
        float amp = owner->volume.multiplier * p_input->multiplier;
        aout_MixerRun (owner->volume.mixer, p_buffer, amp);

        /* Output */
        aout_OutputPlay( p_aout, p_buffer );
    }

    aout_unlock( p_aout );
    return 0;
}

int aout_DecGetResetLost (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);
    aout_input_t *input = owner->input;
    int val;

    aout_lock (aout);
    val = input->i_buffer_lost;
    input->i_buffer_lost = 0;
    aout_unlock (aout);

    return val;
}

void aout_DecChangePause (audio_output_t *aout, bool paused, mtime_t date)
{
    aout_owner_t *owner = aout_owner (aout);

    aout_lock (aout);
    /* XXX: Should the date be offset by the pause duration instead? */
    date_Set (&owner->sync.date, VLC_TS_INVALID);
    aout_OutputPause (aout, paused, date);
    aout_unlock (aout);
}

void aout_DecFlush (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);

    aout_lock (aout);
    date_Set (&owner->sync.date, VLC_TS_INVALID);
    aout_OutputFlush (aout, false);
    aout_unlock (aout);
}

bool aout_DecIsEmpty (audio_output_t *aout)
{
    aout_owner_t *owner = aout_owner (aout);
    mtime_t end_date;

    aout_lock (aout);
    /* FIXME: tell output to drain */
    end_date = date_Get (&owner->sync.date);
    aout_unlock (aout);
    return end_date == VLC_TS_INVALID || end_date <= mdate();
}

/**
 * Notifies the audio input of the drift from the requested audio
 * playback timestamp (@ref block_t.i_pts) to the anticipated playback time
 * as reported by the audio output hardware.
 * Depending on the drift amplitude, the input core may ignore the drift
 * trigger upsampling or downsampling, or even discard samples.
 * Future VLC versions may instead adjust the input decoding speed.
 *
 * The audio output plugin is responsible for estimating the ideal current
 * playback time defined as follows:
 *  ideal time = buffer timestamp - (output latency + pending buffer duration)
 *
 * Practically, this is the PTS (block_t.i_pts) of the current buffer minus
 * the latency reported by the output programming interface.
 * Computing the estimated drift directly would probably be more intuitive.
 * However the use of an absolute time value does not introduce extra
 * measurement errors due to the CPU scheduling jitter and clock resolution.
 * Furthermore, the ideal while it is an abstract value, is easy for most
 * audio output plugins to compute.
 * The following definition is equivalent but depends on the clock time:
 *  ideal time = real time + drift

 * @note If aout_LatencyReport() is never called, the core will assume that
 * there is no drift.
 *
 * @param ideal estimated ideal time as defined above.
 */
void aout_TimeReport (audio_output_t *aout, mtime_t ideal)
{
    mtime_t delta = mdate() - ideal /* = -drift */;

    aout_assert_locked (aout);
    if (delta < -AOUT_MAX_PTS_ADVANCE || +AOUT_MAX_PTS_DELAY < delta)
    {
        aout_owner_t *owner = aout_owner (aout);

        msg_Warn (aout, "not synchronized (%"PRId64" us), resampling",
                  delta);
        if (date_Get (&owner->sync.date) != VLC_TS_INVALID)
            date_Move (&owner->sync.date, delta);
    }
}
