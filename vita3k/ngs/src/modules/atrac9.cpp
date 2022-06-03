// Vita3K emulator project
// Copyright (C) 2022 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <ngs/modules/atrac9.h>
#include <util/bytes.h>
#include <util/log.h>

extern "C" {
#include <libswresample/swresample.h>
}

namespace ngs::atrac9 {

SwrContext *Module::swr_mono_to_stereo = nullptr;
SwrContext *Module::swr_stereo = nullptr;

Module::Module()
    : ngs::Module(ngs::BussType::BUSS_ATRAC9)
    , last_config(0) {}

void get_buffer_parameter(const std::uint32_t start_sample, const std::uint32_t num_samples, const std::uint32_t info, SkipBufferInfo &parameter) {
    const std::uint8_t sample_rate_index = ((info & (0b1111 << 12)) >> 12);
    const std::uint8_t block_rate_index = ((info & (0b111 << 9)) >> 9);
    const std::uint16_t frame_bytes = ((((info & 0xFF0000) >> 16) << 3) | ((info & (0b111 << 29)) >> 29)) + 1;
    const std::uint8_t superframe_index = (info & (0b11 << 27)) >> 27;

    // Calculate bytes per superframe.
    const std::uint32_t frame_per_superframe = 1 << superframe_index;
    const std::uint32_t bytes_per_superframe = frame_bytes * frame_per_superframe;

    // Calculate total superframe
    static const std::int8_t sample_rate_index_to_frame_sample_power[] = {
        6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8
    };

    const std::uint32_t samples_per_frame = 1 << sample_rate_index_to_frame_sample_power[sample_rate_index];
    const std::uint32_t samples_per_superframe = samples_per_frame * frame_per_superframe;

    const std::uint32_t start_superframe = (start_sample / samples_per_superframe);
    const std::uint32_t num_superframe = (start_sample + num_samples + samples_per_superframe - 1) / samples_per_superframe - start_superframe;

    parameter.num_bytes = num_superframe * bytes_per_superframe;
    parameter.is_super_packet = (frame_per_superframe == 1) ? 0 : 1;
    parameter.start_byte_offset = start_superframe * bytes_per_superframe;
    parameter.start_skip = (start_sample - (start_superframe * samples_per_superframe));
    parameter.end_skip = (start_superframe + num_superframe) * samples_per_superframe - (start_sample + num_samples);
}

std::size_t Module::get_buffer_parameter_size() const {
    return sizeof(Parameters);
}

void Module::on_state_change(ModuleData &data, const VoiceState previous) {
    State *state = data.get_state<State>();
    if (data.parent->state == VOICE_STATE_AVAILABLE) {
        state->current_byte_position_in_buffer = 0;
        state->current_loop_count = 0;
        state->current_buffer = 0;
    } else if (data.parent->is_keyed_off) {
        state->samples_generated_since_key_on = 0;
        state->bytes_consumed_since_key_on = 0;
    }
}

bool Module::decode_more_data(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, const Parameters *params, State *state, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    int current_buffer = state->current_buffer;
    const BufferParameters &bufparam = params->buffer_params[current_buffer];

    if (params->playback_frequency != 48000 || params->playback_scalar != 1.0) {
        static bool has_happened = false;
        LOG_WARN_IF(!has_happened, "Playback rate scaling not implemented for ngs atrac9 player");
        has_happened = true;
    }

    if (!data.extra_storage.empty()) {
        data.extra_storage.erase(data.extra_storage.begin(), data.extra_storage.begin() + state->decoded_passed * sizeof(float) * 2);
        state->decoded_passed = 0;
    }

    if (state->current_byte_position_in_buffer >= bufparam.bytes_count) {
        const std::int32_t prev_index = state->current_buffer;

        voice_lock.unlock();
        scheduler_lock.unlock();

        state->current_loop_count++;

        if (bufparam.loop_count != -1 && state->current_loop_count > bufparam.loop_count) {
            state->current_buffer = bufparam.next_buffer_index;
            state->current_loop_count = 0;

            if (state->current_buffer == -1) {
                data.invoke_callback(kern, mem, thread_id, SCE_NGS_AT9_END_OF_DATA, 0, 0);
                // TODO: Free all occupied input routes
                // unroute_occupied(mem, voice);
            } else {
                data.invoke_callback(kern, mem, thread_id, SCE_NGS_AT9_SWAPPED_BUFFER, state->current_loop_count,
                    params->buffer_params[state->current_buffer].buffer.address());
            }
        } else {
            // from what I understand, SCE_NGS_AT9_SWAPPED_BUFFER must be called even when it's just the current buffer looping
            data.invoke_callback(kern, mem, thread_id, SCE_NGS_AT9_SWAPPED_BUFFER, state->current_loop_count,
                params->buffer_params[state->current_buffer].buffer.address());
            data.invoke_callback(kern, mem, thread_id, SCE_NGS_AT9_LOOPED_BUFFER, state->current_loop_count,
                params->buffer_params[state->current_buffer].buffer.address());
        }

        scheduler_lock.lock();
        voice_lock.lock();

        state->current_byte_position_in_buffer = 0;
        current_buffer = state->current_buffer;

        if (current_buffer == -1)
            // we are done
            return false;

        if (params->buffer_params[current_buffer].bytes_count == 0) {
            // try to find a non-empty buffer that can be accessed, there are at most 4
            for (int k = 0; k < 4 && current_buffer != -1 && params->buffer_params[current_buffer].bytes_count == 0; k++) {
                current_buffer = params->buffer_params[current_buffer].next_buffer_index;
            }

            if (current_buffer == -1 || params->buffer_params[current_buffer].bytes_count == 0)
                // we are done
                return false;
        }

        // re-call this function
        return true;
    }
    // now we are sure we have a buffer with some data in it

    const uint32_t superframe_size = decoder->get(DecoderQuery::AT9_SUPERFRAME_SIZE);
    uint32_t frame_bytes_gotten = bufparam.bytes_count - state->current_byte_position_in_buffer;
    if (frame_bytes_gotten < superframe_size) {
        // I don't think this should happen
        static bool has_happened = false;
        LOG_CRITICAL_IF(!has_happened, "The supplied buffer isn't big enough for an atrac9 superframe or isn't aligned, report it to devs");
        has_happened = true;
        return false;
    }

    std::size_t curr_pos = state->decoded_samples_pending * sizeof(float) * 2;

    const uint32_t samples_per_frame = decoder->get(DecoderQuery::AT9_SAMPLE_PER_FRAME);
    const uint32_t samples_per_superframe = decoder->get(DecoderQuery::AT9_SAMPLE_PER_SUPERFRAME);
    // we need to account for sampled skipped at the beginning or the end of the buffer
    uint32_t decoded_size = samples_per_superframe;
    uint32_t decoded_start_offset = 0;

    // remove skipped samples at the beginnning and the end of the buffer
    // in case you have more than a superframe of samples (I don't know if this can happen)
    const uint32_t sample_index = (state->current_byte_position_in_buffer / superframe_size) * samples_per_superframe;
    if (bufparam.samples_discard_start_off > sample_index) {
        // first chunk
        const uint32_t skipped_samples = std::min(samples_per_superframe, bufparam.samples_discard_start_off - sample_index);
        decoded_start_offset += skipped_samples;
        decoded_size -= skipped_samples;
    }

    const uint32_t samples_left_after = (frame_bytes_gotten / superframe_size - 1) * samples_per_superframe;
    if (bufparam.samples_discard_end_off > samples_left_after) {
        // last chunk
        const uint32_t skipped_samples = std::min(samples_per_superframe, bufparam.samples_discard_end_off - samples_left_after);
        decoded_size -= bufparam.samples_discard_end_off;
    }

    data.extra_storage.resize(curr_pos + decoded_size * sizeof(float) * 2);

    auto *input = bufparam.buffer.cast<uint8_t>().get(mem) + state->current_byte_position_in_buffer;
    std::vector<uint8_t> decoded_superframe_samples(decoder->get(DecoderQuery::AT9_SAMPLE_PER_SUPERFRAME) * sizeof(float) * 2);
    uint32_t decoded_superframe_pos = 0;
    bool got_decode_error = false;
    // decode a whole superframe at a time
    for (int frame = 0; frame < decoder->get(DecoderQuery::AT9_FRAMES_IN_SUPERFRAME); frame++) {
        if (!decoder->send(input, 0)) {
            got_decode_error = true;
            break;
        }

        // convert from int16 to float
        uint32_t const channel_count = decoder->get(DecoderQuery::CHANNELS);
        uint32_t const sample_rate = decoder->get(DecoderQuery::SAMPLE_RATE);
        std::vector<uint8_t> temporary_bytes(samples_per_frame * sizeof(int16_t) * channel_count);
        DecoderSize decoder_size;
        decoder->receive(temporary_bytes.data(), &decoder_size);

        SwrContext *swr;
        if (channel_count == 1) {
            if (!swr_mono_to_stereo) {
                swr_mono_to_stereo = swr_alloc_set_opts(nullptr,
                    AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, 480000,
                    AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_S16, 480000,
                    0, nullptr);
                swr_init(swr_mono_to_stereo);
            }

            swr = swr_mono_to_stereo;
        } else {
            if (!swr_stereo) {
                swr_stereo = swr_alloc_set_opts(nullptr,
                    AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, 480000,
                    AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 480000,
                    0, nullptr);
                swr_init(swr_stereo);
            }

            swr = swr_stereo;
        }

        const uint8_t *swr_data_in = reinterpret_cast<uint8_t *>(temporary_bytes.data());
        uint8_t *swr_data_out = reinterpret_cast<uint8_t *>(decoded_superframe_samples.data() + decoded_superframe_pos);
        const int result = swr_convert(swr, &swr_data_out, decoder_size.samples, &swr_data_in, decoder_size.samples);

        decoded_superframe_pos += decoder_size.samples * sizeof(float) * 2;
        input += decoder->get_es_size();
        state->current_byte_position_in_buffer += decoder->get_es_size();
    }
    memcpy(data.extra_storage.data() + curr_pos, decoded_superframe_samples.data() + decoded_start_offset * sizeof(float) * 2, decoded_size * sizeof(float) * 2);

    if (got_decode_error) {
        voice_lock.unlock();
        scheduler_lock.unlock();

        data.invoke_callback(kern, mem, thread_id, SCE_NGS_AT9_DECODE_ERROR, state->current_byte_position_in_buffer,
            params->buffer_params[state->current_buffer].buffer.address());

        scheduler_lock.lock();
        voice_lock.lock();

        // clear the context or we'll get en error next time we cant to decode
        decoder->clear_context();
    }

    state->samples_generated_since_key_on += decoded_size;
    state->samples_generated_total += decoded_size;
    state->bytes_consumed_since_key_on += superframe_size;
    state->total_bytes_consumed += superframe_size;

    state->decoded_samples_pending += decoded_size;
    return true;
}

bool Module::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    const Parameters *params = data.get_parameters<Parameters>(mem);
    State *state = data.get_state<State>();
    assert(state);

