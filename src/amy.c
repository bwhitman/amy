// libAMY

// Brian Whitman
// brian@variogr.am

#include "amy.h"

// Defaults
__attribute__((weak)) const uint16_t amy_block_size = 256;
__attribute__((weak)) const uint8_t amy_block_size_bits = 8;
__attribute__((weak)) const uint16_t amy_oscs = 120;
__attribute__((weak)) const uint16_t amy_sample_rate = 44100;

#ifdef ARDUINO
    __attribute__((weak)) const uint8_t amy_cores = 1;
#elif defined PICO_ON_DEVICE || defined ESP_PLATFORM
    __attribute__((weak)) const uint8_t amy_cores = 2;
#else
    __attribute__((weak)) const uint8_t amy_cores = 1;
#endif

#ifdef ALLES
    __attribute__((weak)) const uint8_t amy_nchans = 1;
#else
    __attribute__((weak)) const uint8_t amy_nchans = 2;
#endif

#ifdef ALLES
    __attribute__((weak)) const uint8_t amy_has_chorus = 1;
    __attribute__((weak)) const uint8_t amy_has_reverb = 0;
#elif defined PICO_ON_DEVICE || defined ARDUINO_ARCH_RP2040
    __attribute__((weak)) const uint8_t amy_has_chorus = 1;
    __attribute__((weak)) const uint8_t amy_has_reverb = 0;
#else
    __attribute__((weak)) const uint8_t amy_has_chorus = 1;
    __attribute__((weak)) const uint8_t amy_has_reverb = 1;
#endif


#include "clipping_lookup_table.h"
#if AMY_HAS_CHORUS == 1 || AMY_HAS_REVERB == 1
#include "delay.h"
// Final output delay lines.
delay_line_t *delay_lines[AMY_NCHANS];
#endif


#ifdef ESP_PLATFORM
// Defined in amy-example-esp32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
extern SemaphoreHandle_t xQueueSemaphore;
extern TaskHandle_t amy_render_handle[AMY_CORES]; // one per core
#endif


// Global state 
struct state global;
// set of deltas for the fifo to be played
struct delta * events;
// state per osc as multi-channel synthesizer that the scheduler renders into
struct synthinfo * synth;
// envelope-modified per-osc state
struct mod_synthinfo * msynth;

// Two mixing blocks, one per core of rendering
SAMPLE ** fbl;
SAMPLE ** per_osc_fb; 

#ifndef malloc_caps
void * malloc_caps(uint32_t size, uint32_t flags) {
#ifdef ESP_PLATFORM
    //fprintf(stderr, "allocing size %ld flags %ld\n", size, flags);
    return heap_caps_malloc(size, flags);
#else
    // ignore flags
    return malloc(size);
#endif
}
#endif


#if defined PICO_ON_DEVICE  || defined ARDUINO_ARCH_RP2040
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#endif


#if AMY_HAS_CHORUS == 1
// CHORUS_ARATE means that chorus delay is updated at full audio rate and
// the chorus delay lines have per-sample variable delays.  Otherwise,
// the chorus oscillator is only evalated once per block (~11ms) and the
// delay is constant within each block.
#ifdef CHORUS_ARATE
SAMPLE *delay_mod = NULL;
#else
SAMPLE delay_mod_val = 0;
#endif // CHORUS_ARATE

typedef struct chorus_config {
    SAMPLE level;     // How much of the delayed signal to mix in to the output, typ F2S(0.5).
    int max_delay;   // Max delay when modulating.  Must be <= DELAY_LINE_LEN
} chorus_config_t;


chorus_config_t chorus = {CHORUS_DEFAULT_LEVEL, CHORUS_DEFAULT_MAX_DELAY};

void alloc_delay_lines(void) {
    for(uint16_t c=0;c<AMY_NCHANS;++c) {
        delay_lines[c] = new_delay_line(DELAY_LINE_LEN, DELAY_LINE_LEN / 2, CHORUS_RAM_CAPS);
    }
#ifdef CHORUS_ARATE
    delay_mod = (SAMPLE *)malloc_caps(sizeof(SAMPLE) * AMY_BLOCK_SIZE, CHORUS_RAM_CAPS);
#endif
}


void config_chorus(float level, int max_delay) {
    // we just config mix level and max_delay here.  modulation freq/amp/shape comes from osc 63.
    if (level > 0) {
        // only allocate delay lines if chorus is more than inaudible.
        if (delay_lines[0] == NULL) {
            alloc_delay_lines();
        }
        // if we're turning on for the first time, start the oscillator.
        if (chorus.level == 0) {
#ifdef CHORUS_ARATE
            osc_note_on(CHORUS_MOD_SOURCE);
#endif
        }
        // apply max_delay.
        for (int core=0; core<AMY_CORES; ++core) {
            for (int chan=0; chan<AMY_NCHANS; ++chan) {
                //delay_lines[chan]->max_delay = max_delay;
                delay_lines[chan]->fixed_delay = (int)max_delay / 2;
            }
        }
    }
    chorus.max_delay = max_delay;
    chorus.level = F2S(level);
}
#endif // AMY_HAS_CHORUS


#if (AMY_HAS_REVERB == 1)
typedef struct reverb_state {
    SAMPLE level;
    float liveness;
    float damping;
    float xover_hz;
} reverb_state_t;

reverb_state_t reverb = {F2S(REVERB_DEFAULT_LEVEL), REVERB_DEFAULT_LIVENESS, REVERB_DEFAULT_DAMPING, REVERB_DEFAULT_XOVER_HZ};

void config_reverb(float level, float liveness, float damping, float xover_hz) {
    if (level > 0) {
        //printf("config_reverb: level %f liveness %f xover %f damping %f\n",
        //      level, liveness, xover_hz, damping);
        if (reverb.level == 0) init_stereo_reverb();  // In case it's the first time
        config_stereo_reverb(liveness, xover_hz, damping);
    }
    reverb.level = F2S(level);
    reverb.liveness = liveness;
    reverb.damping = damping;
    reverb.xover_hz = xover_hz;
}
#endif // AMY_HAS_REVERB

// block -- what gets sent to the dac -- -32768...32767 (int16 le)
output_sample_type * block;
uint32_t total_samples;
uint32_t event_counter ;
uint32_t message_counter ;

char *message_start_pointer;
int16_t message_length;

int32_t computed_delta; // can be negative no prob, but usually host is larger # than client
uint8_t computed_delta_set; // have we set a delta yet?

int8_t check_init(amy_err_t (*fn)(), char *name) {
    fprintf(stderr,"starting %s: ", name);
    const amy_err_t ret = (*fn)();
    if(ret != AMY_OK) {
#ifdef ESP_PLATFORM
        fprintf(stderr,"[error:%i (%s)]\n", ret, esp_err_to_name((esp_err_t)ret));
#else
        fprintf(stderr,"[error:%i]\n", ret);
#endif
        return -1;
    }
    fprintf(stderr,"[ok]\n");
    return 0;
}


void default_amy_parse_callback(char mode, char * message) {
    // do nothing
}

int8_t global_init() {
    // function pointers
    //amy_parse_callback = &default_amy_parse_callback;
    global.next_event_write = 0;
    global.event_start = NULL;
    global.event_qsize = 0;
    global.volume = 1.0f;
    global.latency_ms = 0;
    global.eq[0] = F2S(1.0f);
    global.eq[1] = F2S(1.0f);
    global.eq[2] = F2S(1.0f);
    global.hpf_state = 0;
    return 0;
}


float freq_for_midi_note(uint8_t midi_note) {
    return 440.0f*powf(2,(midi_note-69.0f)/12.0f);
}

