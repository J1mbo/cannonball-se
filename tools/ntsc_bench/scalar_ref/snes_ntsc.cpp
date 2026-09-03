/* Based on snes_ntsc 0.2.2. http://www.slack.net/~ant/

Updates for CannonBall-SE are Copyright (c) 2025 James Pearce:
- S16 emulation at 24-bit output depth
- threaded initialisation
Updates Sep-25:  added scalar fall-back for Pi1 and PiZero
*/
#include <stdio.h>

#include "snes_ntsc.h"
#ifdef _OPENMP
#include <omp.h>    // JJP - OpenMP support for multi-threading initialization
#endif
#include <stdint.h> // uint32_t etc


/* Copyright (C) 2006-2007 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

snes_ntsc_setup_t const snes_ntsc_monochrome = { 0,-1, 0, 0,.2,  0,.2,-.2,-.2,-1,  1, 0, 0 };
snes_ntsc_setup_t const snes_ntsc_composite = { 0, 0, 0, 0, 0,  0, 0,  0,  0, 0,  1, 0, 0 };
snes_ntsc_setup_t const snes_ntsc_svideo = { 0, 0, 0, 0,.2,  0,.2, -1, -1, 0,  1, 0, 0 };
snes_ntsc_setup_t const snes_ntsc_rgb = { 0, 0, 0, 0,.2,  0,.7, -1, -1,-1,  1, 0, 0 };

#define alignment_count 3
#define burst_count     3
#define rescale_in      8
#define rescale_out     7

#define artifacts_mid   1.0f
#define fringing_mid    1.0f
#define std_decoder_hue 0

#define rgb_bits        7   /* half normal range to allow for doubled hires pixels */
#define gamma_size      256 // JJP - 8-bit range of input values for S16 as adjusted for DACs

#include "snes_ntsc_impl.h"

/* 3 input pixels -> 8 composite samples */
pixel_info_t const snes_ntsc_pixels[alignment_count] = {
    { PIXEL_OFFSET(-4, -9), { 1, 1, .6667f, 0 } },
    { PIXEL_OFFSET(-2, -7), {       .3333f, 1, 1, .3333f } },
    { PIXEL_OFFSET(0, -5), {                  0, .6667f, 1, 1 } },
};

const unsigned int S16_rgbVals[64] = {
    // standard levels
    0,8,16,24,31,39,47,55,62,70,78,86,94,102,109,117,125,133,140,148,156,164,171,179,187,195,203,211,
    218,226,234,242,
    // shadow levels
    0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100,105,110,115,120,126,130,136,140,146,
    150,156
};

static void merge_kernel_fields(snes_ntsc_rgb_t* io)
{
    int n;
    for (n = burst_size; n; --n)
    {
        snes_ntsc_rgb_t p0 = io[burst_size * 0] + rgb_bias;
        snes_ntsc_rgb_t p1 = io[burst_size * 1] + rgb_bias;
        snes_ntsc_rgb_t p2 = io[burst_size * 2] + rgb_bias;
        /* merge colors without losing precision */
        io[burst_size * 0] =
            ((p0 + p1 - ((p0 ^ p1) & snes_ntsc_rgb_builder)) >> 1) - rgb_bias;
        io[burst_size * 1] =
            ((p1 + p2 - ((p1 ^ p2) & snes_ntsc_rgb_builder)) >> 1) - rgb_bias;
        io[burst_size * 2] =
            ((p2 + p0 - ((p2 ^ p0) & snes_ntsc_rgb_builder)) >> 1) - rgb_bias;
        ++io;
    }
}

static void correct_errors(snes_ntsc_rgb_t color, snes_ntsc_rgb_t* out)
{
    int n;
    for (n = burst_count; n; --n)
    {
        unsigned i;
        for (i = 0; i < rgb_kernel_size / 2; i++)
        {
            snes_ntsc_rgb_t error = color -
                out[i] - out[(i + 12) % 14 + 14] - out[(i + 10) % 14 + 28] -
                out[i + 7] - out[i + 5 + 14] - out[i + 3 + 28];
            DISTRIBUTE_ERROR(i + 3 + 28, i + 5 + 14, i + 7);
        }
        out += alignment_count * rgb_kernel_size;
    }
}


