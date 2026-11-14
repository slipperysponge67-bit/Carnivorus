/*
 * Carnivorus - a UCI chess engine written from scratch in C.
 *
 * Single-threaded, hand-crafted evaluation, no NNUE.
 *  - bitboards with magic-bitboard slider attacks (magics found at startup)
 *  - copy-make board, pseudo-legal generation + legality check
 *  - negamax / PVS, iterative deepening, aspiration windows
 *  - quiescence search with SEE pruning
 *  - Zobrist transposition table
 *  - null move, reverse futility, futility, late move pruning/reductions
 *  - killer, history and countermove ordering
 *  - tapered evaluation: material, PST, mobility, pawn structure,
 *    passed pawns, rooks, king safety
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t U64;
typedef int Move;

enum { WHITE, BLACK };
enum { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
#define EMPTY 12

#define BIT(s) (1ULL << (s))
#define FILEA 0x0101010101010101ULL
#define FILEH 0x8080808080808080ULL
#define MAXPLY 128
#define MAXMOVES 256
#define INF 32000
#define MATE 31000
#define MATE_BOUND (MATE - MAXPLY)
#define NOEVAL (-32001)

static inline int lsb(U64 b) { return __builtin_ctzll(b); }
static inline int popc(U64 b) { return __builtin_popcountll(b); }
static inline int pop_lsb(U64 *b) {
    int s = lsb(*b);
    *b &= *b - 1;
    return s;
}

/* ---------- move encoding ---------- */
#define F_CAP (1 << 15)
#define F_EP (1 << 16)
#define F_CASTLE (1 << 17)
#define F_DP (1 << 18)
#define MFROM(m) ((m) & 63)
#define MTO(m) (((m) >> 6) & 63)
#define MPROMO(m) (((m) >> 12) & 7)
#define MKMOVE(f, t, pr, fl) ((f) | ((t) << 6) | ((pr) << 12) | (fl))

typedef struct {
    U64 bb[12];
    U64 occ[2];
    U64 all;
    U64 key;
    uint8_t board[64];
    int side, castle, ep, halfmove, fullmove;
} Pos;

/* ---------- tables ---------- */
static U64 knight_att[64], king_att[64], pawn_att[2][64];
static U64 zp[12][64], zcastle[16], zep[8], zside;
static int cmask[64];
static U64 passed_mask[2][64], front_span[2][64], adj_files[8], file_mask[8];

typedef struct {
    U64 mask, magic;
    U64 *att;
    int shift;
} Magic;
static Magic RM[64], BM[64];
static U64 rpool[102400], bpool[5248];

static U64 rng_state = 0x9E3779B97F4A7C15ULL;
static U64 rnd(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ULL;
}

static U64 slide_att(int sq, U64 occ, const int *dirs) {
    U64 a = 0;
    int r0 = sq >> 3, f0 = sq & 7;
    for (int d = 0; d < 4; d++) {
        int dr = dirs[2 * d], df = dirs[2 * d + 1];
        for (int r = r0 + dr, f = f0 + df; r >= 0 && r < 8 && f >= 0 && f < 8; r += dr, f += df) {
            a |= BIT(r * 8 + f);
            if (occ & BIT(r * 8 + f)) break;
        }
    }
    return a;
}
static const int RDIRS[8] = {1, 0, -1, 0, 0, 1, 0, -1};
static const int BDIRS[8] = {1, 1, 1, -1, -1, 1, -1, -1};

static U64 relevant_mask(int sq, const int *dirs) {
    U64 m = 0;
    int r0 = sq >> 3, f0 = sq & 7;
    for (int d = 0; d < 4; d++) {
        int dr = dirs[2 * d], df = dirs[2 * d + 1];
        for (int r = r0 + dr, f = f0 + df; r + dr >= 0 && r + dr < 8 && f + df >= 0 && f + df < 8;
             r += dr, f += df)
            m |= BIT(r * 8 + f);
    }
    return m;
}

static void init_magic(int sq, Magic *m, U64 *pool, const int *dirs) {
    static U64 occ[4096], ref[4096];
    static int stamp[4096];
    static int epoch = 0;
    m->mask = relevant_mask(sq, dirs);
    int bits = popc(m->mask);
    int n = 1 << bits;
    m->shift = 64 - bits;
    m->att = pool;
    U64 b = 0;
    int i = 0;
    do {
        occ[i] = b;
        ref[i] = slide_att(sq, b, dirs);
        i++;
        b = (b - m->mask) & m->mask;
    } while (b);
    for (;;) {
        U64 magic = rnd() & rnd() & rnd();
        if (popc((m->mask * magic) >> 56) < 6) continue;
        int fail = 0;
        epoch++;
        for (i = 0; i < n; i++) {
            int idx = (int)((occ[i] * magic) >> m->shift);
            if (stamp[idx] != epoch) {
                stamp[idx] = epoch;
                m->att[idx] = ref[i];
            } else if (m->att[idx] != ref[i]) {
                fail = 1;
                break;
            }
        }
        if (!fail) {
            m->magic = magic;
            break;
        }
    }
}

static inline U64 rook_att(int sq, U64 occ) {
    const Magic *m = &RM[sq];
    return m->att[((occ & m->mask) * m->magic) >> m->shift];
}
static inline U64 bishop_att(int sq, U64 occ) {
    const Magic *m = &BM[sq];
    return m->att[((occ & m->mask) * m->magic) >> m->shift];
}

