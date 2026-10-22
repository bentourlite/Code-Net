/* C interface to the Code Net receiver, for the Swift menu-bar app. */
#ifndef CODENET_C_H
#define CODENET_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cn_receiver cn_receiver;

typedef struct {
    const char* name;   /* this receiver's name, up to 16 bytes */
    const char* main;   /* main host name, or "" until one is assigned */
    const char* backup; /* backup host name, or "" */
    const char* group;  /* multicast group, or "none" for unicast only */
    int port;           /* frame port, normally 5260 */
    int loss_timeout_frames;
    int max_predict_frames;
    int wifi;           /* ride through Wi-Fi sized gaps (6 frames) */
    int auto_return;    /* go back to main after it has been stable for 10 s */
    double advance_ms;  /* start each frame this much earlier (device delay) */
} cn_receiver_config;

/* Starts listening. Returns NULL and fills `error` on failure. */
cn_receiver* cn_receiver_create(const cn_receiver_config* config, char* error, int error_size);
void cn_receiver_destroy(cn_receiver* receiver);

void cn_receiver_set_sources(cn_receiver* receiver, const char* main, const char* backup);
void cn_receiver_return_to_main(cn_receiver* receiver);

/* Called on the network thread when a host app assigns a new main/backup. */
typedef void (*cn_assign_callback)(void* context, const char* main, const char* backup);
void cn_receiver_set_assign_callback(cn_receiver* receiver, cn_assign_callback callback, void* context);

/* Output advance in ms (positive = earlier), to cancel the output device's delay. */
void cn_receiver_set_advance(cn_receiver* receiver, double advance_ms);

/* Called on the network thread when a host app sets the advance (Calibrate). */
typedef void (*cn_advance_callback)(void* context, double advance_ms);
void cn_receiver_set_advance_callback(cn_receiver* receiver, cn_advance_callback callback, void* context);

/* Copies the timecode on the output right now ("HH:MM:SS:FF") into `buffer`;
   returns 0, with an empty string, while the output is silent. */
int cn_receiver_timecode_now(cn_receiver* receiver, char* buffer, int size);

/* Copies the latest status JSON into `buffer`; returns its full length. */
int cn_receiver_status(cn_receiver* receiver, char* buffer, int size);

/* Output devices: LTC on an audio interface, or MTC to a MIDI port such as a MIF4. */
enum { CN_DEVICE_AUDIO = 0, CN_DEVICE_MIDI = 1 };

typedef struct {
    char id[256];   /* Core Audio device UID, or the MIDI endpoint's unique id */
    char name[256];
    int kind;
    int channels;   /* audio outputs; 0 for MIDI */
} cn_device;

/* Fills up to `max` devices; returns how many there are. */
int cn_list_devices(cn_device* out, int max);

/* Starts output, replacing any previous one. `channel` is the 0-based audio output
   channel for LTC, or -1 for all channels. Returns 0, or -1 with `error` filled. */
int cn_receiver_start_output(cn_receiver* receiver, int kind, const char* device_id, int channel, float level,
                             char* error, int error_size);
void cn_receiver_stop_output(cn_receiver* receiver);
void cn_receiver_set_level(cn_receiver* receiver, float level);

/* The local clock in nanoseconds (the Mac host clock). */
int64_t cn_now_ns(void);

#ifdef __cplusplus
}
#endif

#endif