void snes_ntsc_init(snes_ntsc_t* ntsc, snes_ntsc_setup_t const* setup)
{
    int merge_fields;
    init_t impl;
    if (!setup)
        setup = &snes_ntsc_composite;
    init(&impl, setup);

    merge_fields = setup->merge_fields;
    if (setup->artifacts <= -1 && setup->fringing <= -1)
        merge_fields = 1;

#ifdef _OPENMP
#pragma omp parallel
    {
        init_t impl_local = impl; // or use firstprivate(impl)
        int merge_fields_local = merge_fields;
        int entry;

#pragma omp for schedule(static)
        for (entry = 0; entry < snes_ntsc_palette_size; ++entry) {
            unsigned int red_index = (entry >> 10 & 0x1F) + ((entry >> 15) * 32);
            unsigned int green_index = (entry >> 5 & 0x1F) + ((entry >> 15) * 32);
            unsigned int blue_index = (entry >> 0 & 0x1F) + ((entry >> 15) * 32);

            unsigned int ir = S16_rgbVals[red_index];
            unsigned int ig = S16_rgbVals[green_index];
            unsigned int ib = S16_rgbVals[blue_index];

            float rr = impl_local.to_float[ir];
            float gg = impl_local.to_float[ig];
            float bb = impl_local.to_float[ib];

            float y, i, q = RGB_TO_YIQ(rr, gg, bb, y, i);
            int r, g, b = YIQ_TO_RGB(y, i, q, impl_local.to_rgb, int, r, g);
            snes_ntsc_rgb_t rgb = PACK_RGB(r, g, b);

            snes_ntsc_rgb_t* out = ntsc->table[entry];
            gen_kernel(&impl_local, y, i, q, out);
            if (merge_fields_local) merge_kernel_fields(out);
            correct_errors(rgb, out);
        }
    }
    #else
        for (unsigned int entry = 0; entry < snes_ntsc_palette_size; entry++) {
            unsigned int red_index   = (entry >> 10 & 0x1F) + ((entry >> 15) * 32);
            unsigned int green_index = (entry >> 5  & 0x1F) + ((entry >> 15) * 32);
            unsigned int blue_index  = (entry >> 0  & 0x1F) + ((entry >> 15) * 32);
            unsigned int ir = S16_rgbVals[red_index];
            unsigned int ig = S16_rgbVals[green_index];
            unsigned int ib = S16_rgbVals[blue_index];
            float rr = impl.to_float[ir];
            float gg = impl.to_float[ig];
            float bb = impl.to_float[ib];
            float y, i, q = RGB_TO_YIQ(rr, gg, bb, y, i);
            int r, g, b = YIQ_TO_RGB(y, i, q, impl.to_rgb, int, r, g);
            snes_ntsc_rgb_t rgb = PACK_RGB(r, g, b);
            snes_ntsc_rgb_t* out = ntsc->table[entry];
            gen_kernel(&impl, y, i, q, out);
            if (merge_fields) merge_kernel_fields(out);
            correct_errors(rgb, out);
        }
    #endif
}

#ifndef SNES_NTSC_NO_BLITTERS

void snes_ntsc_blit(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* input, long in_row_width,
    int burst_phase, int in_width, int in_height, void* rgb_out, long out_pitch, long Alevel)
{
    int chunk_count = (in_width - 1) / snes_ntsc_in_chunk;
    for (; in_height; --in_height)
    {
        SNES_NTSC_IN_T const* line_in = input;
        SNES_NTSC_BEGIN_ROW(ntsc, burst_phase,
            snes_ntsc_black, snes_ntsc_black, SNES_NTSC_ADJ_IN(*line_in));
        snes_ntsc_out_t* restrict line_out = (snes_ntsc_out_t*)rgb_out;
        int n;
        ++line_in;

        for (n = chunk_count; n; --n)
        {
            /* order of input and output pixels must not be altered */
            SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(line_in[0]));
            SNES_NTSC_RGB_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);
            SNES_NTSC_RGB_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(line_in[1]));
            SNES_NTSC_RGB_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);
            SNES_NTSC_RGB_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(line_in[2]));
            SNES_NTSC_RGB_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);
            SNES_NTSC_RGB_OUT(5, line_out[5], SNES_NTSC_OUT_DEPTH, Alevel);
            SNES_NTSC_RGB_OUT(6, line_out[6], SNES_NTSC_OUT_DEPTH, Alevel);

            line_in += 3;
            line_out += 7;
        }

        /* finish final pixels */
        SNES_NTSC_COLOR_IN(0, snes_ntsc_black);
        SNES_NTSC_RGB_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(1, snes_ntsc_black);
        SNES_NTSC_RGB_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(2, snes_ntsc_black);
        SNES_NTSC_RGB_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(5, line_out[5], SNES_NTSC_OUT_DEPTH, Alevel);
        SNES_NTSC_RGB_OUT(6, line_out[6], SNES_NTSC_OUT_DEPTH, Alevel);

        burst_phase = (burst_phase + 1) % snes_ntsc_burst_count;
        input += in_row_width;
        rgb_out = (char*)rgb_out + out_pitch;
    }
}




