//compile gcc spanrarity.c libcubiomes.a -fwrapv -lm -O3 -o test
//Result: 1 in 46 fortress work for our search
#include "cubiomes/finders.h"
#include <stdio.h>
#include <stdlib.h>
int main()
{
    int structType = Fortress;
    int mc = MC_1_18;
    Generator g;
    setupGenerator(&g, mc, 0);
    uint64_t lower48;
    int valids = 0;
    for (lower48 = 0; lower48 < (1ULL<<15); lower48++)
    {
        Pos p;
        if (!getStructurePos(structType, mc, lower48, 0, 0, &p))
            continue;
        uint64_t upper16 = 0;
        uint64_t seed = lower48 | (upper16 << 48);
        applySeed(&g, DIM_NETHER, seed);
        if (isViableStructurePos(structType, &g, p.x, p.z, 0))
        {
            StructureSaltConfig ssconfig;
            getStructureSaltConfig(structType, mc, nether_wastes ,&ssconfig);
            StructureVariant sv;
            Piece *list = calloc(100000, sizeof(*list));
            int pieces = getStructurePieces(list, 100000, structType, ssconfig, &sv, mc, seed, p.x, p.z);
            int left = 0;
            int right = 0;
            for (int i = 0; i < pieces; i++){ // largest span 112 + cross dead zone of 80
                if(list[i].pos.x <= p.x - 100 && list[i].pos.z <= p.z - 40){
                    left=1;
                }
                if(list[i].pos.z >= p.z - 100 && list[i].pos.z <= p.z - 40){
                    right=1;
                }
            }
            if (left == 1 && right == 1){
                valids++;
            }
            free(list);
        }
    }
    printf("pct of seeds: %f\n", (double)valids / (1ULL<<15) * 100);
}