float logfreq_for_midi_note(uint8_t midi_note) {
    // TODO: Precompensate for EPS_FOR_LOG
    return midi_note / 12.0f;
}

// Convert to and from the log-frequency scale.
// A log-frequency scale is good for summing control inputs.
// Offset to add to scales when taking logs to avoid large neg values.
#define EPS_FOR_LOG 0.015625

float logfreq_of_freq(float freq) {
    // logfreq is defined as log_2(freq / 8.18 Hz)
    if (freq==0) return 0;
    return log2f(freq / AMY_MIDI0_HZ);
}

float freq_of_logfreq(float logfreq) {
    if (logfreq==0) return 0;
    return AMY_MIDI0_HZ * exp2f(logfreq) - EPS_FOR_LOG;
}


// create a new default API accessible event
struct event amy_default_event() {
    struct event e;
    e.status = EMPTY;
    AMY_UNSET(e.time);
    AMY_UNSET(e.osc);
    AMY_UNSET(e.patch);
    AMY_UNSET(e.wave);
    AMY_UNSET(e.phase);
    AMY_UNSET(e.duty);
    AMY_UNSET(e.feedback);
    AMY_UNSET(e.velocity);
    AMY_UNSET(e.midi_note);
    AMY_UNSET(e.amp); 
    AMY_UNSET(e.freq);
    AMY_UNSET(e.volume);
    AMY_UNSET(e.pan);
    AMY_UNSET(e.latency_ms);
    AMY_UNSET(e.ratio);
    AMY_UNSET(e.filter_freq);
    AMY_UNSET(e.resonance);
    AMY_UNSET(e.filter_type);
    AMY_UNSET(e.mod_source);
    AMY_UNSET(e.mod_target);
    AMY_UNSET(e.eq_l);
    AMY_UNSET(e.eq_m);
    AMY_UNSET(e.eq_h);
    AMY_UNSET(e.algorithm);
    AMY_UNSET(e.bp0_target);
    AMY_UNSET(e.bp1_target);
    AMY_UNSET(e.bp2_target);
    e.algo_source[0] = 0;
    e.bp0[0] = 0;
    e.bp1[0] = 0;
    e.bp2[0] = 0;
    return e;
}

void add_delta_to_queue(struct delta d) {
#if defined ESP_PLATFORM && !defined ARDUINO
    //  take the queue mutex before starting
    xSemaphoreTake(xQueueSemaphore, portMAX_DELAY);
#endif
    //printf("add_delta: time %lld osc %d param %d freq %f\n", total_samples, d.osc, d.param, *(float *)&d.data);
    if(global.event_qsize < AMY_EVENT_FIFO_LEN) {
        // scan through the memory to find a free slot, starting at write pointer
        uint16_t write_location = global.next_event_write;
        int16_t found = -1;
        // guaranteed to find eventually if qsize stays accurate
        while(found<0) {
            if(events[write_location].time == UINT32_MAX) found = write_location;
            write_location = (write_location + 1) % AMY_EVENT_FIFO_LEN;
        }
        // found a mem location. copy the data in and update the write pointers.
        events[found].time = d.time;
        events[found].osc = d.osc;
        events[found].param = d.param;
        events[found].data = d.data;
        global.next_event_write = write_location;
        global.event_qsize++;

        // now insert it into the sorted list for fast playback
        // first, see if it's eariler than the first item, special case
        if(d.time < global.event_start->time) {
            events[found].next = global.event_start;
            global.event_start = &events[found];
        } else {
            // or it's got to be found somewhere
            struct delta* ptr = global.event_start; 
            int8_t inserted = -1;
            while(inserted<0) {
                if(d.time < ptr->next->time) { 
                    // next should point to me, and my next should point to old next
                    events[found].next = ptr->next;
                    ptr->next = &events[found];
                    inserted = 1;
                }
                ptr = ptr->next;
            }
        }
        event_counter++;

    } else {
        // if there's no room in the queue, just skip the message
        // todo -- report this somehow? 
    }
#if defined ESP_PLATFORM  && !defined ARDUINO
    xSemaphoreGive( xQueueSemaphore );
#endif
}

// Add a API facing event, convert into delta directly
void amy_add_event(struct event e) {
    struct delta d;

    // Synth defaults if not set, these are required for the delta struct
    if(AMY_IS_UNSET(e.osc)) { d.osc = 0; } else { d.osc = e.osc; }
    if(AMY_IS_UNSET(e.time)) { d.time = 0; } else { d.time = e.time; }

    // Everything else only added to queue if set
    if(AMY_IS_SET(e.wave)) { d.param=WAVE; d.data = *(uint32_t *)&e.wave; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.patch)) { d.param=PATCH; d.data = *(uint32_t *)&e.patch; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.midi_note)) { d.param=MIDI_NOTE; d.data = *(uint32_t *)&e.midi_note; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.amp)) {  d.param=AMP; d.data = *(uint32_t *)&e.amp; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.duty)) { d.param=DUTY; d.data = *(uint32_t *)&e.duty; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.feedback)) { d.param=FEEDBACK; d.data = *(uint32_t *)&e.feedback; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.freq)) { float logfreq = logfreq_of_freq(e.freq); d.param=FREQ; d.data = *(uint32_t *)&logfreq; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.phase)) { d.param=PHASE; d.data = *(uint32_t *)&e.phase; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.volume)) { d.param=VOLUME; d.data = *(uint32_t *)&e.volume; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.pan)) { d.param=PAN; d.data = *(uint32_t *)&e.pan; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.latency_ms)) { d.param=LATENCY; d.data = *(uint32_t *)&e.latency_ms; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.ratio)) { float logratio = log2f(e.ratio); d.param=RATIO; d.data = *(uint32_t *)&logratio; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.filter_freq)) { float filter_logfreq = logfreq_of_freq(e.filter_freq); d.param=FILTER_FREQ; d.data = *(uint32_t *)&filter_logfreq; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.resonance)) { d.param=RESONANCE; d.data = *(uint32_t *)&e.resonance; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.mod_source)) { d.param=MOD_SOURCE; d.data = *(uint32_t *)&e.mod_source; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.mod_target)) { d.param=MOD_TARGET; d.data = *(uint32_t *)&e.mod_target; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.bp0_target)) { d.param=BP0_TARGET; d.data = *(uint32_t *)&e.bp0_target; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.bp1_target)) { d.param=BP1_TARGET; d.data = *(uint32_t *)&e.bp1_target; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.bp2_target)) { d.param=BP2_TARGET; d.data = *(uint32_t *)&e.bp2_target; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.filter_type)) { d.param=FILTER_TYPE; d.data = *(uint32_t *)&e.filter_type; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.algorithm)) { d.param=ALGORITHM; d.data = *(uint32_t *)&e.algorithm; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.eq_l)) { d.param=EQ_L; d.data = *(uint32_t *)&e.eq_l; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.eq_m)) { d.param=EQ_M; d.data = *(uint32_t *)&e.eq_m; add_delta_to_queue(d); }
    if(AMY_IS_SET(e.eq_h)) { d.param=EQ_H; d.data = *(uint32_t *)&e.eq_h; add_delta_to_queue(d); }

    if(e.algo_source[0] != 0) {
        struct synthinfo t;
        parse_algorithm_source(&t, e.algo_source);
        for(uint8_t i=0;i<MAX_ALGO_OPS;i++) { if(AMY_IS_SET(t.algo_source[i])) { d.param=ALGO_SOURCE_START+i; d.data = *(uint32_t *)&t.algo_source[i]; add_delta_to_queue(d); } }
    }


    char * bps[3] = {e.bp0, e.bp1, e.bp2};
    for(uint8_t i=0;i<3;i++) {
        if(bps[i][0] != 0) {
            struct synthinfo t;
            parse_breakpoint(&t, bps[i], i);
            for(uint8_t j=0;j<MAX_BREAKPOINTS;j++) {
                if(AMY_IS_SET(t.breakpoint_times[i][j])) { d.param=BP_START+(j*2)+(i*MAX_BREAKPOINTS*2); d.data = *(uint32_t *)&t.breakpoint_times[i][j]; add_delta_to_queue(d); }
                if(AMY_IS_SET(t.breakpoint_values[i][j])) { d.param=BP_START+(j*2 + 1)+(i*MAX_BREAKPOINTS*2); d.data = *(uint32_t *)&t.breakpoint_values[i][j]; add_delta_to_queue(d); }
            }
        }
    }

    // add this last -- this is a trigger, that if sent alongside osc setup parameters, you want to run after those
    if(AMY_IS_SET(e.velocity)) {  d.param=VELOCITY; d.data = *(uint32_t *)&e.velocity; add_delta_to_queue(d); }
    message_counter++;
}


