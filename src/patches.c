// patches.c
// baked in AMY string patches (Juno-6 & DX7 for now)

#include "amy.h"
#include "patches.h"


/* patch spec

amy.send(load_patch=1, voices="0,1,2,3") # load juno patch into 4 voices. internally figure out which oscs to use and update our internal map of oscs <-> voices
amy.send(load_patch=130, voices="4,5") # load dx7 patch into 2 voices.
amy.send(voices="0,1", vel=1, note=40) # send this note on to voices 0 and 1
amy.send(voices="2",vel=1, note=44) # different note on differnet voice
amy.send(voices="4", vel=1, note=50) # dx7 note on
amy.send(voices="4", osc=1, filter_freq="440,0,0,0,5") -- can address an osc offset if you give both

so basically, a voice is a lookup to a base_osc
if you get multiple in a message, you call the message N times across the base_oscs , that includes load_patch 
if you get a osc and a voice, you add the osc to the base_osc lookup and send the message 

*/

uint8_t osc_to_voice[AMY_OSCS];
uint16_t voice_to_base_osc[MAX_VOICE];


void patches_reset() {
    for(uint8_t v=0;v<MAX_VOICE;v++) {
        AMY_UNSET(voice_to_base_osc[v]);
    }
    for(uint16_t i=0;i<AMY_OSCS;i++) {
        AMY_UNSET(osc_to_voice[i]);
    }
}

/*
assume we have voices as deltas 
synthinfo can have a voice array in it, even if mostly set to osc 0 (we can optimize this out later)

when a voice delta comes in, it has to send the contents of the _event_ (not synth osc?) to mulitple oscs. that's whhat event_has_voices does
but where do i get that event from? it's torn up and reconstituted

so ok, hold on 
at add_event time, if there's a voice, it calls patches_event_has_voices() , which adds time to the thing. this all works
But the problem is we don't yet know which voice_to_base_osc will be valid at the time of the actual event 
so if we made voices part of the ... .... ... well, deltas are associated with oscs, even 0 
so if we made a voice param for snythinfo ... 
deltas come in for note_on for voices=0,1, note=60, vel=1 . osc = 0 because they didn't specify

so when play_event sees voices=0,1 come in, it sends note=60,vel=1,osc=0 to the base_oscs of v=0 and 1
but how? maybe we add the delta for voices FIRST, like we send the delta for vel LAST

so when play_event runs, it'll get voices FIRST if set
and we can set some flag in synthinfo saying "this is a voices osc" (like, the existence of a set voices synthinfo array)
and then for every other play_event for that osc it sends it to ... the voice_to_base_osc[voice] per voice
i could recurisvely call play_event with modified d.osc to do that

ok, so how do we set/unset voice_to_base_osc then ??

when we get a load_patch DELTA we will have the voices list already in synthinfo , and have a patch number
so i can just call this patches_assign_voices() 

and then call patches_load_patch per voice.


so this guy won't be used anymore...

// This is called when i get an event with voices in it, BUT NOT with a load_patch 
// This can happen at amy_add_event , because the time is set relative 
void patches_event_has_voices(struct event e) {
    int16_t voices[MAX_VOICES];
    uint8_t num_voices = parse_int_list_message(e.voices, voices, MAX_VOICES);
    // clear out the voices and patch now from the event. If we didn't, we'd keep calling this over and over
    e.voices[0] = 0;
    AMY_UNSET(e.load_patch);
    // for each voice, send the event to the base osc (+ e.osc if given!)
    for(uint8_t i=0;i<num_voices;i++) {
        if(AMY_IS_SET(voice_to_base_osc[i])) {
            uint16_t target_osc = voice_to_base_osc[voices[i]] + e.osc;
            amy_add_event_internal(e, target_osc);
        }
    }
}

this guy we can refactor to just take in patch # and voice list.... (why do i need voice list)??

// This can happen at amy_add_event , because the time is set relative 
void patches_load_patch(struct event e) {
    char sub_message[255];
    int16_t voices[MAX_VOICES];
    uint8_t num_voices = parse_int_list_message(e.voices, voices, MAX_VOICES);
    // We're good. Load the patch into this voice. 
    // This just loads the patch into the oscillators we found for the patch / voice
    uint16_t start = 0;
    for(uint16_t i=0;i<strlen(message);i++) {
        if(message[i] == 'Z') {
            strncpy(sub_message, message + start, i - start + 1);
            sub_message[i-start+1]= 0;
            struct event patch_event = amy_parse_message(sub_message);
            // Override time from the patch (usually/always not set) with the event time
            patch_event.time = e.time;
            if(patch_event.status == SCHEDULED) {
                amy_add_event_internal(patch_event, voice_to_base_osc[voices[v]]);
            }
        start = i+1;
    }
}

*/


void patches_load_patch(uint16_t patch, uint8_t voice) {
    char sub_message[255];
    // This just loads the patch into the oscillators we found for the patch / voice
    uint16_t start = 0;
    char*message = (char*)patch_commands[patch];
    for(uint16_t i=0;i<strlen(message);i++) {
        if(message[i] == 'Z') {
            strncpy(sub_message, message + start, i - start + 1);
            sub_message[i-start+1]= 0;
            struct event patch_event = amy_parse_message(sub_message);
            if(patch_event.status == SCHEDULED) {
                amy_add_event_internal(patch_event, voice_to_base_osc[voice]);
            }
            start = i+1;
        }
    }
}


// Don't do this until the time is here
// this has to come in from play_event
void patches_assign_voices(uint8_t * voices, uint8_t num_voices, uint16_t patch) {
    // We have to be smart about voice # and allocating oscs 
    for(uint8_t v=0;v<num_voices;v++) {
        // First, is this an old voice we're re-doing? If so, delete the pointers to the voice
        if(AMY_IS_SET(voice_to_base_osc[voices[v]])) {
            fprintf(stderr, "Already set voice %d, removing it\n", voices[v]);
            // Remove the oscs for this old voice
            for(uint16_t i=0;i<AMY_OSCS;i++) {
                if(osc_to_voice[i]==voices[v]) { 
                    fprintf(stderr, "Already set voice %d osc %d, removing it\n", voices[v], i);
                    AMY_UNSET(osc_to_voice[i]);
                }
            }
            // Remove the base osc setting for this voice
            AMY_UNSET(voice_to_base_osc[voices[v]]);
        }
        // Now find some oscs for this new voice. They have to be consecutive. 
        uint8_t good = 0;
        for(uint16_t i=0;i<AMY_OSCS;i++) {
            if(AMY_IS_UNSET(osc_to_voice[i])) {
                // Are there num_voices patch_oscs free oscs after this one?
                good = 1;
                for(uint16_t j=0;j<patch_oscs[patch];j++) {
                    good = good & AMY_IS_UNSET(osc_to_voice[i+j]);
                }
                if(good) {
                    fprintf(stderr, "found %d consecutive oscs starting at %d for voice %d\n", patch_oscs[patch], i, voices[v]);
                    fprintf(stderr, "setting base osc for voice %d to %d\n", voices[v], i);
                    voice_to_base_osc[voices[v]] = i; 
                    for(uint16_t j=0;j<patch_oscs[patch];j++) {
                        fprintf(stderr, "setting osc %d for voice %d to amy osc %d\n", j, voices[v], i+j);
                        osc_to_voice[i+j] = voices[v];
                    }
                    // exit the loop
                    i = AMY_OSCS + 1;
                }
            }
        }
        if(!good) {
            fprintf(stderr, "we are out of oscs for voice %d. not setting this voice\n", voices[v]);
        }
    }
}

