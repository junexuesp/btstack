/*
 * Copyright (C) 2022 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "le_audio_unicast_sink.c"

/*
 * LE Audio Unicast Sink
 * Until GATT Services are available, we encode LC3 config in advertising
 */


#include "btstack_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>        // open
#include <errno.h>

#include "ad_parser.h"
#include "bluetooth_data_types.h"
#include "bluetooth_company_id.h"
#include "bluetooth_gatt.h"
#include "btstack_debug.h"
#include "btstack_audio.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_ring_buffer.h"
#include "btstack_stdin.h"
#include "btstack_util.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "btstack_lc3.h"
#include "btstack_lc3_google.h"
#include "btstack_lc3plus_fraunhofer.h"

#ifdef HAVE_POSIX_FILE_IO
#include "wav_util.h"
#endif

// max config
#define MAX_CHANNELS 2
#define MAX_SAMPLES_PER_FRAME 480

// playback
#define MAX_NUM_LC3_FRAMES   5
#define MAX_BYTES_PER_SAMPLE 4
#define PLAYBACK_BUFFER_SIZE (MAX_NUM_LC3_FRAMES * MAX_SAMPLES_PER_FRAME * MAX_BYTES_PER_SAMPLE)

// analysis
#define PACKET_PREFIX_LEN 10

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

static void show_usage(void);

static const char * filename_wav = "le_audio_unicast_sink.wav";

static enum {
    APP_W4_WORKING,
    APP_W4_SOURCE_ADV,
    APP_W4_CIG_COMPLETE,
    APP_W4_CIS_CREATED,
    APP_SET_ISO_PATHS,
    APP_STREAMING,
    APP_IDLE
} app_state = APP_W4_WORKING;

//
static btstack_packet_callback_registration_t hci_event_callback_registration;

uint32_t last_samples_report_ms;
uint32_t samples_received;
uint32_t samples_dropped;
uint16_t frames_per_second[MAX_CHANNELS];

// remote info
static char             remote_name[20];
static bd_addr_t        remote_addr;
static bd_addr_type_t   remote_type;
static hci_con_handle_t remote_handle;

static bool count_mode;
static bool pts_mode;

static le_audio_cig_t cig;
static le_audio_cig_params_t cig_params;

// iso info
static bool framed_pdus;
static uint16_t frame_duration_us;
static uint8_t num_cis;
static hci_con_handle_t cis_con_handles[MAX_CHANNELS];
static bool cis_established[MAX_CHANNELS];
static unsigned int     next_cis_index;

// analysis
static uint16_t last_packet_sequence[MAX_CHANNELS];
static uint32_t last_packet_time_ms[MAX_CHANNELS];
static uint8_t last_packet_prefix[MAX_CHANNELS * PACKET_PREFIX_LEN];

// lc3 writer
static int dump_file;
static uint32_t lc3_frames;

// lc3 codec config
static uint16_t sampling_frequency_hz;
static btstack_lc3_frame_duration_t frame_duration;
static uint16_t number_samples_per_frame;
static uint16_t octets_per_frame;
static uint8_t  num_channels;

// lc3 decoder
static bool request_lc3plus_decoder = false;
static bool use_lc3plus_decoder = false;
static const btstack_lc3_decoder_t * lc3_decoder;
static int16_t pcm[MAX_CHANNELS * MAX_SAMPLES_PER_FRAME];

static btstack_lc3_decoder_google_t google_decoder_contexts[MAX_CHANNELS];
#ifdef HAVE_LC3PLUS
static btstack_lc3plus_fraunhofer_decoder_t fraunhofer_decoder_contexts[MAX_CHANNELS];
#endif
static void * decoder_contexts[MAX_NR_BIS];

// playback
static uint8_t playback_buffer_storage[PLAYBACK_BUFFER_SIZE];
static btstack_ring_buffer_t playback_buffer;

// PLC state
#define PLC_GUARD_MS 1

static btstack_timer_source_t next_packet_timer;
static uint16_t               cached_iso_sdu_len;

