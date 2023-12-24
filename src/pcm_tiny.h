// Automatically generated by amy_headers.generate_pcm_header()
#ifndef __PCM_H
#define __PCM_H
#include "pcm_samples_tiny.h"
#define PCM_SAMPLES 11
#define PCM_LENGTH 51053
#define PCM_AMY_SAMPLE_RATE 22050
const pcm_map_t pcm_map[11] PROGMEM = {
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
};

#endif  // __PCM_H
