// Standalone unit test for S1 (visible-window tilemap scan) in hwtiles.cpp.
//
// Verifies that the new two-stage precompute+iterate scan (render_tile_layer /
// render_text_layer in src/main/hwvideo/hwtiles.cpp) visits exactly the same
// (mx,my) cells, in exactly the same order, as the original nested-loop scan
// with its in-body reject test -- for every swept combination of scroll
// offsets and widescreen configuration.
//
// No game headers are used; the two variants of each layer's scan are
// reproduced here as free functions from the source text of hwtiles.cpp.
//
// Build: g++ -O2 -o s1_scan_test s1_scan_test.cpp
// Run:   ./s1_scan_test

#include <cstdio>
#include <cstdint>

// S16_HEIGHT is a fixed constant of the real hardware video mode (224 lines);
// it is not swept, matching IMPLEMENTATION_SPEC.md S1.4.
static const int S16_HEIGHT = 224;

struct Pair { int16_t mx, my; };

// ---------------------------------------------------------------------
// Tile layer (64 rows x 128 cols) -- render_tile_layer, hwtiles.cpp ~212-309
// ---------------------------------------------------------------------

// ORIGINAL: full nested loop (my outer, mx inner), in-body reject test is the
// union of the fast-path test (hwtiles.cpp ~302) and the clip-path test
// (~304), with the same wrap adjustments (x_clamp / 1024, -288 / 512) as the
// real code. Appends accepted (mx,my) to out[] in visitation order.
static int tile_scan_original(int x_decrement, int y_decrement, int x_clamp, int W, Pair* out)
{
    int n = 0;
    for (int my = 0; my < 64; my++)
    {
        int y = my * 8;
        y -= y_decrement;
        if (y < -288) y += 512;

        for (int mx = 0; mx < 128; mx++)
        {
            int x = mx * 8;
            x -= x_decrement;
            if (x < -x_clamp) x += 1024;

            bool fast = (x > 7 && x < (W - 8) && y > 7 && y <= (S16_HEIGHT - 8));
            bool clip = (x > -8 && x < W && y > -8 && y < S16_HEIGHT);
            if (fast || clip)
            {
                out[n].mx = (int16_t)mx;
                out[n].my = (int16_t)my;
                n++;
            }
        }
    }
    return n;
}