void reset_osc(uint16_t i ) {
    // set all the synth state to defaults
    synth[i].osc = i; // self-reference to make updating oscs easier
    synth[i].wave = SINE;
    synth[i].duty = 0.5f;
    msynth[i].duty = 0.5f;
    msynth[i].last_duty = 0.5f;
    AMY_UNSET(synth[i].patch);
    synth[i].midi_note = 0;
    //synth[i].freq = 0;
    synth[i].logfreq = 0;
    //msynth[i].freq = 0;
    msynth[i].logfreq = 0;
    synth[i].feedback = F2S(0); //.996; todo ks feedback is v different from fm feedback
    msynth[i].feedback = F2S(0); //.996; todo ks feedback is v different from fm feedback
    synth[i].amp = 1.0f;
    msynth[i].amp = 1.0f;
    synth[i].phase = F2P(0);
    synth[i].volume = 0;
    synth[i].pan = 0.5f;
    msynth[i].pan = 0.5f;
    synth[i].eq_l = 0;
    synth[i].eq_m = 0;
    synth[i].eq_h = 0;
    AMY_UNSET(synth[i].logratio);
    //synth[i].filter_freq = 0;
    synth[i].filter_logfreq = 0;
    //msynth[i].filter_freq = 0;
    msynth[i].filter_logfreq = 0;
    synth[i].resonance = 0.7f;
    msynth[i].resonance = 0.7f;
    synth[i].velocity = 0;
    synth[i].step = 0;
    synth[i].sample = F2S(0);
    synth[i].substep = 0;
    synth[i].status = OFF;
    AMY_UNSET(synth[i].mod_source);
    synth[i].mod_target = 0; 
    AMY_UNSET(synth[i].note_on_clock);
    AMY_UNSET(synth[i].note_off_clock);
    synth[i].filter_type = FILTER_NONE;
    synth[i].lpf_state = 0;
    synth[i].lpf_alpha = 0;
    synth[i].last_amp = 0;
    synth[i].dc_offset = 0;
    synth[i].algorithm = 0;
    for(uint8_t j=0;j<MAX_ALGO_OPS;j++) AMY_UNSET(synth[i].algo_source[j]);
    for(uint8_t j=0;j<MAX_BREAKPOINT_SETS;j++) { 
        for(uint8_t k=0;k<MAX_BREAKPOINTS;k++) { 
            AMY_UNSET(synth[i].breakpoint_times[j][k]); 
            AMY_UNSET(synth[i].breakpoint_values[j][k]);
        } 
        synth[i].breakpoint_target[j] = 0;
    }
    for(uint8_t j=0;j<MAX_BREAKPOINT_SETS;j++) { synth[i].last_scale[j] = 0; }
    synth[i].last_two[0] = 0;
    synth[i].last_two[1] = 0;
}

void amy_reset_oscs() {
    for(uint16_t i=0;i<AMY_OSCS;i++) reset_osc(i);
    // also reset filters and volume
    global.volume = 1.0f;
    global.eq[0] = F2S(1.0f);
    global.eq[1] = F2S(1.0f);
    global.eq[2] = F2S(1.0f);
    // also reset chorus oscillator.
    //synth[CHORUS_MOD_SOURCE].freq = CHORUS_DEFAULT_LFO_FREQ;
    synth[CHORUS_MOD_SOURCE].logfreq = logfreq_of_freq(CHORUS_DEFAULT_LFO_FREQ);
    synth[CHORUS_MOD_SOURCE].amp = CHORUS_DEFAULT_MOD_DEPTH;
    synth[CHORUS_MOD_SOURCE].wave = TRIANGLE;
    // and the chorus params
    #if ( AMY_HAS_CHORUS == 1)
    config_chorus(CHORUS_DEFAULT_LEVEL, CHORUS_DEFAULT_MAX_DELAY);
    #endif
    #if ( AMY_HAS_REVERB == 1)
    config_reverb(REVERB_DEFAULT_LEVEL, REVERB_DEFAULT_LIVENESS, REVERB_DEFAULT_DAMPING, REVERB_DEFAULT_XOVER_HZ);
    #endif
}



// the synth object keeps held state, whereas events are only deltas/changes
int8_t oscs_init() {
    #if AMY_KS_OSCS > 0
    ks_init();
    #endif
    filters_init();
    algo_init();
    pcm_init();
    events = (struct delta*)malloc_caps(sizeof(struct delta) * AMY_EVENT_FIFO_LEN, EVENTS_RAM_CAPS);
    synth = (struct synthinfo*) malloc_caps(sizeof(struct synthinfo) * AMY_OSCS, SYNTH_RAM_CAPS);
    msynth = (struct mod_synthinfo*) malloc_caps(sizeof(struct mod_synthinfo) * AMY_OSCS, SYNTH_RAM_CAPS);
    block = (output_sample_type *) malloc_caps(sizeof(output_sample_type) * AMY_BLOCK_SIZE * AMY_NCHANS, BLOCK_RAM_CAPS);
    // set all oscillators to their default values
    amy_reset_oscs();

    // make a fencepost last event with no next, time of end-1, and call it start for now, all other events get inserted before it
    events[0].next = NULL;
    events[0].time = UINT32_MAX - 1;
    events[0].osc = 0;
    events[0].data = 0;
    events[0].param = NO_PARAM;
    global.next_event_write = 1;
    global.event_start = &events[0];
    global.event_qsize = 1; // queue will always have at least 1 thing in it 

    // set all the other events to empty
    for(uint16_t i=1;i<AMY_EVENT_FIFO_LEN;i++) { 
        events[i].time = UINT32_MAX;
        events[i].next = NULL;
        events[i].osc = 0;
        events[i].data = 0;
        events[i].param = NO_PARAM;
    }
    fbl = (SAMPLE**) malloc_caps(sizeof(SAMPLE*) * AMY_CORES, FBL_RAM_CAPS); // one per core, just core 0 used off esp32
    per_osc_fb = (SAMPLE**) malloc_caps(sizeof(SAMPLE*) * AMY_CORES, FBL_RAM_CAPS); // one per core, just core 0 used off esp32
    // clear out both as local mode won't use fbl[1] 
    for(uint16_t core=0;core<AMY_CORES;++core) {
        fbl[core]= (SAMPLE*)malloc_caps(sizeof(SAMPLE) * AMY_BLOCK_SIZE * AMY_NCHANS, FBL_RAM_CAPS);
        per_osc_fb[core]= (SAMPLE*)malloc_caps(sizeof(SAMPLE) * AMY_BLOCK_SIZE, FBL_RAM_CAPS);
        for(uint16_t c=0;c<AMY_NCHANS;++c) {
            for(uint16_t i=0;i<AMY_BLOCK_SIZE;i++) { 
                fbl[core][AMY_BLOCK_SIZE*c + i] = 0; 
            }
        }
    }
    // we only alloc delay lines if the chorus is turned on.
    #if AMY_HAS_CHORUS > 0 || AMY_HAS_REVERB > 0
    for (int c = 0; c < AMY_NCHANS; ++c)  delay_lines[c] = NULL;
    #endif
    //init_stereo_reverb();
    
    total_samples = 0;
    computed_delta = 0;
    computed_delta_set = 0;
    event_counter = 0;
    message_counter = 0;
    return 0;
}