    if ((state->current_buffer == -1) || (params->buffer_params[state->current_buffer].buffer.address() == 0)) {
        return true;
    }

    bool finished = false;
    // making this maybe too early...
    if (!decoder || (params->config_data != last_config)) {
        decoder = std::make_unique<Atrac9DecoderState>(params->config_data);
        last_config = params->config_data;
    }

    // call decode more data until we either have an error or reached end of data
    while (static_cast<std::int32_t>(state->decoded_samples_pending) < data.parent->rack->system->granularity) {
        if (!decode_more_data(kern, mem, thread_id, data, params, state, scheduler_lock, voice_lock)) {
            // still give something in the output buffer
            data.fill_to_fit_granularity();
            data.parent->products[0].data = data.extra_storage.data();
            // we are done
            state->samples_generated_since_key_on = 0;
            state->bytes_consumed_since_key_on = 0;
            return true;
        }
    }

    std::uint8_t *data_ptr = data.extra_storage.data() + 2 * sizeof(float) * state->decoded_passed;
    std::uint32_t samples_to_be_passed = data.parent->rack->system->granularity;

    data.parent->products[0].data = data_ptr;

    state->decoded_samples_pending = (state->decoded_samples_pending < samples_to_be_passed) ? 0 : (state->decoded_samples_pending - samples_to_be_passed);
    state->decoded_passed += samples_to_be_passed;

    return false;
}
} // namespace ngs::atrac9