static void le_audio_connection_sink_playback(int16_t * buffer, uint16_t num_samples){
    // called from lower-layer but guaranteed to be on main thread
    uint32_t bytes_needed = num_samples * num_channels * 2;

    static bool underrun = true;

    log_info("Playback: need %u, have %u", num_samples, btstack_ring_buffer_bytes_available(&playback_buffer) / (num_channels * 2));

    if (bytes_needed > btstack_ring_buffer_bytes_available(&playback_buffer)){
        memset(buffer, 0, bytes_needed);
        if (underrun == false){
            log_info("Playback underrun");
            underrun = true;
        }
        return;
    }

    if (underrun){
        underrun = false;
        log_info("Playback started");
    }
    uint32_t bytes_read;
    btstack_ring_buffer_read(&playback_buffer, (uint8_t *) buffer, bytes_needed, &bytes_read);
    btstack_assert(bytes_read == bytes_needed);
}

static void setup_lc3_decoder(void){
    uint8_t channel;
    for (channel = 0 ; channel < num_channels ; channel++){
        // pick decoder
        void * decoder_context = NULL;
#ifdef HAVE_LC3PLUS
        if (use_lc3plus_decoder){
            decoder_context = &fraunhofer_decoder_contexts[channel];
            lc3_decoder = btstack_lc3plus_fraunhofer_decoder_init_instance(decoder_context);
        }
        else
#endif
        {
            decoder_context = &google_decoder_contexts[channel];
            lc3_decoder = btstack_lc3_decoder_google_init_instance(decoder_context);
        }
        decoder_contexts[channel] = decoder_context;
        lc3_decoder->configure(decoder_context, sampling_frequency_hz, frame_duration);
    }
    number_samples_per_frame = lc3_decoder->get_number_samples_per_frame(decoder_contexts[0]);
    btstack_assert(number_samples_per_frame <= MAX_SAMPLES_PER_FRAME);
}

static void close_files(void){
#ifdef HAVE_POSIX_FILE_IO
    printf("Close files\n");
    close(dump_file);
    wav_writer_close();
#endif
}

static void enter_streaming(void){

    // switch to lc3plus if requested and possible
    use_lc3plus_decoder = request_lc3plus_decoder && (frame_duration == BTSTACK_LC3_FRAME_DURATION_10000US);

    // init decoder
    setup_lc3_decoder();

    printf("Configure: %u channels, sampling rate %u, samples per frame %u, lc3plus %u\n", num_channels, sampling_frequency_hz, number_samples_per_frame, use_lc3plus_decoder);

#ifdef HAVE_POSIX_FILE_IO
    // create wav file
    printf("WAV file: %s\n", filename_wav);
    wav_writer_open(filename_wav, num_channels, sampling_frequency_hz);
#endif

    // init playback buffer
    btstack_ring_buffer_init(&playback_buffer, playback_buffer_storage, PLAYBACK_BUFFER_SIZE);

    // start playback
    const btstack_audio_sink_t * sink = btstack_audio_sink_get_instance();
    if (sink != NULL){
        sink->init(num_channels, sampling_frequency_hz, le_audio_connection_sink_playback);
        sink->start_stream();
    }
}

static void start_scanning() {
    app_state = APP_W4_SOURCE_ADV;
    gap_set_scan_params(1, 0x30, 0x30, 0);
    gap_start_scan();
    printf("Start scan..\n");
}