void show_debug(uint8_t type) { 
    if(type>1) {
        struct delta * ptr = global.event_start;
        uint16_t q = global.event_qsize;
        if(q > 25) q = 25;
        for(uint16_t i=0;i<q;i++) {
            fprintf(stderr,"%d time %" PRIu32 " osc %d param %d - %f %d\n", i, ptr->time, ptr->osc, ptr->param, *(float *)&ptr->data, *(int *)&ptr->data);
            ptr = ptr->next;
        }
    }
    if(type>2) {
        // print out all the osc data
        //printf("global: filter %f resonance %f volume %f status %d\n", global.filter_freq, global.resonance, global.volume, global.status);
        fprintf(stderr,"global: volume %f eq: %f %f %f \n", global.volume, S2F(global.eq[0]), S2F(global.eq[1]), S2F(global.eq[2]));
        //printf("mod global: filter %f resonance %f\n", mglobal.filter_freq, mglobal.resonance);
        for(uint16_t i=0;i<AMY_OSCS;i++) {
            //fprintf(stderr,"osc %d: status %d amp %f wave %d logfreq %f duty %f mod_target %d mod source %d velocity %f filter_logfreq %f logratio %f feedback %f resonance %f step %f algo %d pan %f source %d,%d,%d,%d,%d,%d  \n",
            //        i, synth[i].status, synth[i].amp, synth[i].wave, synth[i].logfreq, synth[i].duty, synth[i].mod_target, synth[i].mod_source, 
            //        synth[i].velocity, synth[i].filter_logfreq, synth[i].logratio, synth[i].feedback, synth[i].resonance, P2F(synth[i].step), synth[i].algorithm, synth[i].pan,
            //        synth[i].algo_source[0], synth[i].algo_source[1], synth[i].algo_source[2], synth[i].algo_source[3], synth[i].algo_source[4], synth[i].algo_source[5] );
            if(type>3) { 
                for(uint8_t j=0;j<MAX_BREAKPOINT_SETS;j++) {
                    fprintf(stderr,"bp%d (target %d): ", j, synth[i].breakpoint_target[j]);
                    for(uint8_t k=0;k<MAX_BREAKPOINTS;k++) {
                        fprintf(stderr,"%" PRIi32 ": %f ", synth[i].breakpoint_times[j][k], synth[i].breakpoint_values[j][k]);
                    }
                    fprintf(stderr,"\n");
                }
                fprintf(stderr,"mod osc %d: amp: %f, logfreq %f duty %f filter_logfreq %f resonance %f fb/bw %f pan %f \n", i, msynth[i].amp, msynth[i].logfreq, msynth[i].duty, msynth[i].filter_logfreq, msynth[i].resonance, msynth[i].feedback, msynth[i].pan);
            }
        }
    }
}


   
void oscs_deinit() {
    free(block);
    free(fbl[0]);
    if(AMY_CORES>1)free(fbl[1]);
    free(fbl);
    free(synth);
    free(msynth);
    free(events);
    #if AMY_KS_OSCS > 0
    ks_deinit();
    #endif
    filters_deinit();
}


void osc_note_on(uint16_t osc) {
    if(synth[osc].wave==SINE) sine_note_on(osc);
    if(synth[osc].wave==SAW_DOWN) saw_down_note_on(osc);
    if(synth[osc].wave==SAW_UP) saw_up_note_on(osc);
    if(synth[osc].wave==TRIANGLE) triangle_note_on(osc);
    if(synth[osc].wave==PULSE) pulse_note_on(osc);
    if(synth[osc].wave==PCM) pcm_note_on(osc);
    if(synth[osc].wave==ALGO) algo_note_on(osc);
    #if AMY_HAS_PARTIALS == 1
    if(synth[osc].wave==PARTIAL)  partial_note_on(osc);
    if(synth[osc].wave==PARTIALS) partials_note_on(osc);
    #endif
}

