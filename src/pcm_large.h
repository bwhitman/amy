// Automatically generated by amy_headers.generate_pcm_header()
#ifndef __PCM_H
#define __PCM_H
#include "pcm_samples_large.h"
__attribute__((weak)) const uint16_t pcm_samples=67;
#define PCM_LENGTH 1176036
#define PCM_AMY_SAMPLE_RATE 22050
__attribute__((weak)) const pcm_map_t pcm_map[67] PROGMEM = {
    /* [0] 0 */ {0, 707, 342, 684, 89}, /* 808-MARACA-D */
    /* [1] 3 */ {707, 8186, 4282, 7439, 39}, /* 808-KIK 4-D */
    /* [2] 8 */ {8893, 2766, 1377, 2744, 45}, /* 808-SNR 4-D */
    /* [3] 11 */ {11659, 1311, 898, 1288, 52}, /* 808-SNR 7-D */
    /* [4] 14 */ {12970, 2276, 1164, 2254, 51}, /* 808-SNR 10D */
    /* [5] 16 */ {15246, 2872, 1430, 2849, 41}, /* 808-SNR 12-D */
    /* [6] 17 */ {18118, 1751, 888, 1728, 53}, /* 808-C-HAT1-D */
    /* [7] 18 */ {19869, 15400, 7815, 15377, 56}, /* 808-O-HAT1-D */
    /* [8] 20 */ {35269, 8995, 4588, 8973, 61}, /* 808-LTOM M-D */
    /* [9] 23 */ {44264, 3027, 1504, 3004, 94}, /* 808-DRYCLP-D */
    /* [10] 25 */ {47291, 3762, 22, 1871, 69}, /* 808-CWBELL-D */
    /* [11] 26 */ {51053, 7486, 3802, 7463, 74}, /* 808-CNGLO2-D */
    /* [12] 29 */ {58539, 731, 354, 709, 82}, /* 808-CLAVE -D */
    /* [13] 30 */ {59270, 12292, 4, 12288, 60}, /* Temple Block 3-mf */
    /* [14] 31 */ {71562, 39920, 7584, 39894, 60}, /* MTnr-3 roll P */
    /* [15] 32 */ {111482, 26940, 0, 26940, 60}, /* MTnr-3 hit 4 */
    /* [16] 37 */ {138422, 22050, 4, 22046, 60}, /* 58_79_CRASHCHOKE1 */
    /* [17] 39 */ {160472, 10907, 4, 10903, 60}, /* MSnr-shell 2 */
    /* [18] 40 */ {171379, 23018, 18835, 23010, 52}, /* Cello E4 */
    /* [19] 42 */ {194397, 24710, 20304, 24687, 54}, /* Cello-F#2 */
    /* [20] 47 */ {219107, 60406, 60004, 60361, 59}, /* Steel Gtr B4 */
    /* [21] 49 */ {279513, 36400, 35590, 36387, 59}, /* Clean F50 */
    /* [22] 50 */ {315913, 40325, 21144, 40229, 66}, /* SynthVz F#5 */
    /* [23] 52 */ {356238, 31284, 31092, 31271, 59}, /* Nylon B4 */
    /* [24] 58 */ {387522, 11562, 5, 11559, 60}, /* Windchimes */
    /* [25] 63 */ {399084, 2061, 2005, 2052, 70}, /* Clarinet A#5 */
    /* [26] 69 */ {401145, 12076, 11250, 12060, 45}, /* Synth Bass 2-B1 */
    /* [27] 74 */ {413221, 16170, 16042, 16136, 62}, /* DX7 Strike 3 */
    /* [28] 76 */ {429391, 8288, 7852, 8253, 76}, /* harmnc_e4 */
    /* [29] 80 */ {437679, 39910, 32126, 39733, 72}, /* dulci_c4 */
    /* [30] 83 */ {477589, 35643, 32, 35612, 60}, /* Sea Shore */
    /* [31] 85 */ {513232, 28055, 4, 28051, 60}, /* Power Snare 1 */
    /* [32] 86 */ {541287, 22136, 4, 22132, 60}, /* Tom Hi */
    /* [33] 95 */ {563423, 27680, 22401, 27648, 74}, /* Flute D5 */
    /* [34] 96 */ {591103, 16772, 11265, 16768, 70}, /* Piccolo A#6 */
    /* [35] 99 */ {607875, 26144, 21995, 26119, 77}, /* Violin F6 */
    /* [36] 100 */ {634019, 33544, 28879, 33538, 70}, /* Viola A#3 */
    /* [37] 101 */ {667563, 17271, 8272, 17248, 72}, /* Pan Flute C6 */
    /* [38] 107 */ {684834, 4502, 3933, 4500, 39}, /* Tuba D#4 */
    /* [39] 108 */ {689336, 19458, 15109, 19456, 70}, /* Oboe A#4 */
    /* [40] 109 */ {708794, 42836, 38954, 42781, 60}, /* English Horn C5 */
    /* [41] 112 */ {751630, 16124, 15953, 16101, 50}, /* PalmMuted Gtr D4 */
    /* [42] 116 */ {767754, 4068, 3739, 4037, 66}, /* Shamisen F#5 */
    /* [43] 117 */ {771822, 7418, 7138, 7377, 66}, /* Koto F#5 */
    /* [44] 118 */ {779240, 9512, 5, 9507, 72}, /* Steel Drum C6 */
    /* [45] 120 */ {788752, 17536, 4, 17532, 60}, /* Power Kick 3 */
    /* [46] 127 */ {806288, 37049, 4, 37045, 66}, /* Marimba F#5 */
    /* [47] 130 */ {843337, 5834, 4, 5830, 60}, /* Fretnoise */
    /* [48] 134 */ {849171, 9612, 4, 9608, 60}, /* Piccolo 1-1 */
    /* [49] 136 */ {858783, 8144, 4, 8140, 60}, /* Std Kick 3 */
    /* [50] 145 */ {866927, 32064, 31525, 32030, 60}, /* Rhodes C5 */
    /* [51] 149 */ {898991, 23296, 4, 23292, 78}, /* Xylophone F#6 */
    /* [52] 155 */ {922287, 39120, 8110, 39116, 63}, /* B3 F5 */
    /* [53] 161 */ {961407, 6856, 4, 6852, 60}, /* Elec Kick2 */
    /* [54] 165 */ {968263, 7168, 4, 7164, 60}, /* Low Woodblock */
    /* [55] 166 */ {975431, 8448, 4, 8444, 60}, /* Clap */
    /* [56] 170 */ {983879, 6197, 4, 6193, 60}, /* High Agogo */
    /* [57] 171 */ {990076, 6688, 4, 6684, 60}, /* High Woodblock */
    /* [58] 175 */ {996764, 56406, 28702, 56398, 60}, /* Ahh Choir C5-L */
    /* [59] 177 */ {1053170, 22384, 21973, 22352, 46}, /* Acoustic Bass A#3 */
    /* [60] 178 */ {1075554, 31000, 4, 30996, 60}, /* Bird */
    /* [61] 183 */ {1106554, 12463, 11894, 12141, 45}, /* Harmonics 2 */
    /* [62] 192 */ {1119017, 7501, 7000, 7500, 69}, /* Kalimba A5 */
    /* [63] 197 */ {1126518, 18546, 4, 18542, 68}, /* Orch Hit G#6 */
    /* [64] 198 */ {1145064, 9498, 9280, 9492, 63}, /* Harp D#4 */
    /* [65] 200 */ {1154562, 9862, 4, 9858, 60}, /* Metronome Bell(L) */
    /* [66] 204 */ {1164424, 11612, 7135, 11601, 72}, /* Trumpet C4 */
};

#endif  // __PCM_H