static void init_tables(void) {
    U64 *rp = rpool, *bp = bpool;
    for (int s = 0; s < 64; s++) {
        init_magic(s, &RM[s], rp, RDIRS);
        rp += 1 << popc(RM[s].mask);
        init_magic(s, &BM[s], bp, BDIRS);
        bp += 1 << popc(BM[s].mask);
    }
    static const int kn[16] = {1, 2, 2, 1, 2, -1, 1, -2, -1, -2, -2, -1, -2, 1, -1, 2};
    static const int kg[16] = {1, 0, -1, 0, 0, 1, 0, -1, 1, 1, 1, -1, -1, 1, -1, -1};
    for (int s = 0; s < 64; s++) {
        int r = s >> 3, f = s & 7;
        for (int d = 0; d < 8; d++) {
            int rr = r + kn[2 * d], ff = f + kn[2 * d + 1];
            if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) knight_att[s] |= BIT(rr * 8 + ff);
            rr = r + kg[2 * d];
            ff = f + kg[2 * d + 1];
            if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) king_att[s] |= BIT(rr * 8 + ff);
        }
        if (r < 7) {
            if (f > 0) pawn_att[WHITE][s] |= BIT(s + 7);
            if (f < 7) pawn_att[WHITE][s] |= BIT(s + 9);
        }
        if (r > 0) {
            if (f > 0) pawn_att[BLACK][s] |= BIT(s - 9);
            if (f < 7) pawn_att[BLACK][s] |= BIT(s - 7);
        }
    }
    for (int f = 0; f < 8; f++) {
        file_mask[f] = FILEA << f;
        adj_files[f] = (f > 0 ? FILEA << (f - 1) : 0) | (f < 7 ? FILEA << (f + 1) : 0);
    }
    for (int s = 0; s < 64; s++) {
        int r = s >> 3, f = s & 7;
        U64 wf = 0, bf = 0;
        for (int rr = r + 1; rr < 8; rr++) wf |= BIT(rr * 8 + f);
        for (int rr = r - 1; rr >= 0; rr--) bf |= BIT(rr * 8 + f);
        front_span[WHITE][s] = wf;
        front_span[BLACK][s] = bf;
        U64 wm = 0, bm = 0;
        for (int rr = r + 1; rr < 8; rr++)
            wm |= (file_mask[f] | adj_files[f]) & (0xFFULL << (rr * 8));
        for (int rr = r - 1; rr >= 0; rr--)
            bm |= (file_mask[f] | adj_files[f]) & (0xFFULL << (rr * 8));
        passed_mask[WHITE][s] = wm;
        passed_mask[BLACK][s] = bm;
    }
    for (int p = 0; p < 12; p++)
        for (int s = 0; s < 64; s++) zp[p][s] = rnd();
    for (int i = 0; i < 16; i++) zcastle[i] = rnd();
    for (int i = 0; i < 8; i++) zep[i] = rnd();
    zside = rnd();
    for (int s = 0; s < 64; s++) cmask[s] = 15;
    cmask[0] &= ~2;
    cmask[7] &= ~1;
    cmask[4] &= ~3;
    cmask[56] &= ~8;
    cmask[63] &= ~4;
    cmask[60] &= ~12;
}

/* ---------- board primitives ---------- */
static inline void put_piece(Pos *p, int pc, int sq) {
    U64 b = BIT(sq);
    p->bb[pc] |= b;
    p->occ[pc / 6] |= b;
    p->all |= b;
    p->board[sq] = (uint8_t)pc;
    p->key ^= zp[pc][sq];
}
static inline void remove_piece(Pos *p, int pc, int sq) {
    U64 b = BIT(sq);
    p->bb[pc] &= ~b;
    p->occ[pc / 6] &= ~b;
    p->all &= ~b;
    p->board[sq] = EMPTY;
    p->key ^= zp[pc][sq];
}

static inline int sq_attacked(const Pos *p, int sq, int by) {
    const U64 *b = p->bb + by * 6;
    if (pawn_att[by ^ 1][sq] & b[PAWN]) return 1;
    if (knight_att[sq] & b[KNIGHT]) return 1;
    if (king_att[sq] & b[KING]) return 1;
    if (bishop_att(sq, p->all) & (b[BISHOP] | b[QUEEN])) return 1;
    if (rook_att(sq, p->all) & (b[ROOK] | b[QUEEN])) return 1;
    return 0;
}
static inline int king_sq(const Pos *p, int c) { return lsb(p->bb[c * 6 + KING]); }
static inline int in_check(const Pos *p) { return sq_attacked(p, king_sq(p, p->side), p->side ^ 1); }

static U64 attackers_to(const Pos *p, int sq, U64 occ) {
    return (pawn_att[BLACK][sq] & p->bb[PAWN]) | (pawn_att[WHITE][sq] & p->bb[6 + PAWN]) |
           (knight_att[sq] & (p->bb[KNIGHT] | p->bb[6 + KNIGHT])) |
           (king_att[sq] & (p->bb[KING] | p->bb[6 + KING])) |
           (bishop_att(sq, occ) & (p->bb[BISHOP] | p->bb[QUEEN] | p->bb[6 + BISHOP] | p->bb[6 + QUEEN])) |
           (rook_att(sq, occ) & (p->bb[ROOK] | p->bb[QUEEN] | p->bb[6 + ROOK] | p->bb[6 + QUEEN]));
}

/* returns 0 if the move leaves own king in check (position then undefined) */
static int make_move(Pos *p, Move m) {
    int from = MFROM(m), to = MTO(m), us = p->side, them = us ^ 1;
    int pc = p->board[from];
    if (p->ep >= 0) p->key ^= zep[p->ep & 7];
    p->ep = -1;
    p->halfmove++;
    if (m & F_CAP) {
        int cs = (m & F_EP) ? to + (us == WHITE ? -8 : 8) : to;
        remove_piece(p, p->board[cs], cs);
        p->halfmove = 0;
    }
    remove_piece(p, pc, from);
    int pr = MPROMO(m);
    put_piece(p, pr ? us * 6 + pr : pc, to);
    if (pc % 6 == PAWN) {
        p->halfmove = 0;
        if (m & F_DP) {
            int eps = (from + to) / 2;
            if (pawn_att[us][eps] & p->bb[them * 6 + PAWN]) {
                p->ep = eps;
                p->key ^= zep[eps & 7];
            }
        }
    }
    if (m & F_CASTLE) {
        int base = us == WHITE ? 0 : 56;
        if (to == base + 6) {
            remove_piece(p, us * 6 + ROOK, base + 7);
            put_piece(p, us * 6 + ROOK, base + 5);
        } else {
            remove_piece(p, us * 6 + ROOK, base);
            put_piece(p, us * 6 + ROOK, base + 3);
        }
    }
    p->key ^= zcastle[p->castle];
    p->castle &= cmask[from] & cmask[to];
    p->key ^= zcastle[p->castle];
    if (sq_attacked(p, king_sq(p, us), them)) return 0;
    p->side = them;
    p->key ^= zside;
    if (us == BLACK) p->fullmove++;
    return 1;
}

static void make_null(Pos *p) {
    if (p->ep >= 0) p->key ^= zep[p->ep & 7];
    p->ep = -1;
    p->side ^= 1;
    p->key ^= zside;
    p->halfmove++;
}

/* ---------- move generation ---------- */
static inline Move *add_promos(Move *l, int from, int to, int fl, int caps_only) {
    *l++ = MKMOVE(from, to, QUEEN, fl);
    if (!caps_only) {
        *l++ = MKMOVE(from, to, KNIGHT, fl);
        *l++ = MKMOVE(from, to, ROOK, fl);
        *l++ = MKMOVE(from, to, BISHOP, fl);
    }
    return l;
}