// play an event, now -- tell the audio loop to start making noise
void play_event(struct delta d) {
    uint8_t trig=0;
    // todo: event-only side effect, remove
    if(d.param == MIDI_NOTE) {
        synth[d.osc].midi_note = *(uint16_t *)&d.data;
        //synth[d.osc].freq = freq_for_midi_note(*(uint16_t *)&d.data);
        synth[d.osc].logfreq = logfreq_for_midi_note(*(uint16_t *)&d.data);
        //printf("time %lld osc %d midi_note %d logfreq %f\n", total_samples, d.osc, synth[d.osc].midi_note, synth[d.osc].logfreq);
    } 

    if(d.param == WAVE) {
        synth[d.osc].wave = *(uint16_t *)&d.data; 
        // todo: event-only side effect, remove
        // we do this because we need to set up LUTs for FM oscs. it's a TODO to make this cleaner 
        if(synth[d.osc].wave == SINE) {
            sine_note_on(d.osc);
        }
    }
    if(d.param == PHASE) synth[d.osc].phase = *(PHASOR *)&d.data;  // PHASOR
    if(d.param == PAN) { synth[d.osc].pan = *(float *)&d.data; /*fprintf(stderr, "pan osc %d is now %f\n", d.osc, synth[d.osc].pan);*/ }
    if(d.param == PATCH) synth[d.osc].patch = *(uint16_t *)&d.data;
    if(d.param == DUTY) synth[d.osc].duty = *(float *)&d.data;
    if(d.param == FEEDBACK) synth[d.osc].feedback = *(float *)&d.data;
    if(d.param == AMP) synth[d.osc].amp = *(float *)&d.data;
    if(d.param == FREQ) synth[d.osc].logfreq = *(float *)&d.data;
    
    if(d.param == BP0_TARGET) { synth[d.osc].breakpoint_target[0] = *(uint8_t *)&d.data; trig=1; }
    if(d.param == BP1_TARGET) { synth[d.osc].breakpoint_target[1] = *(uint8_t *)&d.data; trig=1; }
    if(d.param == BP2_TARGET) { synth[d.osc].breakpoint_target[2] = *(uint8_t *)&d.data; trig=1; }
    // todo, i really should clean this up
    if(d.param >= BP_START && d.param < BP_END) {
        uint8_t pos = d.param - BP_START;
        uint8_t bp_set = 0;
        if(pos > (MAX_BREAKPOINTS * 2)) { bp_set = 1; pos = pos - (MAX_BREAKPOINTS * 2); }
        if(pos > (MAX_BREAKPOINTS * 4)) { bp_set = 2; pos = pos - (MAX_BREAKPOINTS * 2); } 
        if(pos % 2 == 0) {
            synth[d.osc].breakpoint_times[bp_set][pos / 2] = *(uint32_t *)&d.data;
        } else {
            synth[d.osc].breakpoint_values[bp_set][(pos-1) / 2] = *(float *)&d.data;
        }
        trig=1;
    }
    if(trig) synth[d.osc].note_on_clock = total_samples;

    // todo: event-only side effect, remove
    if(d.param == MOD_SOURCE) { synth[d.osc].mod_source = *(uint16_t *)&d.data; synth[*(uint16_t *)&d.data].status = IS_MOD_SOURCE; }
    if(d.param == MOD_TARGET) synth[d.osc].mod_target = *(uint16_t *)&d.data; 

    if(d.param == RATIO) synth[d.osc].logratio = *(float *)&d.data;

    if(d.param == FILTER_FREQ) synth[d.osc].filter_logfreq = *(float *)&d.data;
    if(d.param == FILTER_TYPE) synth[d.osc].filter_type = *(uint8_t *)&d.data; 
    if(d.param == RESONANCE) synth[d.osc].resonance = *(float *)&d.data;

    if(d.param == ALGORITHM) synth[d.osc].algorithm = *(uint8_t *)&d.data; 

    if(d.param >= ALGO_SOURCE_START && d.param < ALGO_SOURCE_END) {
        uint16_t which_source = d.param - ALGO_SOURCE_START;
        synth[d.osc].algo_source[which_source] = *(int16_t *)&d.data; 
        if(synth[d.osc].algo_source[which_source] >= 0) synth[*(int16_t*)&d.data].status=IS_ALGO_SOURCE;
    }

    // for global changes, just make the change, no need to update the per-osc synth
    if(d.param == VOLUME) global.volume = *(float *)&d.data;
    if(d.param == LATENCY) { global.latency_ms = *(uint16_t *)&d.data; computed_delta_set = 0; computed_delta = 0; }
    if(d.param == EQ_L) global.eq[0] = F2S(powf(10, *(float *)&d.data / 20.0));
    if(d.param == EQ_M) global.eq[1] = F2S(powf(10, *(float *)&d.data / 20.0));
    if(d.param == EQ_H) global.eq[2] = F2S(powf(10, *(float *)&d.data / 20.0));

    // triggers / envelopes 
    // the only way a sound is made is if velocity (note on) is >0.
    if(d.param == VELOCITY && *(float *)&d.data > 0) { // new note on (even if something is already playing on this osc)
        synth[d.osc].amp = *(float *)&d.data; // these could be decoupled, later
        synth[d.osc].velocity = *(float *)&d.data;
        synth[d.osc].status = AUDIBLE;
        // take care of fm & ks first -- no special treatment for bp/mod
        if(synth[d.osc].wave==KS) { 
            #if AMY_KS_OSCS > 0
            ks_note_on(d.osc); 
            #endif
        }  else {
            // an osc came in with a note on.
            // start the bp clock
            synth[d.osc].note_on_clock = total_samples; //esp_timer_get_time() / 1000;

            // if there was a filter active for this voice, reset it
            if(synth[d.osc].filter_type != FILTER_NONE) update_filter(d.osc);
            // restart the waveforms, adjusting for phase if given
            osc_note_on(d.osc);
            // trigger the mod source, if we have one
            if(AMY_IS_SET(synth[d.osc].mod_source)) {
                if(synth[synth[d.osc].mod_source].wave==SINE) sine_mod_trigger(synth[d.osc].mod_source);
                if(synth[synth[d.osc].mod_source].wave==SAW_DOWN) saw_up_mod_trigger(synth[d.osc].mod_source);
                if(synth[synth[d.osc].mod_source].wave==SAW_UP) saw_down_mod_trigger(synth[d.osc].mod_source);
                if(synth[synth[d.osc].mod_source].wave==TRIANGLE) triangle_mod_trigger(synth[d.osc].mod_source);
                if(synth[synth[d.osc].mod_source].wave==PULSE) pulse_mod_trigger(synth[d.osc].mod_source);
                if(synth[synth[d.osc].mod_source].wave==PCM) pcm_mod_trigger(synth[d.osc].mod_source);
            }

        }
    } else if(synth[d.osc].velocity > 0 && d.param == VELOCITY && *(float *)&d.data == 0) { // new note off
        synth[d.osc].velocity = 0;
        if(synth[d.osc].wave==KS) { 
            #if AMY_KS_OSCS > 0
            ks_note_off(d.osc);
            #endif 
        }
        else if(synth[d.osc].wave==ALGO) { algo_note_off(d.osc); } 
        else if(synth[d.osc].wave==PARTIAL) { 
            #if AMY_HAS_PARTIALS == 1
            partial_note_off(d.osc); 
            #endif
        }
        else if(synth[d.osc].wave==PARTIALS) { 
            #if AMY_HAS_PARTIALS == 1
            partials_note_off(d.osc); 
            #endif
        }
        else if(synth[d.osc].wave==PCM) { pcm_note_off(d.osc); }
        else {
            // osc note off, start release
            AMY_UNSET(synth[d.osc].note_on_clock);
            synth[d.osc].note_off_clock = total_samples;    
        }
    }

}

// apply an mod & bp, if any, to the osc
void hold_and_modify(uint16_t osc) {
    // copy all the modifier variables
    msynth[osc].amp = synth[osc].amp;
    msynth[osc].last_pan = msynth[osc].pan;
    msynth[osc].pan = synth[osc].pan;
    msynth[osc].duty = synth[osc].duty;
    //msynth[osc].freq = synth[osc].freq;
    msynth[osc].logfreq = synth[osc].logfreq;
    msynth[osc].feedback = synth[osc].feedback;
    //msynth[osc].filter_freq = synth[osc].filter_freq;
    msynth[osc].filter_logfreq = synth[osc].filter_logfreq;
    msynth[osc].resonance = synth[osc].resonance;

    // modify the synth params by scale -- bp scale is (original * scale)
    int amp_touched = false;
    for(uint8_t i=0;i<MAX_BREAKPOINT_SETS;i++) {
        if (synth[osc].breakpoint_target[i] & TARGET_AMP)  amp_touched = true;
        SAMPLE scale = compute_breakpoint_scale(osc, i);
        if (scale != F2S(1.0f)) {
            float fscale = S2F(scale);
            float logfscale = log2f(fscale);
            //printf("H&M: osc %d bpset %d fscale %f logfscale %f\n", osc, i, fscale, logfscale);
            //if (scale != F2S(1.0f)) printf("osc %d scale %f\n", osc, fscale);
            if(synth[osc].breakpoint_target[i] & TARGET_AMP) msynth[osc].amp *= fscale;
            if(synth[osc].breakpoint_target[i] & TARGET_PAN) msynth[osc].pan *= fscale;
            if(synth[osc].breakpoint_target[i] & TARGET_DUTY) msynth[osc].duty *= fscale;
            if(synth[osc].breakpoint_target[i] & TARGET_FREQ) msynth[osc].logfreq += logfscale;  // or logfscale
            if(synth[osc].breakpoint_target[i] & TARGET_FEEDBACK) msynth[osc].feedback *= fscale;
            if(synth[osc].breakpoint_target[i] & TARGET_FILTER_FREQ) msynth[osc].filter_logfreq += logfscale;  // or logfscale
            if(synth[osc].breakpoint_target[i] & TARGET_RESONANCE) msynth[osc].resonance *= fscale;
        }
    }
    // If nothing has altered the amp, we should apply the keyboard gate.
    if (!amp_touched && AMY_IS_SET(synth[osc].note_off_clock)) {
        // No amp envelope and the note is off -> initiate 1-frame ramp down.
        msynth[osc].amp = 0;
    }
    // Stop oscillators if amp is zero for two frames in a row.
    // Note: We can't wait for the note off because we need to turn off PARTIAL oscs when envelopes end, even if no note off.
    if(msynth[osc].amp == 0 && synth[osc].last_amp == 0) {
        synth[osc].status=OFF;
        AMY_UNSET(synth[osc].note_off_clock);
    }
    // and the mod -- mod scale is (original + (original * scale))
    SAMPLE scale = compute_mod_scale(osc);
    if (scale != 0) {
        float fscale = S2F(scale + F2S(1.0f));
        float logfscale = log2f(fscale);
        //printf("H&M: osc %d modscale fscale %f logfscale %f\n", osc, fscale, logfscale);
        if(synth[osc].mod_target & TARGET_AMP) msynth[osc].amp *= fscale;
        if(synth[osc].mod_target & TARGET_PAN) msynth[osc].pan *= fscale;
        if(synth[osc].mod_target & TARGET_DUTY) msynth[osc].duty *= fscale;
        if(synth[osc].mod_target & TARGET_FREQ) msynth[osc].logfreq += logfscale;  // or fscale
        if(synth[osc].mod_target & TARGET_FEEDBACK) msynth[osc].feedback *= fscale;
        if(synth[osc].mod_target & TARGET_FILTER_FREQ) msynth[osc].filter_logfreq += logfscale;  // or fscale
        if(synth[osc].mod_target & RESONANCE) msynth[osc].resonance *= fscale;

        //printf("h&m: osc %d bp_tgt0 %d bp_tgt1 %d mod_targ %d slf %f logfreq %f\n", osc,
        //       synth[osc].breakpoint_target[0], synth[osc].breakpoint_target[1], synth[osc].mod_target, 
        //       synth[osc].logfreq, msynth[osc].logfreq);
    }
}