void snes_ntsc_blit_hires(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* input, long in_row_width,
    int burst_phase, int in_width, int in_height, void* rgb_out, long out_pitch, long Alevel)
{
    int chunk_count = (in_width - 2) / (snes_ntsc_in_chunk * 2);
    for (; in_height; --in_height)
    {
        SNES_NTSC_IN_T const* line_in = input;
        SNES_NTSC_HIRES_ROW(ntsc, burst_phase,
            snes_ntsc_black, snes_ntsc_black, snes_ntsc_black,
            SNES_NTSC_ADJ_IN(line_in[0]),
            SNES_NTSC_ADJ_IN(line_in[1]));
        snes_ntsc_out_t* restrict line_out = (snes_ntsc_out_t*)rgb_out;
        int n;
        line_in += 2;

        for (n = chunk_count; n; --n)
        {
            // twice as many input pixels per chunk, compared to lo-res version
            SNES_NTSC_COLOR_IN(0, SNES_NTSC_ADJ_IN(line_in[0]));
            SNES_NTSC_HIRES_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(1, SNES_NTSC_ADJ_IN(line_in[1]));
            SNES_NTSC_HIRES_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(2, SNES_NTSC_ADJ_IN(line_in[2]));
            SNES_NTSC_HIRES_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(3, SNES_NTSC_ADJ_IN(line_in[3]));
            SNES_NTSC_HIRES_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(4, SNES_NTSC_ADJ_IN(line_in[4]));
            SNES_NTSC_HIRES_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_COLOR_IN(5, SNES_NTSC_ADJ_IN(line_in[5]));
            SNES_NTSC_HIRES_OUT(5, line_out[5], SNES_NTSC_OUT_DEPTH, Alevel);

            SNES_NTSC_HIRES_OUT(6, line_out[6], SNES_NTSC_OUT_DEPTH, Alevel);

            line_in += 6;
            line_out += 7;
        }

        SNES_NTSC_COLOR_IN(0, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(0, line_out[0], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(1, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(1, line_out[1], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(2, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(2, line_out[2], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(3, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(3, line_out[3], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(4, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(4, line_out[4], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_COLOR_IN(5, snes_ntsc_black);
        SNES_NTSC_HIRES_OUT(5, line_out[5], SNES_NTSC_OUT_DEPTH, Alevel);

        SNES_NTSC_HIRES_OUT(6, line_out[6], SNES_NTSC_OUT_DEPTH, Alevel);

        burst_phase = (burst_phase + 1) % snes_ntsc_burst_count;
        input += in_row_width;
        rgb_out = (char*)rgb_out + out_pitch;
    }
}

/*
* The following table attempts to depicts how the data is gathered and summed, showing
* the third and fourth iterations of the loop.
* 
* The RGB table for the colour at Line_in[0] is loaded on the first row at the first position
* The second row has one value from the previous iteration, and then loads Line_in[1] at the second
* The third row likewise, but from the second phase offset so table 2.14 rather than 2.0
* Likewise rows 5 & 6 third phase so 2.28.
* 
* The lower table stores the 'afterglow' impact of the first, so values 2.07-2.13 are processed
* In the following pass.
* 
* Values are then summed vertically
*

Phase       Line_in Pixel index within each loop (chunk_count)          Pixel (next iteration)
Offset		[0] 	[1]     [2]     [3]     [4]     [5]     [6] 		[0] 	[1]     [2]     [3]     [4]     [5]     [6]
0.00		2.00	2.01	2.02	2.03	2.04	2.05	2.06		3.00	3.01	3.02	3.03	3.04	3.05	3.06
0.00		1.06	2.00	2.01	2.02	2.03	2.04	2.05		2.06	3.00	3.01	3.02	3.03	3.04	3.05
0.14		1.19	1.20	2.14	2.15	2.16	2.17	2.18		2.19	2.20	3.14	3.15	3.16	3.17	3.18
0.14		1.18	1.19	1.20	2.14	2.15	2.16	2.17		2.18	2.19	2.20	3.14	3.15	3.16	3.17
0.28		1.31	1.32	1.33	1.34	2.28	2.29	2.30		2.31	2.32	2.33	2.34	3.28	3.29	3.30
0.28		1.30	1.31	1.32	1.33	1.34	2.28	2.29		2.30	2.31	2.32	2.33	2.34	3.28	3.29

0.00		1.07	1.08	1.09	1.10	1.11	1.12	1.13		2.07	2.08	2.09	2.10	2.11	2.12	2.13
0.00		0.13	1.07	1.08	1.09	1.10	1.11	1.12		1.13	2.07	2.08	2.09	2.10	2.11	2.12
0.14		0.26	0.27	1.21	1.22	1.23	1.24	1.25		1.26	1.27	2.21	2.22	2.23	2.24	2.25
0.14		0.25	0.26	0.27	1.21	1.22	1.23	1.24		1.25	1.26	1.27	2.21	2.22	2.23	2.24
0.28		0.38	0.39	0.40	0.41	1.35	1.36	1.37		1.38	1.39	1.40	1.41	2.35	2.36	2.37
0.28		0.37	0.38	0.39	0.40	0.41	1.35	1.36		1.37	1.38	1.39	1.40	1.41	2.35	2.36

*/

#endif