static void create_cig() {
    if (sampling_frequency_hz == 44100){
        framed_pdus = 1;
        // same config as for 48k -> frame is longer by 48/44.1
        frame_duration_us = frame_duration == BTSTACK_LC3_FRAME_DURATION_7500US ? 8163 : 10884;
    } else {
        framed_pdus = 0;
        frame_duration_us = frame_duration == BTSTACK_LC3_FRAME_DURATION_7500US ? 7500 : 10000;
    }

    printf("Send: LE Set CIG Parameters\n");

    num_cis = 1;
    cig_params.cig_id = 0;
    cig_params.num_cis = 1;
    cig_params.sdu_interval_c_to_p = frame_duration_us;
    cig_params.sdu_interval_p_to_c = frame_duration_us;
    cig_params.worst_case_sca = 0; // 251 ppm to 500 ppm
    cig_params.packing = 0;        // sequential
    cig_params.framing = framed_pdus;
    cig_params.max_transport_latency_c_to_p = 40;
    cig_params.max_transport_latency_p_to_c = 40;
    uint8_t i;
    for (i=0; i < num_cis; i++){
        cig_params.cis_params[i].cis_id = i;
        cig_params.cis_params[i].max_sdu_c_to_p = 0;
        cig_params.cis_params[i].max_sdu_p_to_c = num_channels * octets_per_frame;
        cig_params.cis_params[i].phy_c_to_p = 2;  // 2M
        cig_params.cis_params[i].phy_p_to_c = 2;  // 2M
        cig_params.cis_params[i].rtn_c_to_p = 2;
        cig_params.cis_params[i].rtn_p_to_c = 2;
    }

    app_state = APP_W4_CIG_COMPLETE;
    gap_cig_create(&cig, &cig_params);
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    bd_addr_t event_addr;
    if (packet_type != HCI_EVENT_PACKET) return;
    unsigned int i;
    switch (packet[0]) {
        case BTSTACK_EVENT_STATE:
            switch(btstack_event_state_get_state(packet)) {
                case HCI_STATE_WORKING:
#ifdef ENABLE_DEMO_MODE
                    if (app_state != APP_W4_WORKING) break;
                    start_scanning();
#else
                    show_usage();
#endif
                    break;
                case HCI_STATE_OFF:
                    printf("Goodbye\n");
                    exit(0);
                    break;
                default:
                    break;
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (hci_event_disconnection_complete_get_connection_handle(packet) == remote_handle){
                printf("Disconnected, back to scanning\n");
                // stop playback
                const btstack_audio_sink_t * sink = btstack_audio_sink_get_instance();
                if (sink != NULL){
                    sink->stop_stream();
                }

                start_scanning();
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
        {
            if (app_state != APP_W4_SOURCE_ADV) break;

            gap_event_advertising_report_get_address(packet, remote_addr);
            uint8_t adv_size = gap_event_advertising_report_get_data_length(packet);
            const uint8_t * adv_data = gap_event_advertising_report_get_data(packet);

            ad_context_t context;
            bool found = false;
            remote_name[0] = '\0';
            uint16_t uuid;
            uint16_t company_id;
            for (ad_iterator_init(&context, adv_size, adv_data) ; ad_iterator_has_more(&context) ; ad_iterator_next(&context)) {
                uint8_t data_type = ad_iterator_get_data_type(&context);
                uint8_t size = ad_iterator_get_data_len(&context);
                const uint8_t *data = ad_iterator_get_data(&context);
                switch (data_type){
                    case BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA:
                        company_id = little_endian_read_16(data, 0);
                        if (company_id == BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH){
                            // subtype = 0 -> le audio unicast source
                            uint8_t subtype = data[2];
                            if (subtype != 0) break;
                            // flags
                            uint8_t flags = data[3];
                            pts_mode = (flags & 1) != 0;
                            count_mode = (flags & 2) != 0;
                            // num channels
                            num_channels = data[4];
                            if (num_channels > 2) break;
                            // sampling frequency
                            sampling_frequency_hz = 1000 * data[5];
                            // frame duration
                            frame_duration = data[6] == 0 ? BTSTACK_LC3_FRAME_DURATION_7500US : BTSTACK_LC3_FRAME_DURATION_10000US;
                            // octets per frame
                            octets_per_frame = data[7];
                            // done
                            found = true;
                        }
                        break;
                    case BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME:
                    case BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME:
                        size = btstack_min(sizeof(remote_name) - 1, size);
                        memcpy(remote_name, data, size);
                        remote_name[size] = 0;
                        break;
                    default:
                        break;
                }
            }
            if (!found) break;
            remote_type = gap_event_advertising_report_get_address_type(packet);
            pts_mode   = false;
            count_mode = false;
            printf("Remote Broadcast source found, addr %s, name: '%s' (pts-mode: %u, count: %u)\n", bd_addr_to_str(remote_addr), remote_name, pts_mode, count_mode);
            // stop scanning
            app_state = APP_W4_CIS_CREATED;
            gap_stop_scan();
            gap_connect(remote_addr, remote_type);
            break;
        }

        case HCI_EVENT_LE_META:
            switch(hci_event_le_meta_get_subevent_code(packet)) {
                case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                    enter_streaming();
                    hci_subevent_le_connection_complete_get_peer_address(packet, event_addr);
                    remote_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                    printf("Connected, remote %s, handle %04x\n", bd_addr_to_str(event_addr), remote_handle);
                    create_cig();
                    break;
                case HCI_SUBEVENT_LE_CIS_ESTABLISHED:
                {
                    // only look for cis handle
                    uint8_t i;
                    hci_con_handle_t cis_handle = hci_subevent_le_cis_established_get_connection_handle(packet);
                    for (i=0; i < num_cis; i++){
                        if (cis_handle == cis_con_handles[i]){
                            cis_established[i] = true;
                        }
                    }
                    // check for complete
                    bool complete = true;
                    for (i=0; i < num_cis; i++) {
                        complete &= cis_established[i];
                    }
                    if (complete) {
                        printf("All CIS Established\n");
                        next_cis_index = 0;
                        app_state = APP_SET_ISO_PATHS;
                    }
                    break;
                }
                default:
                    break;
            }
        case HCI_EVENT_META_GAP:
            switch (hci_event_gap_meta_get_subevent_code(packet)){
                case GAP_SUBEVENT_CIG_CREATED:
                    if (app_state == APP_W4_CIG_COMPLETE){
                        uint8_t i;
                        printf("CIS Connection Handles: ");
                        for (i=0; i < num_cis; i++){
                            cis_con_handles[i] = gap_subevent_cig_created_get_cis_con_handles(packet, i);
                            printf("0x%04x ", cis_con_handles[i]);
                        }
                        printf("\n");
                        next_cis_index = 0;

                        printf("Create CIS\n");
                        hci_con_handle_t acl_connection_handles[MAX_CHANNELS];
                        for (i=0; i < num_cis; i++){
                            acl_connection_handles[i] = remote_handle;
                        }
                        gap_cis_create(cig_params.cig_id, cis_con_handles, acl_connection_handles);
                        app_state = APP_W4_CIS_CREATED;
                    }
                    break;
            }
        default:
            break;
    }

    if (!hci_can_send_command_packet_now()) return;

    switch(app_state){
        case APP_SET_ISO_PATHS:
            hci_send_cmd(&hci_le_setup_iso_data_path, cis_con_handles[next_cis_index++], 1, 0, 0, 0, 0, 0, 0, NULL);
            if (next_cis_index == num_cis){
                app_state = APP_STREAMING;
                last_samples_report_ms = btstack_run_loop_get_time_ms();
            }
            break;
        default:
            break;
    }
}

static void store_samples_in_ringbuffer(void){
#ifdef HAVE_POSIX_FILE_IO
    // write wav samples
    wav_writer_write_int16(num_channels * number_samples_per_frame, pcm);
#endif
    // store samples in playback buffer
    uint32_t bytes_to_store = num_channels * number_samples_per_frame * 2;
    samples_received += number_samples_per_frame;
    if (btstack_ring_buffer_bytes_free(&playback_buffer) >= bytes_to_store) {
        btstack_ring_buffer_write(&playback_buffer, (uint8_t *) pcm, bytes_to_store);
    } else {
        printf("Samples dropped\n");
        samples_dropped += number_samples_per_frame;
    }
}

static void plc_timeout(btstack_timer_source_t * timer) {
    // set timer again
    uint32_t frame_duration_ms = frame_duration == BTSTACK_LC3_FRAME_DURATION_7500US ? 8 : 10;
    btstack_run_loop_set_timer(&next_packet_timer, frame_duration_ms);
    btstack_run_loop_set_timer_handler(&next_packet_timer, plc_timeout);
    btstack_run_loop_add_timer(&next_packet_timer);

    // inject packet
    uint8_t tmp_BEC_detect;
    uint8_t BFI = 1;
    uint8_t channel;
    for (channel = 0; channel < num_channels; channel++){
        (void) lc3_decoder->decode_signed_16(decoder_contexts[channel], NULL, cached_iso_sdu_len, BFI,
                                             &pcm[channel], num_channels,
                                             &tmp_BEC_detect);
    }

    store_samples_in_ringbuffer();
}

static void iso_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){

    uint16_t header = little_endian_read_16(packet, 0);
    hci_con_handle_t con_handle = header & 0x0fff;
    uint8_t pb_flag = (header >> 12) & 3;
    uint8_t ts_flag = (header >> 14) & 1;
    uint16_t iso_load_len = little_endian_read_16(packet, 2);

    uint16_t offset = 4;
    uint32_t time_stamp = 0;
    if (ts_flag){
        uint32_t time_stamp = little_endian_read_32(packet, offset);
        offset += 4;
    }

    uint16_t packet_sequence_number = little_endian_read_16(packet, offset);
    offset += 2;

    uint16_t header_2 = little_endian_read_16(packet, offset);
    uint16_t iso_sdu_length = header_2 & 0x3fff;
    uint8_t packet_status_flag = (uint8_t) (header_2 >> 14);
    offset += 2;

    if (iso_sdu_length == 0) return;

    if (iso_sdu_length != num_channels * octets_per_frame) {
        printf("ISO Length %u != %u * %u\n", iso_sdu_length, num_channels, octets_per_frame);
    }

    // infer channel from con handle - only works for up to 2 channels
    uint8_t cis_channel = (con_handle == cis_con_handles[0]) ? 0 : 1;

    if (count_mode){
        // check for missing packet
        uint16_t last_seq_no = last_packet_sequence[cis_channel];
        uint32_t now = btstack_run_loop_get_time_ms();
        bool packet_missed = (last_seq_no != 0) && ((last_seq_no + 1) != packet_sequence_number);
        if (packet_missed){
            // print last packet
            printf("\n");
            printf("%04x %10u %u ", last_seq_no, last_packet_time_ms[cis_channel], cis_channel);
            printf_hexdump(&last_packet_prefix[num_channels * PACKET_PREFIX_LEN], PACKET_PREFIX_LEN);
            last_seq_no++;

            printf(ANSI_COLOR_RED);
            while (last_seq_no < packet_sequence_number){
                printf("%04x            %u MISSING\n", last_seq_no, cis_channel);
                last_seq_no++;
            }
            printf(ANSI_COLOR_RESET);

            // print current packet
            printf("%04x %10u %u ", packet_sequence_number, now, cis_channel);
            printf_hexdump(&packet[offset], PACKET_PREFIX_LEN);
        }

        // cache current packet
        last_packet_time_ms[cis_channel] = now;
        last_packet_sequence[cis_channel] = packet_sequence_number;
        memcpy(&last_packet_prefix[num_channels * PACKET_PREFIX_LEN], &packet[offset], PACKET_PREFIX_LEN);

    } else {

        uint8_t channel;
        for (channel = 0 ; channel < num_channels ; channel++){

            // decode codec frame
            uint8_t tmp_BEC_detect;
            uint8_t BFI = 0;
            (void) lc3_decoder->decode_signed_16(decoder_contexts[channel], &packet[offset], octets_per_frame, BFI,
                                       &pcm[channel], num_channels,
                                       &tmp_BEC_detect);
            offset += octets_per_frame;
        }

        store_samples_in_ringbuffer();

        log_info("Samples in playback buffer %5u", btstack_ring_buffer_bytes_available(&playback_buffer) / (num_channels * 2));

        lc3_frames++;
        frames_per_second[cis_channel]++;

        // PLC
        cached_iso_sdu_len = iso_sdu_length;
        uint32_t frame_duration_ms = frame_duration == BTSTACK_LC3_FRAME_DURATION_7500US ? 8 : 10;
        btstack_run_loop_remove_timer(&next_packet_timer);
        btstack_run_loop_set_timer(&next_packet_timer, frame_duration_ms + PLC_GUARD_MS);
        btstack_run_loop_set_timer_handler(&next_packet_timer, plc_timeout);
        btstack_run_loop_add_timer(&next_packet_timer);

        uint32_t time_ms = btstack_run_loop_get_time_ms();
        if (btstack_time_delta(time_ms, last_samples_report_ms) > 1000){
            last_samples_report_ms = time_ms;
            printf("LC3 Frames: %4u - ", lc3_frames / num_channels);
            uint8_t i;
            for (i=0; i < num_channels; i++){
                printf("%u ", frames_per_second[i]);
                frames_per_second[i] = 0;
            }
            printf(" frames per second, dropped %u of %u\n", samples_dropped, samples_received);
            samples_received = 0;
            samples_dropped  =  0;
        }
    }
}

static void show_usage(void){
    printf("\n--- LE Audio Unicast Sink Test Console ---\n");
    printf("x - close files and exit\n");
    printf("s - start scanning\n");
#ifdef HAVE_LC3PLUS
    printf("q - use LC3plus decoder if 10 ms ISO interval is used\n");
#endif
    printf("---\n");
}

static void stdin_process(char c){
    switch (c){
        case 's':
            if (app_state != APP_W4_WORKING) break;
            start_scanning();
            break;
#ifdef HAVE_LC3PLUS
        case 'q':
            request_lc3plus_decoder = true;
            break;
#endif
            case 'x':
            close_files();
            printf("Shutdown...\n");
            hci_power_control(HCI_POWER_OFF);
            break;
        case '\n':
        case '\r':
            break;
        default:
            show_usage();
            break;

    }
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    (void) argv;
    (void) argc;

    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // register for ISO Packet
    hci_register_iso_packet_handler(&iso_packet_handler);

    // turn on!
    hci_power_control(HCI_POWER_ON);

    btstack_stdin_setup(stdin_process);
    return 0;
}