static int gen_moves(const Pos *p, Move *list, int caps_only) {
    Move *l = list;
    int us = p->side, them = us ^ 1;
    U64 own = p->occ[us], enemy = p->occ[them], all = p->all;
    int up = us == WHITE ? 8 : -8;
    int promo_rank = us == WHITE ? 7 : 0, start_rank = us == WHITE ? 1 : 6;
    U64 pw = p->bb[us * 6 + PAWN];
    while (pw) {
        int from = pop_lsb(&pw);
        int to = from + up;
        int promo = (to >> 3) == promo_rank;
        if (!(all & BIT(to))) {
            if (promo)
                l = add_promos(l, from, to, 0, caps_only);
            else if (!caps_only) {
                *l++ = MKMOVE(from, to, 0, 0);
                if ((from >> 3) == start_rank && !(all & BIT(to + up)))
                    *l++ = MKMOVE(from, to + up, 0, F_DP);
            }
        }
        U64 att = pawn_att[us][from] & enemy;
        while (att) {
            int t = pop_lsb(&att);
            if (promo)
                l = add_promos(l, from, t, F_CAP, 0);
            else
                *l++ = MKMOVE(from, t, 0, F_CAP);
        }
        if (p->ep >= 0 && (pawn_att[us][from] & BIT(p->ep))) *l++ = MKMOVE(from, p->ep, 0, F_CAP | F_EP);
    }
    U64 targets = caps_only ? enemy : ~own;
    for (int t = KNIGHT; t <= KING; t++) {
        U64 b = p->bb[us * 6 + t];
        while (b) {
            int from = pop_lsb(&b);
            U64 a;
            switch (t) {
            case KNIGHT: a = knight_att[from]; break;
            case BISHOP: a = bishop_att(from, all); break;
            case ROOK: a = rook_att(from, all); break;
            case QUEEN: a = bishop_att(from, all) | rook_att(from, all); break;
            default: a = king_att[from]; break;
            }
            a &= targets;
            while (a) {
                int to = pop_lsb(&a);
                *l++ = MKMOVE(from, to, 0, (enemy & BIT(to)) ? F_CAP : 0);
            }
        }
    }
    if (!caps_only) {
        int base = us == WHITE ? 0 : 56;
        int kr = us == WHITE ? 1 : 4, qr = us == WHITE ? 2 : 8;
        if ((p->castle & (kr | qr)) && !sq_attacked(p, base + 4, them)) {
            if ((p->castle & kr) && !(all & (BIT(base + 5) | BIT(base + 6))) && !sq_attacked(p, base + 5, them))
                *l++ = MKMOVE(base + 4, base + 6, 0, F_CASTLE);
            if ((p->castle & qr) && !(all & (BIT(base + 1) | BIT(base + 2) | BIT(base + 3))) &&
                !sq_attacked(p, base + 3, them))
                *l++ = MKMOVE(base + 4, base + 2, 0, F_CASTLE);
        }
    }
    return (int)(l - list);
}

/* ---------- FEN / strings ---------- */
static const char *PCHARS = "PNBRQKpnbrqk";

static int set_fen(Pos *p, const char *fen) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < 64; i++) p->board[i] = EMPTY;
    p->ep = -1;
    int r = 7, f = 0;
    const char *s = fen;
    while (*s && *s != ' ') {
        if (*s == '/') {
            r--;
            f = 0;
        } else if (isdigit((unsigned char)*s)) {
            f += *s - '0';
        } else {
            const char *c = strchr(PCHARS, *s);
            if (!c || r < 0 || f > 7) return 0;
            put_piece(p, (int)(c - PCHARS), r * 8 + f);
            f++;
        }
        s++;
    }
    if (!p->bb[KING] || !p->bb[6 + KING]) return 0;
    while (*s == ' ') s++;
    p->side = (*s == 'b') ? BLACK : WHITE;
    if (p->side == BLACK) p->key ^= zside;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    while (*s && *s != ' ') {
        if (*s == 'K') p->castle |= 1;
        if (*s == 'Q') p->castle |= 2;
        if (*s == 'k') p->castle |= 4;
        if (*s == 'q') p->castle |= 8;
        s++;
    }
    p->key ^= zcastle[p->castle];
    while (*s == ' ') s++;
    if (*s && *s != '-' && s[1]) {
        int ep = (s[1] - '1') * 8 + (s[0] - 'a');
        if (ep >= 0 && ep < 64 && (pawn_att[p->side ^ 1][ep] & p->bb[p->side * 6 + PAWN])) {
            p->ep = ep;
            p->key ^= zep[ep & 7];
        }
    }
    while (*s && *s != ' ') s++;
    p->halfmove = atoi(s);
    while (*s == ' ') s++;
    while (*s && *s != ' ') s++;
    p->fullmove = atoi(s);
    if (p->fullmove < 1) p->fullmove = 1;
    return 1;
}

static void move_str(Move m, char *s) {
    s[0] = (char)('a' + (MFROM(m) & 7));
    s[1] = (char)('1' + (MFROM(m) >> 3));
    s[2] = (char)('a' + (MTO(m) & 7));
    s[3] = (char)('1' + (MTO(m) >> 3));
    int pr = MPROMO(m);
    if (pr) {
        s[4] = "?nbrq"[pr];
        s[5] = 0;
    } else
        s[4] = 0;
}

static Move parse_move(const Pos *p, const char *s) {
    Move list[MAXMOVES];
    int n = gen_moves(p, list, 0);
    for (int i = 0; i < n; i++) {
        char buf[8];
        move_str(list[i], buf);
        if (!strcmp(buf, s)) {
            Pos np = *p;
            if (make_move(&np, list[i])) return list[i];
        }
    }
    return 0;
}

/* ---------- perft ---------- */
static U64 perft(const Pos *p, int depth) {
    Move list[MAXMOVES];
    int n = gen_moves(p, list, 0);
    U64 total = 0;
    for (int i = 0; i < n; i++) {
        Pos np = *p;
        if (!make_move(&np, list[i])) continue;
        total += depth <= 1 ? 1 : perft(&np, depth - 1);
    }
    return total;
}

/* ---------- time ---------- */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- evaluation ---------- */
static const int VAL_MG[6] = {90, 325, 340, 500, 1000, 0};
static const int VAL_EG[6] = {110, 300, 330, 540, 960, 0};
static int pst_mg[6][64], pst_eg[6][64];