static inline float lgain_of_pan(float pan) {
    if(pan > 1.f)  pan = 1.f;
    if(pan < 0)  pan = 0;
    return dsps_sqrtf_f32_ansi(1.f - pan);
}

static inline float rgain_of_pan(float pan) {
    if(pan > 1.f)  pan = 1.f;
    if(pan < 0)  pan = 0;
    return dsps_sqrtf_f32_ansi(pan);
}


void mix_with_pan(SAMPLE *stereo_dest, SAMPLE *mono_src, float pan_start, float pan_end) {
    /* copy a block_size of mono samples into an interleaved stereo buffer, applying pan */
#if AMY_NCHANS == 1
    // actually dest is mono, pan is ignored.
    for(uint16_t i=0;i<AMY_BLOCK_SIZE;i++) { stereo_dest[i] += mono_src[i]; }
#else
    // stereo 
    SAMPLE gain_l = F2S(lgain_of_pan(pan_start));
    SAMPLE gain_r = F2S(rgain_of_pan(pan_start));
    SAMPLE d_gain_l = F2S((lgain_of_pan(pan_end) - lgain_of_pan(pan_start)) / AMY_BLOCK_SIZE);
    SAMPLE d_gain_r = F2S((rgain_of_pan(pan_end) - rgain_of_pan(pan_start)) / AMY_BLOCK_SIZE);
    for(uint16_t i=0;i<AMY_BLOCK_SIZE;i++) {
        stereo_dest[i] += MUL4_SS(gain_l, mono_src[i]);
        stereo_dest[AMY_BLOCK_SIZE + i] += MUL4_SS(gain_r, mono_src[i]);
        gain_l += d_gain_l;
        gain_r += d_gain_r;
    }
#endif
}

void render_osc_wave(uint16_t osc, uint8_t core, SAMPLE* buf) {
    // fill buf with next block_size of samples for specified osc.
    for(uint16_t i=0;i<AMY_BLOCK_SIZE;i++) { buf[i] = 0; }
    hold_and_modify(osc); // apply bp / mod
    if(synth[osc].wave == NOISE) render_noise(buf, osc);
    if(synth[osc].wave == SAW_DOWN) render_saw_down(buf, osc);
    if(synth[osc].wave == SAW_UP) render_saw_up(buf, osc);
    if(synth[osc].wave == PULSE) render_pulse(buf, osc);
    if(synth[osc].wave == TRIANGLE) render_triangle(buf, osc);
    if(synth[osc].wave == SINE) render_sine(buf, osc);
    if(synth[osc].wave == KS) { 
        #if AMY_KS_OSCS > 0
        render_ks(buf, osc);
        #endif
    }
    if(synth[osc].wave == PCM) render_pcm(buf, osc);
    if(synth[osc].wave == ALGO) render_algo(buf, osc, core);
    #if AMY_HAS_PARTIALS == 1
    if(synth[osc].wave == PARTIAL) render_partial(buf, osc);
    if(synth[osc].wave == PARTIALS) render_partials(buf, osc);
    #endif
}

void render_task(uint16_t start, uint16_t end, uint8_t core) {
    for(uint16_t i=0;i<AMY_BLOCK_SIZE*AMY_NCHANS;i++) { fbl[core][i] = 0; }
    for(uint16_t osc=start; osc<end; osc++) {
        if(synth[osc].status==AUDIBLE) { // skip oscs that are silent or mod sources from playback
            render_osc_wave(osc, core, per_osc_fb[core]);
            // check it's not off, just in case. todo, why do i care?
            if(synth[osc].wave != OFF) {
                // apply filter to osc if set
                if(synth[osc].filter_type != FILTER_NONE) filter_process(per_osc_fb[core], osc);
                mix_with_pan(fbl[core], per_osc_fb[core], msynth[osc].last_pan, msynth[osc].pan);
                //printf("render5 %d %d %d %d\n", osc, start, end, core);

            }
        }
    }
    // apply the eq filters if set
    if(global.eq[0] != F2S(1.0f) || global.eq[1] != F2S(1.0f) || global.eq[2] != F2S(1.0f)) {
        parametric_eq_process(fbl[core]);
    }
}

// on all platforms, sysclock is based on total samples played, using audio out (i2s or etc) as system clock
uint32_t amy_sysclock() {
    return (uint32_t)((total_samples / (float)AMY_SAMPLE_RATE) * 1000);
}


void amy_increase_volume() {
    global.volume += 0.5f;
    if(global.volume > MAX_VOLUME) global.volume = MAX_VOLUME;    
}

void amy_decrease_volume() {
    global.volume -= 0.5f;
    if(global.volume < 0) global.volume = 0;    
}

// this takes scheduled events and plays them at the right time
void amy_prepare_buffer() {
    // check to see which sounds to play 
    uint32_t sysclock = amy_sysclock(); 

#if defined ESP_PLATFORM && !defined ARDUINO
    // put a mutex around this so that the event parser doesn't touch these while i'm running  
    xSemaphoreTake(xQueueSemaphore, portMAX_DELAY);
#endif

    // find any events that need to be played from the (in-order) queue
    while(sysclock >= global.event_start->time) {
        play_event(*global.event_start);
        global.event_start->time = UINT32_MAX;
        global.event_qsize--;
        global.event_start = global.event_start->next;
    }

#if defined ESP_PLATFORM && !defined ARDUINO
    // give the mutex back
    xSemaphoreGive(xQueueSemaphore);
#endif

#if AMY_HAS_CHORUS == 1
    // here's a little fragment of hold_and_modify() for you.
    msynth[CHORUS_MOD_SOURCE].amp = synth[CHORUS_MOD_SOURCE].amp;
    msynth[CHORUS_MOD_SOURCE].duty = synth[CHORUS_MOD_SOURCE].duty;
    //msynth[CHORUS_MOD_SOURCE].freq = synth[CHORUS_MOD_SOURCE].freq;
    msynth[CHORUS_MOD_SOURCE].logfreq = synth[CHORUS_MOD_SOURCE].logfreq;
#ifdef CHORUS_ARATE
    if(delay_mod)  render_osc_wave(CHORUS_MOD_SOURCE, 0 /* core */, delay_mod);
#else
    delay_mod_val = compute_mod_value(CHORUS_MOD_SOURCE);
#endif // CHORUS_ARATE
#endif // AMY_HAS_CHORUS

}

