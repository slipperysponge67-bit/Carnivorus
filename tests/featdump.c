/* Batch feature dump for the tuning pipeline: reads '<label> <fen>' lines
 * on stdin, writes '<label>\t<phase>\t<ks>\t<draw>\t<tempo>\t<side>\t<nf> i:v ...'
 * Built only for tuning; not part of the engine binary. */
#define main engine_main
#include "../carnivorus.c"
#undef main

int main(void) {
    init_tables();
    init_eval();
    static char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        char *fen = sp + 1;
        fen[strcspn(fen, "\r\n")] = 0;
        Pos p;
        if (!set_fen(&p, fen)) {
            fprintf(stderr, "bad fen: %s\n", fen);
            continue;
        }
        edump_wanted = 1;
        eval_uncached(&p);
        edump_wanted = 0;
        printf("%s\t%d\t%d\t%d\t%d\t%d\t%d", line, edump.phase, edump.ks, edump.draw, edump.tempo,
               edump.side, edump.nf);
        for (int i = 0; i < edump.nf; i++) printf(" %d:%d", edump.fi[i], edump.fv[i]);
        putchar('\n');
    }
    fflush(stdout);
    return 0;
}