static void init_eval(void) {
    static const int pawn_r_mg[8] = {0, 0, 2, 7, 14, 26, 40, 0};
    static const int pawn_r_eg[8] = {0, -4, 0, 6, 16, 34, 60, 0};
    static const int pawn_f[8] = {-6, -2, 3, 8, 8, 3, -2, -6};
    static const int rook_f[8] = {-3, 0, 3, 6, 6, 3, 0, -3};
    for (int s = 0; s < 64; s++) {
        int r = s >> 3, f = s & 7;
        int fc = f > 3 ? f - 4 : 3 - f, rc = r > 3 ? r - 4 : 3 - r;
        int cen = 6 - (fc + rc);
        pst_mg[PAWN][s] = pawn_r_mg[r] + (r >= 1 && r <= 5 ? pawn_f[f] * (r >= 2 ? 1 : 2) / 2 : 0);
        pst_eg[PAWN][s] = pawn_r_eg[r] - (fc >= 2 ? 3 : 0);
        pst_mg[KNIGHT][s] = cen * 8 - 30 + (r == 0 ? -6 : 0);
        pst_eg[KNIGHT][s] = cen * 6 - 24;
        pst_mg[BISHOP][s] = cen * 3 - 9 + (r == 0 ? -5 : 0);
        pst_eg[BISHOP][s] = cen * 3 - 9;
        pst_mg[ROOK][s] = rook_f[f];
        pst_eg[ROOK][s] = 0;
        pst_mg[QUEEN][s] = cen * 2 - 6;
        pst_eg[QUEEN][s] = cen * 4 - 12;
        pst_mg[KING][s] = -cen * 6 - r * 12 + (r == 0 && (f <= 2 || f >= 6) ? 18 : 0);
        pst_eg[KING][s] = cen * 9 - 27;
    }
}

static const int PASS_MG[8] = {0, 4, 8, 16, 32, 56, 90, 0};
static const int PASS_EG[8] = {0, 10, 18, 36, 62, 105, 160, 0};
static const int SAFE_W[6] = {0, 2, 2, 3, 5, 0};

static inline int kdist(int a, int b) {
    int dr = abs((a >> 3) - (b >> 3)), df = abs((a & 7) - (b & 7));
    return dr > df ? dr : df;
}

static int eval(const Pos *p) {
    int mg[2] = {0, 0}, eg[2] = {0, 0}, npm[2] = {0, 0};
    int phase = 0;
    int katt_n[2] = {0, 0}, katt_u[2] = {0, 0};
    U64 pawns[2] = {p->bb[PAWN], p->bb[6 + PAWN]};
    U64 pa[2];
    pa[WHITE] = ((pawns[WHITE] << 7) & ~FILEH) | ((pawns[WHITE] << 9) & ~FILEA);
    pa[BLACK] = ((pawns[BLACK] >> 7) & ~FILEA) | ((pawns[BLACK] >> 9) & ~FILEH);
    U64 zone[2];
    for (int c = 0; c < 2; c++) {
        int ks = king_sq(p, c);
        zone[c] = king_att[ks] | BIT(ks);
        zone[c] |= c == WHITE ? (king_att[ks] << 8) : (king_att[ks] >> 8);
    }
    for (int c = 0; c < 2; c++) {
        int them = c ^ 1;
        U64 own = p->occ[c];
        U64 mob_ok = ~own & ~pa[them];
        int ksq = king_sq(p, c), eksq = king_sq(p, them);
        for (int t = PAWN; t <= KING; t++) {
            U64 b = p->bb[c * 6 + t];
            while (b) {
                int s = pop_lsb(&b);
                int ps = c == WHITE ? s : s ^ 56;
                mg[c] += VAL_MG[t] + pst_mg[t][ps];
                eg[c] += VAL_EG[t] + pst_eg[t][ps];
                if (t == PAWN) {
                    int f = s & 7, rr = c == WHITE ? s >> 3 : 7 - (s >> 3);
                    int supported = (pa[c] & BIT(s)) != 0;
                    if (!(pawns[c] & adj_files[f])) {
                        mg[c] -= 10;
                        eg[c] -= 14;
                    }
                    if (pawns[c] & front_span[c][s]) {
                        mg[c] -= 10;
                        eg[c] -= 18;
                    }
                    if (supported) {
                        mg[c] += 6 + rr;
                        eg[c] += 4 + rr;
                    }
                    if (!(passed_mask[c][s] & pawns[them]) && !(pawns[c] & front_span[c][s])) {
                        int bm = PASS_MG[rr], be = PASS_EG[rr];
                        int ahead = c == WHITE ? s + 8 : s - 8;
                        if (p->all & BIT(ahead)) {
                            bm /= 2;
                            be = be * 2 / 3;
                        }
                        if (supported) {
                            bm += rr * 2;
                            be += rr * 3;
                        }
                        be += rr * (kdist(eksq, ahead) - kdist(ksq, ahead)) * 2;
                        mg[c] += bm;
                        eg[c] += be;
                    }
                    continue;
                }
                if (t == KING) continue;
                U64 a;
                switch (t) {
                case KNIGHT: a = knight_att[s]; break;
                case BISHOP: a = bishop_att(s, p->all & ~p->bb[c * 6 + QUEEN]); break;
                case ROOK: a = rook_att(s, p->all & ~(p->bb[c * 6 + QUEEN] | p->bb[c * 6 + ROOK])); break;
                default: a = bishop_att(s, p->all) | rook_att(s, p->all); break;
                }
                if (a & zone[them]) {
                    katt_n[c]++;
                    katt_u[c] += SAFE_W[t];
                }
                int mob = popc(a & mob_ok);
                phase += t == QUEEN ? 4 : (t == ROOK ? 2 : 1);
                npm[c] += VAL_MG[t];
                switch (t) {
                case KNIGHT:
                    mg[c] += (mob - 4) * 4;
                    eg[c] += (mob - 4) * 4;
                    break;
                case BISHOP:
                    mg[c] += (mob - 6) * 4;
                    eg[c] += (mob - 6) * 5;
                    break;
                case ROOK: {
                    mg[c] += (mob - 7) * 2;
                    eg[c] += (mob - 7) * 4;
                    int f = s & 7;
                    if (!(pawns[c] & file_mask[f])) {
                        if (!(pawns[them] & file_mask[f])) {
                            mg[c] += 20;
                            eg[c] += 10;
                        } else {
                            mg[c] += 10;
                            eg[c] += 6;
                        }
                    }
                    if ((c == WHITE ? s >> 3 : 7 - (s >> 3)) == 6) {
                        mg[c] += 14;
                        eg[c] += 22;
                    }
                    break;
                }
                default:
                    mg[c] += (mob - 13);
                    eg[c] += (mob - 13) * 2;
                    break;
                }
            }
        }
        if (popc(p->bb[c * 6 + BISHOP]) >= 2) {
            mg[c] += 28;
            eg[c] += 45;
        }
        /* pawn shield and open files near the king */
        {
            int kf = ksq & 7, kr = c == WHITE ? ksq >> 3 : 7 - (ksq >> 3);
            int pen = 0;
            if (kr <= 2) {
                for (int f = (kf > 0 ? kf - 1 : 0); f <= (kf < 7 ? kf + 1 : 7); f++) {
                    U64 fp = pawns[c] & file_mask[f];
                    int best = 9;
                    while (fp) {
                        int s = pop_lsb(&fp);
                        int rr = c == WHITE ? s >> 3 : 7 - (s >> 3);
                        if (rr > kr && rr - kr < best) best = rr - kr;
                    }
                    if (best == 1) pen += 0;
                    else if (best == 2) pen += 7;
                    else if (best < 9) pen += 14;
                    else pen += 22;
                    if (!(pawns[them] & file_mask[f]) && !(pawns[c] & file_mask[f])) pen += 14;
                }
            }
            mg[c] -= pen;
        }
    }
    /* king safety: attackers of side s hurt the opposing king */
    for (int s = 0; s < 2; s++) {
        if (katt_n[s] >= 2) {
            int pen = katt_u[s] * katt_u[s] / 2;
            if (pen > 350) pen = 350;
            if (!p->bb[(s ^ 1) * 6 + QUEEN] && !p->bb[s * 6 + QUEEN]) pen /= 2;
            mg[s ^ 1] -= pen;
        }
    }
    if (phase > 24) phase = 24;
    int mgs = mg[WHITE] - mg[BLACK], egs = eg[WHITE] - eg[BLACK];
    int score = (mgs * phase + egs * (24 - phase)) / 24;
    /* drawish endings: the stronger side has no pawns and little extra material */
    if (score != 0) {
        int strong = score > 0 ? WHITE : BLACK;
        if (!pawns[strong] && npm[strong] - npm[strong ^ 1] < 400) score /= 4;
    }
    score += p->side == WHITE ? 10 : -10;
    return p->side == WHITE ? score : -score;
}