// This is a (for now) legacy call that we'll move over to a new style asap
int16_t * fill_audio_buffer_task() {
    // First, prepare the bffer
    amy_prepare_buffer();

#if defined ESP_PLATFORM && !defined ARDUINO
    // Tell the rendering threads to start rendering
    xTaskNotifyGive(amy_render_handle[0]);
    if(AMY_CORES == 2) xTaskNotifyGive(amy_render_handle[1]);

    // and wait for each of them to come back
    ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
    if(AMY_CORES == 2) ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
#elif defined PICO_ON_DEVICE
    if(AMY_CORES == 2) {
        // Tell renderer2 it's ok to render
        multicore_fifo_push_blocking(32);
        // Do renderer1
        render_task(0, AMY_OSCS/2, 0);

        // and wait for other core to finish
        int32_t ret = 0;
        while (!multicore_fifo_rvalid());
        ret = multicore_fifo_pop_blocking();
    } else {
        render_task(0, AMY_OSCS/2, 0);
    }

#else
    // todo -- there's no reason we can't multicore render on other platforms
    render_task(0, AMY_OSCS, 0);        
#endif

    return amy_fill_buffer();
}

int16_t * amy_fill_buffer() {
    // mix results from both cores.
#if AMY_CORES == 2
    for (int16_t i=0; i < AMY_BLOCK_SIZE * AMY_NCHANS; ++i)  fbl[0][i] += fbl[1][i];
#endif
#if  AMY_HAS_CHORUS == 1
    // apply chorus.
    if(chorus.level > 0 && delay_lines[0] != NULL) {
        // apply time-varying delays to both chans.
        // delay_mod_val, the modulated delay amount, is set up before calling render_*.
        SAMPLE scale = F2S(1.0f);
        for (int16_t c=0; c < AMY_NCHANS; ++c) {
#ifdef CHORUS_ARATE
            apply_variable_delay(fbl[0] + c * AMY_BLOCK_SIZE, delay_lines[c],
                                 delay_mod, scale, chorus.level, 0);
#else
            apply_fixed_delay(fbl[0] + c * AMY_BLOCK_SIZE, delay_lines[c],
                              MUL4_SS(scale, delay_mod_val), chorus.level);
#endif // CHORUS_ARATE
            // flip delay direction for alternating channels.
            scale = -scale;
        }
    }
#endif

#if (AMY_HAS_REVERB == 1)
    // apply reverb.
    if(reverb.level > 0) {
#if AMY_NCHANS == 1
        stereo_reverb(fbl[0], NULL, fbl[0], NULL, AMY_BLOCK_SIZE, reverb.level);
#else
        stereo_reverb(fbl[0], fbl[0] + AMY_BLOCK_SIZE, fbl[0], fbl[0] + AMY_BLOCK_SIZE, AMY_BLOCK_SIZE, reverb.level);
#endif
    }
#endif

    // global volume is supposed to max out at 10, so scale by 0.1.
    SAMPLE volume_scale = F2S(0.1f * global.volume);
    for(int16_t i=0; i < AMY_BLOCK_SIZE; ++i) {
        for (int16_t c=0; c < AMY_NCHANS; ++c) {

            // Convert the mixed sample into the int16 range, applying overall gain.
            SAMPLE fsample = MUL4_SS(volume_scale, fbl[0][i + c * AMY_BLOCK_SIZE]);

            // One-pole high-pass filter to remove large low-frequency excursions from
            // some FM patches. b = [1 -1]; a = [1 -0.995]
            //SAMPLE new_state = fsample + MUL8_SS(F2S(0.995f), global.hpf_state);  // MUL8 is critical here.
#ifdef HPF_OUTPUT
            SAMPLE new_state = fsample + global.hpf_state - SHIFTR(global.hpf_state, 8);  // i.e. 0.9961*hpf_state
            fsample = new_state - global.hpf_state;
            global.hpf_state = new_state;
#endif
            
            // soft clipping.
            int positive = 1; 
            if (fsample < 0) positive = 0;

            int32_t uintval;
            if (positive) {  // avoid fabs()
                uintval = S2L(fsample);
            } else {
                uintval = S2L(-fsample);
            }
            if (uintval >= FIRST_NONLIN) {
                if (uintval >= FIRST_HARDCLIP) {
                    uintval = SAMPLE_MAX;
                } else {
                    uintval = clipping_lookup_table[uintval - FIRST_NONLIN];
                }
            }
            int16_t sample;
            // For some reason, have to drop a bit to stop hard wrapping on esp?
#ifdef ESP_PLATFORM
            uintval >>= 1;
#endif
            if (positive) {
              sample = uintval;
            } else {
              sample = -uintval;
            }
#if AMY_NCHANS == 1
  #ifdef ESP_PLATFORM
            // esp32's i2s driver has this bug
            block[i ^ 0x01] = sample;
  #else
            block[i] = sample;
  #endif
#else // stereo

            block[(AMY_NCHANS * i) + c] = sample;
#endif
        }
    }

    total_samples += AMY_BLOCK_SIZE;
    return block;
}

uint32_t ms_to_samples(uint32_t ms) {
    return (uint32_t)(((float)ms / 1000.0) * (float)AMY_SAMPLE_RATE);
} 

float atoff(const char *s) {
    // Returns float value corresponding to parseable prefix of s.
    // Unlike atof(), it does not recognize scientific format ('e' or 'E')
    // and will stop parsing there.  Needed for message strings that contain
    // 'e' as a command prefix.
    float frac = 0;
    // Skip leading spaces.
    while (*s == ' ') ++s;
    float whole = (float)atoi(s);
    int is_negative = (s[0] == '-');  // Can't use (whole < 0) because of "-0.xx".
    //const char *s_in = s;  // for debug message.
    s += strspn(s, "-0123456789");
    if (*s == '.') {
        // Float with a decimal part.
        // Step over dp
        ++s;
        // Extract fractional part.
        int fraclen = strspn(s, "0123456789");
        char fracpart[8];
        // atoi() will overflow for values larger than 2^31, so only decode a prefix.
        if (fraclen > 6) {
            for(int i = 0; i < 7; ++i) {
                fracpart[i] = s[i];
            }
            fracpart[7] = '\0';
            s = fracpart;
            fraclen = 7;
        }
        frac = (float)atoi(s);
        frac /= powf(10.f, (float)fraclen);
        if (is_negative) frac = -frac;
    }
    //fprintf(stderr, "input was %s output is %f + %f = %f\n", s_in, whole, frac, whole+frac);
    return whole + frac;
}

int parse_float_list_message(char *message, float *vals, int max_num_vals) {
    // Return the number of values extracted from message.
    uint16_t c = 0;
    uint16_t stop = strspn(message, " 0123456789-,.");  // Note space & period.
    int num_vals_received = 0;
    while(c < stop && num_vals_received < max_num_vals) {
        if(message[c] != ',') {
            *vals++ = atoff(message + c);
            ++num_vals_received;
        }
        while(message[c] != ',' && message[c] != 0 && c < MAX_MESSAGE_LEN) c++;
        c++;
    }
    return num_vals_received;
}

int parse_int_list_message(char *message, int16_t *vals, int max_num_vals) {
    // Return the number of values extracted from message.
    uint16_t c = 0;
    uint16_t stop = strspn(message, " 0123456789-,");  // Space, no period.
    int num_vals_received = 0;
    while(c < stop && num_vals_received < max_num_vals) {
        if(message[c] != ',') {
            *vals++ = atoi(message + c);
            ++num_vals_received;
        }
        while(message[c] != ',' && message[c] != 0 && c < MAX_MESSAGE_LEN) c++;
        c++;
    }
    return num_vals_received;
}