// NEW: two-stage precompute + iterate, mirrors the updated render_tile_layer.
static int tile_scan_new(int x_decrement, int y_decrement, int x_clamp, int W, Pair* out)
{
    int xs[128]; uint8_t mxl[128]; int nmx = 0;
    for (int mx = 0; mx < 128; mx++) {
        int x = mx * 8 - x_decrement;
        if (x < -x_clamp) x += 1024;
        if (x > -8 && x < W) { xs[nmx] = x; mxl[nmx++] = (uint8_t)mx; }
    }
    int ys[64]; uint8_t myl[64]; int nmy = 0;
    for (int my = 0; my < 64; my++) {
        int y = my * 8 - y_decrement;
        if (y < -288) y += 512;
        if (y > -8 && y < S16_HEIGHT) { ys[nmy] = y; myl[nmy++] = (uint8_t)my; }
    }

    int n = 0;
    for (int j = 0; j < nmy; j++)
    {
        int my = myl[j];
        for (int i = 0; i < nmx; i++)
        {
            int mx = mxl[i];
            out[n].mx = (int16_t)mx;
            out[n].my = (int16_t)my;
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------
// Text layer (32 rows x 64 cols) -- render_text_layer, hwtiles.cpp ~311-354
// ---------------------------------------------------------------------

// ORIGINAL: full nested loop (my outer, mx inner), in-body reject test is the
// union of the fast-path test (~345) and the clip-path test (~347). x and y
// are modelled as uint16_t, exactly as in the real code (x = 8*mx; x -= 192;
// with the resulting unsigned wraparound for small mx), so the wrap behaviour
// is reproduced faithfully rather than assumed.
static int text_scan_original(int W, Pair* out)
{
    int n = 0;
    for (int my = 0; my < 32; my++)
    {
        uint16_t y = (uint16_t)(my * 8);

        for (int mx = 0; mx < 64; mx++)
        {
            uint16_t x = (uint16_t)(mx * 8);
            x = (uint16_t)(x - 192);

            bool fast = (x > 7 && x < (W - 8) && y > 7 && y <= (S16_HEIGHT - 8));
            bool clip = (x > -8 && x < W && y >= 0 && y < S16_HEIGHT);
            if (fast || clip)
            {
                out[n].mx = (int16_t)mx;
                out[n].my = (int16_t)my;
                n++;
            }
        }
    }
    return n;
}

// NEW: two-stage precompute + iterate, mirrors the updated render_text_layer.
static int text_scan_new(int W, Pair* out)
{
    int xs[64]; uint8_t mxl[64]; int nmx = 0;
    for (int mx = 0; mx < 64; mx++) {
        int x = 8 * mx - 192;
        if (x > -8 && x < W) { xs[nmx] = x; mxl[nmx++] = (uint8_t)mx; }
    }
    int ys[32]; uint8_t myl[32]; int nmy = 0;
    for (int my = 0; my < 32; my++) {
        int y = 8 * my;
        if (y >= 0 && y < S16_HEIGHT) { ys[nmy] = y; myl[nmy++] = (uint8_t)my; }
    }

    int n = 0;
    for (int j = 0; j < nmy; j++)
    {
        int my = myl[j];
        for (int i = 0; i < nmx; i++)
        {
            int mx = mxl[i];
            out[n].mx = (int16_t)mx;
            out[n].my = (int16_t)my;
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------
// Tile layer, per-axis acceptance in isolation.
//
// render_tile_layer's mx-loop (x_decrement/x_clamp/W) and my-loop
// (y_decrement) are, in both the original and the new code, entirely
// independent of each other -- neither loop's precompute reads a variable
// the other one writes. So cell (mx,my) is accepted iff mx is independently
// accepted (holding y fixed at any point deep inside the fast interior) AND
// my is independently accepted (holding x fixed at any point deep inside the
// fast interior). That means a full exhaustive check of every mx-affecting
// parameter combination, and separately of every my-affecting one, is an
// exhaustive check of every (x_decrement, y_decrement, x_clamp, W) 4-tuple's
// visitation set without materialising all 3.1M full 128x64 grids (which is
// what makes the sweep in main() run in a few seconds rather than tens of
// seconds under -O2). The full-grid functions above are additionally spot
// checked directly against a large strided sample in main(), to guard
// against any unexpected coupling between the two loops.

// mx accepted by the literal fast||clip formula, y held at 120 (>7 and
// <= S16_HEIGHT-8, so both the fast and clip y-parts are always true here).
static int tile_mx_accept_original(int x_decrement, int x_clamp, int W, uint8_t* out)
{
    const int y = 120;
    int n = 0;
    for (int mx = 0; mx < 128; mx++)
    {
        int x = mx * 8;
        x -= x_decrement;
        if (x < -x_clamp) x += 1024;

        bool fast = (x > 7 && x < (W - 8) && y > 7 && y <= (S16_HEIGHT - 8));
        bool clip = (x > -8 && x < W && y > -8 && y < S16_HEIGHT);
        if (fast || clip) out[n++] = (uint8_t)mx;
    }
    return n;
}

static int tile_mx_accept_new(int x_decrement, int x_clamp, int W, uint8_t* out)
{
    int n = 0;
    for (int mx = 0; mx < 128; mx++)
    {
        int x = mx * 8 - x_decrement;
        if (x < -x_clamp) x += 1024;
        if (x > -8 && x < W) out[n++] = (uint8_t)mx;
    }
    return n;
}

// my accepted by the literal fast||clip formula, x held at 120 (well inside
// the fast interior for both swept widths, 320 and 404, and unaffected by
// x_clamp since it is positive and small x_clamp values never wrap it).
static int tile_my_accept_original(int y_decrement, uint8_t* out)
{
    const int x = 120;
    const int W = 404;
    int n = 0;
    for (int my = 0; my < 64; my++)
    {
        int y = my * 8;
        y -= y_decrement;
        if (y < -288) y += 512;

        bool fast = (x > 7 && x < (W - 8) && y > 7 && y <= (S16_HEIGHT - 8));
        bool clip = (x > -8 && x < W && y > -8 && y < S16_HEIGHT);
        if (fast || clip) out[n++] = (uint8_t)my;
    }
    return n;
}

static int tile_my_accept_new(int y_decrement, uint8_t* out)
{
    int n = 0;
    for (int my = 0; my < 64; my++)
    {
        int y = my * 8 - y_decrement;
        if (y < -288) y += 512;
        if (y > -8 && y < S16_HEIGHT) out[n++] = (uint8_t)my;
    }
    return n;
}

// ---------------------------------------------------------------------

static bool sequences_equal(const Pair* a, int na, const Pair* b, int nb, int& diff_index)
{
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++)
    {
        if (a[i].mx != b[i].mx || a[i].my != b[i].my) { diff_index = i; return false; }
    }
    if (na != nb) { diff_index = n; return false; }
    return true;
}

static bool bytes_equal(const uint8_t* a, int na, const uint8_t* b, int nb, int& diff_index)
{
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++)
    {
        if (a[i] != b[i]) { diff_index = i; return false; }
    }
    if (na != nb) { diff_index = n; return false; }
    return true;
}

int main()
{
    static Pair origBuf[128 * 64];
    static Pair newBuf[128 * 64];
    static uint8_t bufA[128], bufB[128];

    const int x_clamps[3] = { 192, 150, 108 };
    const int widths[2]   = { 320, 404 };

    // ---- Exhaustive per-axis check: every x_decrement x x_clamp x W
    // (1024 x 3 x 2 = 6144 cases) and every y_decrement (512 cases). ----
    for (int xc = 0; xc < 3; xc++)
    {
        int x_clamp = x_clamps[xc];
        for (int wi = 0; wi < 2; wi++)
        {
            int W = widths[wi];
            for (int x_decrement = 0; x_decrement < 1024; x_decrement++)
            {
                int na = tile_mx_accept_original(x_decrement, x_clamp, W, bufA);
                int nb = tile_mx_accept_new(x_decrement, x_clamp, W, bufB);
                int diff_index;
                if (!bytes_equal(bufA, na, bufB, nb, diff_index))
                {
                    printf("TILE MX MISMATCH: x_decrement=%d x_clamp=%d W=%d diff_index=%d (na=%d nb=%d)\n",
                           x_decrement, x_clamp, W, diff_index, na, nb);
                    return 1;
                }
            }
        }
    }
    for (int y_decrement = 0; y_decrement < 512; y_decrement++)
    {
        int na = tile_my_accept_original(y_decrement, bufA);
        int nb = tile_my_accept_new(y_decrement, bufB);
        int diff_index;
        if (!bytes_equal(bufA, na, bufB, nb, diff_index))
        {
            printf("TILE MY MISMATCH: y_decrement=%d diff_index=%d (na=%d nb=%d)\n",
                   y_decrement, diff_index, na, nb);
            return 1;
        }
    }

    // ---- Strided full-grid spot check: run the two literal 128x64 scans
    // (tile_scan_original / tile_scan_new) directly against each other over
    // a large sample spanning the full 3.1M-case space, to confirm the two
    // loops really are independent as assumed above. ----
    for (int xc = 0; xc < 3; xc++)
    {
        int x_clamp = x_clamps[xc];
        for (int wi = 0; wi < 2; wi++)
        {
            int W = widths[wi];
            for (int x_decrement = 0; x_decrement < 1024; x_decrement += 16)
            {
                for (int y_decrement = 0; y_decrement < 512; y_decrement += 8)
                {
                    int no = tile_scan_original(x_decrement, y_decrement, x_clamp, W, origBuf);
                    int nn = tile_scan_new(x_decrement, y_decrement, x_clamp, W, newBuf);
                    int diff_index;
                    if (!sequences_equal(origBuf, no, newBuf, nn, diff_index))
                    {
                        printf("TILE MISMATCH: x_decrement=%d y_decrement=%d x_clamp=%d W=%d diff_index=%d\n",
                               x_decrement, y_decrement, x_clamp, W, diff_index);
                        printf("  orig count=%d new count=%d\n", no, nn);
                        return 1;
                    }
                }
            }
        }
    }

    // ---- Text layer sweep: full grid, W in {320, 404} (trivially cheap:
    // 32 x 64 cells x 2 widths). ----
    for (int wi = 0; wi < 2; wi++)
    {
        int W = widths[wi];
        int no = text_scan_original(W, origBuf);
        int nn = text_scan_new(W, newBuf);
        int diff_index;
        if (!sequences_equal(origBuf, no, newBuf, nn, diff_index))
        {
            printf("TEXT MISMATCH: W=%d diff_index=%d\n", W, diff_index);
            printf("  orig count=%d new count=%d\n", no, nn);
            return 1;
        }
    }

    printf("S1 SCAN TEST PASS\n");
    return 0;
}