static int insufficient(const Pos *p) {
    if (p->bb[PAWN] | p->bb[6 + PAWN] | p->bb[ROOK] | p->bb[6 + ROOK] | p->bb[QUEEN] | p->bb[6 + QUEEN])
        return 0;
    int minors = popc(p->bb[KNIGHT] | p->bb[BISHOP] | p->bb[6 + KNIGHT] | p->bb[6 + BISHOP]);
    return minors <= 1;
}

/* ---------- SEE ---------- */
static const int SEEV[6] = {100, 320, 330, 500, 900, 20000};

static int see(const Pos *p, Move m) {
    int from = MFROM(m), to = MTO(m), us = p->side;
    int gain[34], d = 0;
    int mover = p->board[from] % 6;
    int cap = 0;
    U64 occ = p->all ^ BIT(from);
    if (m & F_EP) {
        cap = SEEV[PAWN];
        occ ^= BIT(to + (us == WHITE ? -8 : 8));
    } else if (p->board[to] != EMPTY)
        cap = SEEV[p->board[to] % 6];
    int cur = SEEV[mover];
    if (MPROMO(m)) {
        cap += SEEV[MPROMO(m)] - SEEV[PAWN];
        cur = SEEV[MPROMO(m)];
    }
    gain[0] = cap;
    U64 bq = p->bb[BISHOP] | p->bb[QUEEN] | p->bb[6 + BISHOP] | p->bb[6 + QUEEN];
    U64 rq = p->bb[ROOK] | p->bb[QUEEN] | p->bb[6 + ROOK] | p->bb[6 + QUEEN];
    U64 att = attackers_to(p, to, occ) & occ;
    int stm = us ^ 1;
    for (;;) {
        U64 mine = att & p->occ[stm];
        if (!mine) break;
        int t;
        U64 sub = 0;
        for (t = PAWN; t <= KING; t++) {
            sub = mine & p->bb[stm * 6 + t];
            if (sub) break;
        }
        d++;
        gain[d] = cur - gain[d - 1];
        cur = SEEV[t];
        occ ^= sub & -sub;
        att |= (bishop_att(to, occ) & bq) | (rook_att(to, occ) & rq);
        att &= occ;
        stm ^= 1;
        if (d >= 32) break;
    }
    d--;
    while (d > 0) {
        int a = -gain[d - 1], b = gain[d];
        gain[d - 1] = -(a > b ? a : b);
        d--;
    }
    return gain[0];
}

/* ---------- transposition table ---------- */
typedef struct {
    U64 key;
    int32_t move;
    int16_t score;
    int8_t depth;
    uint8_t flag; /* 0 none, 1 exact, 2 lower, 3 upper */
    uint8_t age;
    uint8_t pad[3];
} TTE;
static TTE *tt = NULL;
static U64 tt_mask = 0;
static uint8_t tt_age = 0;

static void tt_resize(size_t mb) {
    size_t n = 1;
    while (n * 2 * sizeof(TTE) <= mb * 1024 * 1024) n *= 2;
    free(tt);
    tt = (TTE *)calloc(n, sizeof(TTE));
    if (!tt) {
        n = 1 << 16;
        tt = (TTE *)calloc(n, sizeof(TTE));
    }
    tt_mask = n - 1;
}
static inline int score_to_tt(int s, int ply) {
    return s >= MATE_BOUND ? s + ply : (s <= -MATE_BOUND ? s - ply : s);
}
static inline int score_from_tt(int s, int ply) {
    return s >= MATE_BOUND ? s - ply : (s <= -MATE_BOUND ? s + ply : s);
}

/* ---------- search ---------- */
typedef struct {
    long long start, soft, hard;
    int depth;
    long long nodes;
    int infinite;
} Limits;

static Limits lim;
static long long nodes;
static int stopped, quit_flag;
static int seldepth;
static U64 g_hist[4096 + MAXPLY];
static int g_hlen;
static Move killers[MAXPLY][2];
static int history[2][64][64];
static Move countermove[64][64];
static Move stack_move[MAXPLY + 2];
static int eval_stack[MAXPLY + 2];
static Move pv[MAXPLY][MAXPLY];
static int pv_len[MAXPLY];
static Move root_best;
static int lmr_tab[64][64];
static int root_depth;

static char ibuf[1 << 16];
static int ilen = 0;

/* read one line from stdin; block != 0 waits. returns 1 on line, 0 none, -1 EOF */
static int read_line(char *out, size_t cap, int block) {
    for (;;) {
        char *nl = memchr(ibuf, '\n', (size_t)ilen);
        if (nl) {
            size_t len = (size_t)(nl - ibuf);
            size_t c = len < cap - 1 ? len : cap - 1;
            memcpy(out, ibuf, c);
            out[c] = 0;
            if (c && out[c - 1] == '\r') out[c - 1] = 0;
            memmove(ibuf, nl + 1, (size_t)ilen - len - 1);
            ilen -= (int)len + 1;
            return 1;
        }
        if (ilen >= (int)sizeof(ibuf) - 1) ilen = 0;
        if (!block) {
            struct pollfd pfd = {0, POLLIN, 0};
            if (poll(&pfd, 1, 0) <= 0) return 0;
        }
        ssize_t r = read(0, ibuf + ilen, sizeof(ibuf) - (size_t)ilen - 1);
        if (r <= 0) return -1;
        ilen += (int)r;
    }
}