// helper to parse the list of source voices for an algorithm
void parse_algorithm_source(struct synthinfo * e, char *message) {
    parse_int_list_message(message, e->algo_source, MAX_ALGO_OPS);
}

// helper to parse the special bp string
void parse_breakpoint(struct synthinfo * e, char* message, uint8_t which_bpset) {
    uint8_t idx = 0;
    uint16_t c = 0;
    // set the breakpoint to default first
    for(uint8_t i=0;i<MAX_BREAKPOINTS;i++) {
        AMY_UNSET(e->breakpoint_times[which_bpset][i]);
        AMY_UNSET(e->breakpoint_values[which_bpset][i]);
    }
    uint16_t stop = strspn(message, " 0123456789-,.");
    while(c < stop) {
        if(message[c]!=',') {
            if(idx % 2 == 0) {
                e->breakpoint_times[which_bpset][idx/2] = ms_to_samples(atoi(message+c));
            } else {
                e->breakpoint_values[which_bpset][(idx-1) / 2] = atoff(message+c);
            }
        }
        while(message[c]!=',' && message[c]!=0 && c < MAX_MESSAGE_LEN) c++;
        c++; idx++;
    }
}

// given a string return an event
struct event amy_parse_message(char * message) {
    uint8_t mode = 0;
    uint16_t osc = 0;
    uint16_t start = 0;
    uint16_t c = 0;
    int16_t length = strlen(message);
    struct event e = amy_default_event();
    uint32_t sysclock = amy_sysclock();
    
    // cut the osc cruft max etc add, they put a 0 and then more things after the 0
    int new_length = length; 
    for(int d=0;d<length;d++) {
        if(message[d] == 0) { new_length = d; d = length + 1;  } 
    }
    length = new_length;
    //fprintf(stderr, "%s\n", message);

    while(c < length+1) {
        uint8_t b = message[c];
        //if(b == '_' && c==0) sync_response = 1;
        if( ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')) || b == 0) {  // new mode or end
            if(mode=='t') {
                e.time=atol(message + start);
                // if we have set latency AND haven't yet synced our times, do it now
                if(global.latency_ms != 0) {
                    if(!computed_delta_set) {
                        computed_delta = e.time - sysclock;
                        //fprintf(stderr,"setting computed delta to %lld (e.time is %lld sysclock %lld) max_drift_ms %d latency %d\n", computed_delta, e.time, sysclock, AMY_MAX_DRIFT_MS, global.latency_ms);
                        computed_delta_set = 1;
                    }
                }
            } else {
                if(mode >= 'A' && mode <= 'z') {
                    switch(mode) {
                        case 'a': e.amp=atoff(message+start); break;
                        case 'A': strcpy(e.bp0, message+start); break;
                        case 'B': strcpy(e.bp1, message+start); break;
                        case 'b': e.feedback=atoff(message+start); break; 
                        case 'C': strcpy(e.bp2, message+start); break; 
                        case 'd': e.duty=atoff(message + start); break; 
                        case 'D': show_debug(atoi(message + start)); break; 
                        case 'f': e.freq = atoff(message + start); break; 
                        case 'F': e.filter_freq = atoff(message + start); break; 
                        case 'G': e.filter_type = atoi(message + start); break; 
                        case 'g': e.mod_target = atoi(message + start);  break; 
                        #if(AMY_HAS_REVERB == 1)
                        case 'H': config_reverb(S2F(reverb.level), atoff(message + start), reverb.damping, reverb.xover_hz); break;
                        case 'h': config_reverb(atoff(message + start), reverb.liveness, reverb.damping, reverb.xover_hz); break;
                        #endif
                        case 'I': e.ratio = atoff(message + start); break;
                        #if(AMY_HAS_REVERB == 1)
                        case 'j': config_reverb(S2F(reverb.level), reverb.liveness, atoff(message + start), reverb.xover_hz); break;
                        case 'J': config_reverb(S2F(reverb.level), reverb.liveness, reverb.damping, atoff(message + start)); break;
                        #endif
                        #if(AMY_HAS_CHORUS == 1)
                        case 'k': config_chorus(atoff(message + start), chorus.max_delay); break;
                        #endif
                        case 'l': e.velocity=atoff(message + start); break; 
                        case 'L': e.mod_source=atoi(message + start); break; 
                        #if (AMY_HAS_CHORUS == 1)
                        case 'm': config_chorus(S2F(chorus.level), atoi(message + start)); break;
                        #endif
                        case 'N': e.latency_ms = atoi(message + start);  break; 
                        case 'n': e.midi_note=atoi(message + start); break; 
                        case 'o': e.algorithm=atoi(message+start); break; 
                        case 'O': strcpy(e.algo_source, message+start); break; 
                        case 'p': e.patch=atoi(message + start); break; 
                        case 'P': e.phase=F2P(atoff(message + start)); break; 
                        case 'Q': e.pan = atoff(message + start); break;
                        case 'R': e.resonance=atoff(message + start); break; 
                        case 'S': osc = atoi(message + start); if(osc > AMY_OSCS-1) { amy_reset_oscs(); } else { reset_osc(osc); } break; 
                        case 'T': e.bp0_target = atoi(message + start);  break; 
                        case 'W': e.bp1_target = atoi(message + start);  break; 
                        case 'v': e.osc=(atoi(message + start) % AMY_OSCS);  break; // allow osc wraparound
                        case 'V': e.volume = atoff(message + start); break; 
                        case 'X': e.bp2_target = atoi(message + start); break; 
                        case 'w': e.wave=atoi(message + start); break; 
                        case 'x': e.eq_l = atoff(message+start); break; 
                        case 'y': e.eq_m = atoff(message+start); break; 
                        case 'z': e.eq_h = atoff(message+start); break; 
                        default:
                            break;
                            // If a parse callback function is declared, call it to see if there's something else to parse
                            //(*amy_parse_callback)(mode, message+start);
                            //break;
                    }
                }
            }
            mode=b;
            start=c+1;
        }
        c++;
    }

    // Only do this if we got some data
    if(length >0) {
        // TODO -- should time adjustment happen during parsing or playback?

        // Now adjust time in some useful way:
        // if we have a delta OR latency is 0 , AND got a time in this message, use it schedule it properly
        if(( (computed_delta_set || global.latency_ms==0) && e.time > 0)) {
            // OK, so check for potentially negative numbers here (or really big numbers-sysclock) 
            int32_t potential_time = (int32_t)((int32_t)e.time - (int32_t)computed_delta) + global.latency_ms;
            if(potential_time < 0 || (potential_time > (int32_t)(sysclock + global.latency_ms + AMY_MAX_DRIFT_MS))) {
                //fprintf(stderr,"recomputing time base: message came in with %lld, mine is %lld, computed delta was %lld\n", e.time, sysclock, computed_delta);
                computed_delta = e.time - sysclock;
                //fprintf(stderr,"computed delta now %lld\n", computed_delta);
            }
            e.time = (e.time - computed_delta) + global.latency_ms;
        } else { // else play it asap 
            e.time = sysclock + global.latency_ms;
        }
        e.status = SCHEDULED;
        return e;
    }
    return amy_default_event();
}

// given a string play / schedule the event directly 
void amy_play_message(char *message) {
    struct event e = amy_parse_message(message);
    if(e.status == SCHEDULED) {
        amy_add_event(e);
    }
}
// amy_play_message -> amy_parse_message -> amy_add_i_event -> add_delta_to_queue -> i_events queue -> global event queue
//                         amy_add_event /

// fill_audio_buffer_task -> read delta global event queue -> play_event -> apply delta to synth[d.osc]


void amy_stop() {
    oscs_deinit();
}

void amy_start() {
    global_init();
    oscs_init();
    amy_reset_oscs();
}