static void check_input(void) {
    char line[256];
    int r;
    while ((r = read_line(line, sizeof line, 0)) > 0) {
        if (!strcmp(line, "stop")) stopped = 1;
        else if (!strcmp(line, "quit")) {
            stopped = 1;
            quit_flag = 1;
        } else if (!strcmp(line, "isready")) {
            printf("readyok\n");
            fflush(stdout);
        }
    }
    if (r < 0) {
        stopped = 1;
        quit_flag = 1;
    }
}

static void check_time(void) {
    if (root_depth > 1 && !lim.infinite) {
        if (lim.hard && now_ms() - lim.start >= lim.hard) stopped = 1;
        if (lim.nodes && nodes >= lim.nodes) stopped = 1;
    }
    check_input();
}

static inline int is_repetition(const Pos *p, int ply) {
    int idx = g_hlen + ply;
    int lo = idx - p->halfmove;
    if (lo < 0) lo = 0;
    for (int i = idx - 2; i >= lo; i -= 2)
        if (g_hist[i] == p->key) return 1;
    return 0;
}

static inline int has_nonpawn(const Pos *p, int c) {
    return (p->bb[c * 6 + KNIGHT] | p->bb[c * 6 + BISHOP] | p->bb[c * 6 + ROOK] | p->bb[c * 6 + QUEEN]) != 0;
}

static int qsearch(const Pos *p, int alpha, int beta, int ply, int qd) {
    if ((++nodes & 2047) == 0) check_time();
    if (stopped) return 0;
    if (ply > seldepth) seldepth = ply;
    if (ply >= MAXPLY - 1) return eval(p);
    int incheck = in_check(p);
    int pvnode = beta - alpha > 1;
    TTE *e = &tt[p->key & tt_mask];
    Move ttmove = 0;
    if (e->key == p->key && e->flag) {
        ttmove = e->move;
        int ts = score_from_tt(e->score, ply);
        if (!pvnode && ((e->flag == 1) || (e->flag == 2 && ts >= beta) || (e->flag == 3 && ts <= alpha))) return ts;
    }
    int stand = -INF, best;
    if (!incheck) {
        stand = eval(p);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
        best = stand;
    } else {
        if (qd >= 6) return eval(p);
        best = -MATE + ply;
    }
    Move mv[MAXMOVES];
    int sc[MAXMOVES];
    int n = gen_moves(p, mv, !incheck);
    for (int i = 0; i < n; i++) {
        Move m = mv[i];
        int s = 0;
        if (m == ttmove) s = 10000000;
        else if (m & F_CAP) {
            int victim = (m & F_EP) ? PAWN : p->board[MTO(m)] % 6;
            s = 1000000 + victim * 16 - p->board[MFROM(m)] % 6;
        } else if (MPROMO(m))
            s = 900000 + MPROMO(m);
        else
            s = history[p->side][MFROM(m)][MTO(m)];
        sc[i] = s;
    }
    int legal = 0, flag = 3, oa = alpha;
    Move bestmove = 0;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++)
            if (sc[j] > sc[bi]) bi = j;
        Move m = mv[bi];
        mv[bi] = mv[i];
        sc[bi] = sc[i];
        if (!incheck) {
            if (!MPROMO(m)) {
                int victim = (m & F_EP) ? PAWN : p->board[MTO(m)] % 6;
                if (stand + SEEV[victim] + 150 < alpha) continue;
            }
            if (!(m & F_EP) && see(p, m) < 0) continue;
        }
        Pos np = *p;
        if (!make_move(&np, m)) continue;
        legal++;
        int score = -qsearch(&np, -beta, -alpha, ply + 1, qd + 1);
        if (stopped) return 0;
        if (score > best) {
            best = score;
            bestmove = m;
            if (score > alpha) {
                alpha = score;
                flag = 1;
                if (score >= beta) {
                    flag = 2;
                    break;
                }
            }
        }
    }
    if (incheck && legal == 0) return -MATE + ply;
    (void)oa;
    if (!stopped) {
        e = &tt[p->key & tt_mask];
        e->key = p->key;
        e->move = bestmove;
        e->score = (int16_t)score_to_tt(best, ply);
        e->depth = 0;
        e->flag = (uint8_t)flag;
        e->age = tt_age;
    }
    return best;
}

static int search(const Pos *p, int depth, int alpha, int beta, int ply, int donull) {
    int pvnode = beta - alpha > 1;
    pv_len[ply] = ply;
    g_hist[g_hlen + ply] = p->key;
    if (stopped) return 0;
    if ((++nodes & 2047) == 0) check_time();
    if (stopped) return 0;
    if (ply > seldepth) seldepth = ply;
    int us = p->side;
    if (ply > 0) {
        if (p->halfmove >= 100 || is_repetition(p, ply) || insufficient(p)) return 0;
    }
    if (ply >= MAXPLY - 1) return eval(p);
    int incheck = in_check(p);
    if (ply > 0) {
        if (alpha < -MATE + ply) alpha = -MATE + ply;
        if (beta > MATE - ply - 1) beta = MATE - ply - 1;
        if (alpha >= beta) return alpha;
    }
    if (incheck) depth++;
    if (depth <= 0) return qsearch(p, alpha, beta, ply, 0);

    TTE *e = &tt[p->key & tt_mask];
    Move ttmove = 0;
    if (e->key == p->key && e->flag) {
        ttmove = e->move;
        int ts = score_from_tt(e->score, ply);
        if (!pvnode && ply > 0 && e->depth >= depth && p->halfmove < 90 &&
            ((e->flag == 1) || (e->flag == 2 && ts >= beta) || (e->flag == 3 && ts <= alpha)))
            return ts;
    }
    int se = incheck ? NOEVAL : eval(p);
    eval_stack[ply] = se;
    int improving = !incheck && ply >= 2 && eval_stack[ply - 2] != NOEVAL && se > eval_stack[ply - 2];

    if (!pvnode && !incheck && abs(beta) < MATE_BOUND) {
        if (depth <= 6 && se - 85 * depth + (improving ? 40 : 0) >= beta) return se;
        if (donull && depth >= 3 && se >= beta && has_nonpawn(p, us)) {
            Pos np = *p;
            make_null(&np);
            stack_move[ply] = 0;
            int R = 3 + depth / 4 + ((se - beta) / 200 > 3 ? 3 : (se - beta) / 200);
            int score = -search(&np, depth - 1 - R, -beta, -beta + 1, ply + 1, 0);
            if (stopped) return 0;
            if (score >= beta) return score >= MATE_BOUND ? beta : score;
        }
    }
    if (!ttmove && depth >= 6) depth--; /* cheap internal iterative reduction */

    Move mv[MAXMOVES];
    int sc[MAXMOVES];
    int n = gen_moves(p, mv, 0);
    Move prev = ply > 0 ? stack_move[ply - 1] : 0;
    Move cm = prev ? countermove[MFROM(prev)][MTO(prev)] : 0;
    for (int i = 0; i < n; i++) {
        Move m = mv[i];
        int s;
        if (m == ttmove) s = 100000000;
        else if (m & F_CAP) {
            int victim = (m & F_EP) ? PAWN : p->board[MTO(m)] % 6;
            int att = p->board[MFROM(m)] % 6;
            if (victim >= att || see(p, m) >= 0) s = 10000000 + victim * 16 - att;
            else s = -1000000 + victim * 16 - att;
            if (MPROMO(m) == QUEEN) s += 500000;
        } else if (MPROMO(m)) {
            s = MPROMO(m) == QUEEN ? 9000000 : -2000000;
        } else if (m == killers[ply][0]) s = 8000000;
        else if (m == killers[ply][1]) s = 7000000;
        else if (m == cm) s = 6000000;
        else s = history[us][MFROM(m)][MTO(m)];
        sc[i] = s;
    }
    int legal = 0, best = -INF, oa = alpha;
    Move bestmove = 0;
    Move quiets[64];
    int nq = 0;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++)
            if (sc[j] > sc[bi]) bi = j;
        Move m = mv[bi];
        int msc = sc[bi];
        mv[bi] = mv[i];
        sc[bi] = sc[i];
        int quiet = !(m & F_CAP) && !MPROMO(m);
        if (!pvnode && !incheck && legal > 0 && best > -MATE_BOUND) {
            if (quiet && depth <= 4 && legal >= 3 + depth * depth) continue;
            if (quiet && depth <= 5 && se + 90 + 90 * depth <= alpha) continue;
            if ((m & F_CAP) && depth <= 6 && msc < 0 && see(p, m) < -80 * depth) continue;
        }
        Pos np = *p;
        if (!make_move(&np, m)) continue;
        legal++;
        int gives_check = in_check(&np);
        int newdepth = depth - 1;
        stack_move[ply] = m;
        int score;
        if (legal == 1) {
            score = -search(&np, newdepth, -beta, -alpha, ply + 1, 1);
        } else {
            int r = 0;
            if (depth >= 3 && legal >= (pvnode ? 4 : 3) && quiet) {
                r = lmr_tab[depth < 63 ? depth : 63][legal < 63 ? legal : 63];
                if (!pvnode) r++;
                if (!improving) r++;
                if (incheck) r--;
                if (gives_check) r--;
                if (m == killers[ply][0] || m == killers[ply][1]) r--;
                r -= history[us][MFROM(m)][MTO(m)] / 5000;
                if (r < 0) r = 0;
                if (r > newdepth - 1) r = newdepth - 1;
                if (r < 0) r = 0;
            }
            score = -search(&np, newdepth - r, -alpha - 1, -alpha, ply + 1, 1);
            if (score > alpha && r > 0) score = -search(&np, newdepth, -alpha - 1, -alpha, ply + 1, 1);
            if (score > alpha && score < beta) score = -search(&np, newdepth, -beta, -alpha, ply + 1, 1);
        }
        if (stopped) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (score > best) {
            best = score;
            bestmove = m;
            if (score > alpha) {
                alpha = score;
                pv[ply][ply] = m;
                for (int k = ply + 1; k < pv_len[ply + 1]; k++) pv[ply][k] = pv[ply + 1][k];
                pv_len[ply] = pv_len[ply + 1] > ply + 1 ? pv_len[ply + 1] : ply + 1;
                if (ply == 0) root_best = m;
                if (score >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) {
                            killers[ply][1] = killers[ply][0];
                            killers[ply][0] = m;
                        }
                        int bonus = depth * depth > 400 ? 400 : depth * depth;
                        for (int k = 0; k < nq; k++) {
                            int *h = &history[us][MFROM(quiets[k])][MTO(quiets[k])];
                            int b = quiets[k] == m ? bonus : -bonus;
                            *h += b - *h * abs(b) / 8000;
                        }
                        if (prev) countermove[MFROM(prev)][MTO(prev)] = m;
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return incheck ? -MATE + ply : 0;
    e = &tt[p->key & tt_mask];
    if (!(e->key == p->key && e->age == tt_age && e->depth > depth + 3)) {
        e->key = p->key;
        e->move = bestmove;
        e->score = (int16_t)score_to_tt(best, ply);
        e->depth = (int8_t)depth;
        e->flag = best >= beta ? 2 : (best > oa ? 1 : 3);
        e->age = tt_age;
    }
    return best;
}

static void print_info(int depth, int score, const Pos *root) {
    long long el = now_ms() - lim.start;
    if (el < 1) el = 1;
    printf("info depth %d seldepth %d score ", depth, seldepth);
    if (score >= MATE_BOUND) printf("mate %d", (MATE - score + 1) / 2);
    else if (score <= -MATE_BOUND) printf("mate %d", -(MATE + score + 1) / 2);
    else printf("cp %d", score);
    printf(" nodes %lld nps %lld time %lld pv", nodes, nodes * 1000 / el, el);
    Pos tmp = *root;
    for (int i = 0; i < pv_len[0]; i++) {
        char s[8];
        move_str(pv[0][i], s);
        printf(" %s", s);
        if (!make_move(&tmp, pv[0][i])) break;
    }
    printf("\n");
    fflush(stdout);
}

static void think(const Pos *root) {
    Move list[MAXMOVES];
    int n = gen_moves(root, list, 0), nlegal = 0;
    Move first = 0;
    for (int i = 0; i < n; i++) {
        Pos np = *root;
        if (make_move(&np, list[i])) {
            if (!first) first = list[i];
            nlegal++;
        }
    }
    if (!nlegal) {
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }
    stopped = 0;
    nodes = 0;
    seldepth = 0;
    root_best = first;
    tt_age++;
    memset(killers, 0, sizeof killers);
    memset(countermove, 0, sizeof countermove);
    memset(eval_stack, 0, sizeof eval_stack);
    for (int c = 0; c < 2; c++)
        for (int a = 0; a < 64; a++)
            for (int b = 0; b < 64; b++) history[c][a][b] /= 2;
    int prev = 0, maxd = lim.depth ? lim.depth : MAXPLY - 2;
    Move best = first;
    for (int d = 1; d <= maxd; d++) {
        root_depth = d;
        int alpha = -INF, beta = INF, delta = 18, score;
        if (d >= 5) {
            alpha = prev - delta;
            beta = prev + delta;
        }
        for (;;) {
            score = search(root, d, alpha, beta, 0, 1);
            if (stopped) break;
            if (score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = score - delta > -INF ? score - delta : -INF;
            } else if (score >= beta) {
                beta = score + delta < INF ? score + delta : INF;
            } else
                break;
            delta += delta / 2 + 5;
            if (delta > 800) {
                alpha = -INF;
                beta = INF;
            }
        }
        if (stopped) {
            if (d == 1) best = root_best;
            else best = root_best; /* partial iterations only replace the move with a better one */
            break;
        }
        best = root_best;
        prev = score;
        print_info(d, score, root);
        if (!lim.infinite) {
            long long el = now_ms() - lim.start;
            if (lim.soft && el * 10 > lim.soft * 6) break;
            if (nlegal == 1 && d >= 4 && lim.soft) break;
            if (abs(score) >= MATE_BOUND && d >= 8) break;
        }
    }
    char s[8];
    move_str(best, s);
    printf("bestmove %s\n", s);
    fflush(stdout);
}

/* ---------- UCI ---------- */
static Pos g_pos;
static int move_overhead = 30;

static void set_position(char *args) {
    Pos p;
    char *mvs = strstr(args, "moves");
    char fen[256];
    if (!strncmp(args, "startpos", 8)) {
        set_fen(&p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    } else if (!strncmp(args, "fen", 3)) {
        char *f = args + 3;
        while (*f == ' ') f++;
        size_t len = mvs ? (size_t)(mvs - f) : strlen(f);
        if (len > 255) len = 255;
        memcpy(fen, f, len);
        fen[len] = 0;
        if (!set_fen(&p, fen)) return;
    } else
        return;
    g_hlen = 0;
    g_hist[g_hlen++] = p.key;
    if (mvs) {
        char *tok = strtok(mvs + 5, " \t");
        while (tok) {
            Move m = parse_move(&p, tok);
            if (!m) break;
            make_move(&p, m);
            if (g_hlen < 4096) g_hist[g_hlen++] = p.key;
            tok = strtok(NULL, " \t");
        }
    }
    g_pos = p;
    g_hlen--; /* the root itself is written by search() at index g_hlen */
}

static void go(char *args) {
    int wtime = -1, btime = -1, winc = 0, binc = 0, mtg = 0, movetime = -1, depth = 0;
    long long nlim = 0;
    int infinite = 0, perft_d = 0;
    char *tok = strtok(args, " \t");
    while (tok) {
        if (!strcmp(tok, "wtime")) wtime = atoi(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "btime")) btime = atoi(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "winc")) winc = atoi(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "binc")) binc = atoi(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "movestogo")) mtg = atoi(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "movetime")) movetime = atoi(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "depth")) depth = atoi(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "nodes")) nlim = atoll(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "perft")) perft_d = atoi(strtok(NULL, " \t") ?: "0");
        else if (!strcmp(tok, "infinite")) infinite = 1;
        tok = strtok(NULL, " \t");
    }
    if (perft_d > 0) {
        long long t0 = now_ms();
        Move list[MAXMOVES];
        int n = gen_moves(&g_pos, list, 0);
        U64 total = 0;
        for (int i = 0; i < n; i++) {
            Pos np = g_pos;
            if (!make_move(&np, list[i])) continue;
            U64 c = perft_d <= 1 ? 1 : perft(&np, perft_d - 1);
            char s[8];
            move_str(list[i], s);
            printf("%s: %llu\n", s, (unsigned long long)c);
            total += c;
        }
        long long el = now_ms() - t0;
        printf("\nNodes searched: %llu\nTime: %lld ms\n", (unsigned long long)total, el);
        fflush(stdout);
        return;
    }
    memset(&lim, 0, sizeof lim);
    lim.start = now_ms();
    lim.depth = depth;
    lim.nodes = nlim;
    lim.infinite = infinite;
    int t = g_pos.side == WHITE ? wtime : btime, inc = g_pos.side == WHITE ? winc : binc;
    if (movetime >= 0) {
        lim.soft = lim.hard = movetime > move_overhead ? movetime - move_overhead : 1;
    } else if (t >= 0 && !infinite) {
        long long avail = t - move_overhead;
        if (avail < 1) avail = 1;
        int div = mtg > 0 ? (mtg < 40 ? mtg + 1 : 40) : 26;
        long long soft = avail / div + inc * 3 / 4;
        if (soft > avail * 4 / 10) soft = avail * 4 / 10;
        long long hard = soft * 4;
        if (hard > avail * 7 / 10) hard = avail * 7 / 10;
        if (soft < 1) soft = 1;
        if (hard < 1) hard = 1;
        lim.soft = soft;
        lim.hard = hard;
    }
    think(&g_pos);
}

int main(void) {
    init_tables();
    init_eval();
    for (int d = 0; d < 64; d++)
        for (int m = 0; m < 64; m++)
            lmr_tab[d][m] = (d > 0 && m > 0) ? (int)(0.75 + log((double)d) * log((double)m) / 2.25) : 0;
    tt_resize(64);
    set_fen(&g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    g_hlen = 0;
    char line[8192];
    int r;
    while (!quit_flag && (r = read_line(line, sizeof line, 1)) > 0) {
        char *cmd = line;
        while (*cmd == ' ') cmd++;
        if (!strcmp(cmd, "uci")) {
            printf("id name Carnivorus 1.0\nid author slipperysponge67-bit\n");
            printf("option name Hash type spin default 64 min 1 max 4096\n");
            printf("option name Threads type spin default 1 min 1 max 1\n");
            printf("option name Move Overhead type spin default 30 min 0 max 5000\n");
            printf("option name Clear Hash type button\n");
            printf("uciok\n");
        } else if (!strcmp(cmd, "isready")) {
            printf("readyok\n");
        } else if (!strcmp(cmd, "ucinewgame")) {
            memset(tt, 0, (tt_mask + 1) * sizeof(TTE));
            memset(history, 0, sizeof history);
        } else if (!strncmp(cmd, "setoption", 9)) {
            char *nm = strstr(cmd, "name"), *vl = strstr(cmd, "value");
            if (nm && vl) {
                if (!strncmp(nm + 5, "Hash", 4)) {
                    int mb = atoi(vl + 6);
                    tt_resize((size_t)(mb < 1 ? 1 : (mb > 4096 ? 4096 : mb)));
                } else if (!strncmp(nm + 5, "Move Overhead", 13)) {
                    move_overhead = atoi(vl + 6);
                }
            } else if (nm && !strncmp(nm + 5, "Clear Hash", 10)) {
                memset(tt, 0, (tt_mask + 1) * sizeof(TTE));
            }
        } else if (!strncmp(cmd, "position", 8)) {
            char *a = cmd + 8;
            while (*a == ' ') a++;
            set_position(a);
        } else if (!strncmp(cmd, "go", 2)) {
            go(cmd + 2);
        } else if (!strncmp(cmd, "perft", 5)) {
            char buf[64];
            snprintf(buf, sizeof buf, "perft %d", atoi(cmd + 5));
            go(buf);
        } else if (!strcmp(cmd, "quit")) {
            break;
        }
        fflush(stdout);
    }
    return 0;
}
