/*
 * move.c
 *
 * Compile:
 *
 *   gcc move.c libcubiomes.a -fwrapv -lm -O3 -pthread -o move
 *
 * Usage:
 *
 *   ./move 12
 *   ./move 12 --debug 1
 *   ./move 12 --debug 1 2
 *   ./move 12 --debug 1 2 3 4 5
 * (output debug outputs for the passing the nth constraint)
 */

#include "cubiomes/finders.h"
#include "results.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

extern int getFortressPieces(
    Piece *list,
    int n,
    int mc,
    uint64_t seed,
    int chunkX,
    int chunkZ
);


#define MC_VERSION MC_1_18
#define STRUCT_TYPE Fortress
#define NUM_FORTS 5
#define MAX_PIECES 512
#define MAX_DISTANCE 6000
#define DEFAULT_THREADS 12
#define JOB_BATCH 256
#define PROGRESS_INTERVAL_MS 500
#define X_REACH 98
#define Z_REACH 40
#define PIECE_RADIUS 8
#define X_MUL 341873128712LL
#define Z_MUL 132897987541LL


static const int REGION_X[NUM_FORTS] = {
    0, 0, 1, 1, 2
};
static const int REGION_Z[NUM_FORTS] = {
    0, -1, 0, -1, 0
};


typedef struct {
    int x;
    int z;
} Translation;

typedef struct {
    bool enabled[NUM_FORTS];
} DebugOptions;

typedef struct {
    int x1;
    int z1;
    int x2;
    int z2;
} PieceInfo;

typedef struct {
    int startX;
    int startZ;

    PieceInfo *pieces;
    int pieceCount;

    int minX;
    int maxX;
    int minZ;
    int maxZ;
} FortressInfo;

typedef struct {
    size_t fortressPassed[NUM_FORTS];

    size_t fastMatches;
    size_t completeMatches;
} WorkerStats;

typedef struct {
    const Translation *translations;
    size_t translationCount;

    DebugOptions debug;

    atomic_size_t nextJob;
    atomic_size_t jobsCompleted;

    atomic_bool stop;

    pthread_mutex_t printMutex;
} SearchState;

typedef struct {
    SearchState *state;
    int threadId;

    Generator generator;
    Piece rawPieces[MAX_PIECES];
    PieceInfo fortressPieces[NUM_FORTS][MAX_PIECES];

    WorkerStats stats;
} WorkerContext;

static inline uint64_t moveStructureLocal(
    uint64_t baseSeed,
    int moveX,
    int moveZ)
{
    int64_t delta =
        (int64_t) moveX * X_MUL +
        (int64_t) moveZ * Z_MUL;

    return
        (baseSeed - (uint64_t) delta) &
        MASK48;
}

static bool generateFortress(
    WorkerContext *worker,
    uint64_t seed,
    int chunkX,
    int chunkZ,
    FortressInfo *out,
    int fortressIndex)
{
    int count;

    memset(
        out,
        0,
        sizeof(*out)
    );

    out->startX = chunkX * 16;
    out->startZ = chunkZ * 16;

    out->pieces =
        worker->fortressPieces[fortressIndex];

    out->minX = INT_MAX;
    out->maxX = INT_MIN;
    out->minZ = INT_MAX;
    out->maxZ = INT_MIN;
    count = getFortressPieces(
        worker->rawPieces,
        MAX_PIECES,
        MC_VERSION,
        seed,
        chunkX,
        chunkZ
    );

    if (count <= 0)
        return false;

    if (count > MAX_PIECES) {
        return false;
    }

    out->pieceCount = count;

    for (int i = 0; i < count; i++) {
        int x1 = worker->rawPieces[i].bb0.x;
        int z1 = worker->rawPieces[i].bb0.z;
        int x2 = worker->rawPieces[i].bb1.x;
        int z2 = worker->rawPieces[i].bb1.z;

        out->pieces[i].x1 = x1;
        out->pieces[i].z1 = z1;
        out->pieces[i].x2 = x2;
        out->pieces[i].z2 = z2;

        if (x1 < out->minX)
            out->minX = x1;

        if (x2 > out->maxX)
            out->maxX = x2;

        if (z1 < out->minZ)
            out->minZ = z1;

        if (z2 > out->maxZ)
            out->maxZ = z2;
    }

    return true;
}

static inline bool piecesIntersect(
    const PieceInfo *a,
    const PieceInfo *b)
{
    int ax1 = a->x1 - PIECE_RADIUS;
    int ax2 = a->x2 + PIECE_RADIUS;
    int az1 = a->z1 - PIECE_RADIUS;
    int az2 = a->z2 + PIECE_RADIUS;

    int bx1 = b->x1 - PIECE_RADIUS;
    int bx2 = b->x2 + PIECE_RADIUS;
    int bz1 = b->z1 - PIECE_RADIUS;
    int bz2 = b->z2 + PIECE_RADIUS;

    if (ax2 < bx1)
        return false;

    if (bx2 < ax1)
        return false;

    if (az2 < bz1)
        return false;

    if (bz2 < az1)
        return false;

    return true;
}

static int countPairIntersections(
    const FortressInfo *a,
    const FortressInfo *b)
{
    int count = 0;

    for (int i = 0; i < a->pieceCount; i++) {
        const PieceInfo *pa = &a->pieces[i];

        for (int j = 0; j < b->pieceCount; j++) {
            if (piecesIntersect(
                    pa,
                    &b->pieces[j]))
            {
                count++;
            }
        }
    }

    return count;
}

static inline bool checkF1(
    const FortressInfo *f,
    int *piece)
{
    bool left = false;
    bool right = false;

    *piece = -1;

    for (int i = 0; i < f->pieceCount; i++) {
        const PieceInfo *p = &f->pieces[i];

        if (p->x1 <= f->startX - X_REACH)
            left = true;

        if (p->x2 >= f->startX + X_REACH)
            right = true;

        if (p->x2 >= f->startX + X_REACH &&
            p->z1 <= f->startZ - Z_REACH)
        {
            *piece = i;
        }
    }

    return
        left &&
        right &&
        *piece >= 0;
}

static inline bool checkF2(
    const FortressInfo *f,
    int *leftPiece,
    int *rightPiece)
{
    bool left = false;
    bool right = false;

    *leftPiece = -1;
    *rightPiece = -1;

    for (int i = 0; i < f->pieceCount; i++) {
        const PieceInfo *p = &f->pieces[i];

        if (p->x1 <= f->startX - X_REACH)
            left = true;

        if (p->x2 >= f->startX + X_REACH)
            right = true;

        if (p->x1 <= f->startX - X_REACH &&
            p->z2 >= f->startZ + Z_REACH)
        {
            *leftPiece = i;
        }

        if (p->x2 >= f->startX + X_REACH &&
            p->z2 >= f->startZ + Z_REACH)
        {
            *rightPiece = i;
        }
    }

    return
        left &&
        right &&
        *leftPiece >= 0 &&
        *rightPiece >= 0;
}

static inline bool checkF3(
    const FortressInfo *f,
    int *leftPiece,
    int *rightPiece)
{
    *leftPiece = -1;
    *rightPiece = -1;

    for (int i = 0; i < f->pieceCount; i++) {
        const PieceInfo *p = &f->pieces[i];

        if (p->x1 <= f->startX - X_REACH &&
            p->z1 <= f->startZ - Z_REACH)
        {
            *leftPiece = i;
        }

        if (p->x2 >= f->startX + X_REACH &&
            p->z1 <= f->startZ - Z_REACH)
        {
            *rightPiece = i;
        }
    }

    return
        *leftPiece >= 0 &&
        *rightPiece >= 0;
}

static inline bool checkF4(
    const FortressInfo *f,
    int *leftPiece,
    int *rightPiece)
{
    *leftPiece = -1;
    *rightPiece = -1;

    for (int i = 0; i < f->pieceCount; i++) {
        const PieceInfo *p = &f->pieces[i];

        if (p->x1 <= f->startX - X_REACH &&
            p->z2 >= f->startZ + Z_REACH)
        {
            *leftPiece = i;
        }

        if (p->x2 >= f->startX + X_REACH &&
            p->z2 >= f->startZ + Z_REACH)
        {
            *rightPiece = i;
        }
    }

    return
        *leftPiece >= 0 &&
        *rightPiece >= 0;
}

static inline bool checkF5(
    const FortressInfo *f,
    int *leftPiece,
    int *rightPiece)
{
    *leftPiece = -1;
    *rightPiece = -1;

    for (int i = 0; i < f->pieceCount; i++) {
        const PieceInfo *p = &f->pieces[i];

        if (p->x1 <= f->startX - X_REACH &&
            p->z1 <= f->startZ - Z_REACH)
        {
            *leftPiece = i;
        }

        if (p->x2 >= f->startX + X_REACH)
        {
            *rightPiece = i;
        }
    }

    return
        *leftPiece >= 0 &&
        *rightPiece >= 0;
}

static void printDebugPiece(
    const FortressInfo *f,
    int index,
    const char *name)
{
    if (index < 0 ||
        index >= f->pieceCount)
    {
        printf(
            "    %-14s <none>\n",
            name
        );
        return;
    }

    const PieceInfo *p =
        &f->pieces[index];

    printf(
        "    %-14s piece=%d "
        "absX=[%d,%d] "
        "absZ=[%d,%d] "
        "relX=[%d,%d] "
        "relZ=[%d,%d]\n",
        name,
        index,
        p->x1,
        p->x2,
        p->z1,
        p->z2,
        p->x1 - f->startX,
        p->x2 - f->startX,
        p->z1 - f->startZ,
        p->z2 - f->startZ
    );
}

static void printDebugFortress(
    SearchState *state,
    int fortressIndex,
    uint64_t baseSeed,
    uint64_t movedSeed,
    int moveX,
    int moveZ,
    int baseRegionX,
    int baseRegionZ,
    int translatedRegionX,
    int translatedRegionZ,
    const Pos *pos,
    const FortressInfo *f,
    int pieceA,
    int pieceB)
{
    pthread_mutex_lock(
        &state->printMutex
    );

    printf(
        "\n"
        "============================================================\n"
        "F%d PASS\n"
        "============================================================\n"
        "BASE SEED          : %llu\n"
        "MOVED SEED         : %llu\n"
        "MOVEMENT           : (%d, %d)\n"
        "DISTANCE           : %d\n"
        "BASE REGION        : (%d, %d)\n"
        "TRANSLATED REGION  : (%d, %d)\n"
        "STRUCT POS         : (%d, %d)\n"
        "STRUCT CHUNK       : (%d, %d)\n"
        "PIECE COUNT        : %d\n"
        "BOUNDING BOX       : X=[%d,%d] Z=[%d,%d]\n"
        "RELATIVE BOX       : X=[%d,%d] Z=[%d,%d]\n",
        fortressIndex + 1,
        (unsigned long long) baseSeed,
        (unsigned long long) movedSeed,
        moveX,
        moveZ,
        abs(moveX) + abs(moveZ),
        baseRegionX,
        baseRegionZ,
        translatedRegionX,
        translatedRegionZ,
        pos->x,
        pos->z,
        pos->x >> 4,
        pos->z >> 4,
        f->pieceCount,
        f->minX,
        f->maxX,
        f->minZ,
        f->maxZ,
        f->minX - f->startX,
        f->maxX - f->startX,
        f->minZ - f->startZ,
        f->maxZ - f->startZ
    );

    printDebugPiece(
        f,
        pieceA,
        "MATCH A:"
    );

    printDebugPiece(
        f,
        pieceB,
        "MATCH B:"
    );

    printf(
        "============================================================\n"
    );

    fflush(stdout);

    pthread_mutex_unlock(
        &state->printMutex
    );
}

static bool testSeed(
    WorkerContext *worker,
    uint64_t movedSeed,
    int moveX,
    int moveZ,
    uint64_t baseSeed)
{
    SearchState *state =
        worker->state;

    Generator *g =
        &worker->generator;

    Pos positions[NUM_FORTS];

    int translatedRegionX[NUM_FORTS];
    int translatedRegionZ[NUM_FORTS];

    FortressInfo forts[NUM_FORTS];

    int pieceA[NUM_FORTS];
    int pieceB[NUM_FORTS];

    memset(
        forts,
        0,
        sizeof(forts)
    );

    for (int i = 0; i < NUM_FORTS; i++) {
        pieceA[i] = -1;
        pieceB[i] = -1;

        forts[i].pieces =
            worker->fortressPieces[i];
    }

    applySeed(
        g,
        DIM_NETHER,
        movedSeed
    );

    for (int i = 0; i < NUM_FORTS; i++) {
        translatedRegionX[i] =
            REGION_X[i] + moveX;

        translatedRegionZ[i] =
            REGION_Z[i] + moveZ;

        if (!getStructurePos(
                STRUCT_TYPE,
                MC_VERSION,
                movedSeed,
                translatedRegionX[i],
                translatedRegionZ[i],
                &positions[i]))
        {
            return false;
        }
    }
    for (int i = 0; i < NUM_FORTS; i++) {
        if (!isViableStructurePos(
                STRUCT_TYPE,
                g,
                positions[i].x,
                positions[i].z,
                0))
        {
            return false;
        }
    }

    if (!generateFortress(
            worker,
            movedSeed,
            positions[0].x >> 4,
            positions[0].z >> 4,
            &forts[0],
            0))
    {
        return false;
    }

    if (!checkF1(
            &forts[0],
            &pieceA[0]))
    {
        return false;
    }

    worker->stats.fortressPassed[0]++;

    if (state->debug.enabled[0]) {
        printDebugFortress(
            state,
            0,
            baseSeed,
            movedSeed,
            moveX,
            moveZ,
            REGION_X[0],
            REGION_Z[0],
            translatedRegionX[0],
            translatedRegionZ[0],
            &positions[0],
            &forts[0],
            pieceA[0],
            -1
        );
    }

    if (!generateFortress(
            worker,
            movedSeed,
            positions[1].x >> 4,
            positions[1].z >> 4,
            &forts[1],
            1))
    {
        return false;
    }

    if (!checkF2(
            &forts[1],
            &pieceA[1],
            &pieceB[1]))
    {
        return false;
    }

    worker->stats.fortressPassed[1]++;

    if (state->debug.enabled[1]) {
        printDebugFortress(
            state,
            1,
            baseSeed,
            movedSeed,
            moveX,
            moveZ,
            REGION_X[1],
            REGION_Z[1],
            translatedRegionX[1],
            translatedRegionZ[1],
            &positions[1],
            &forts[1],
            pieceA[1],
            pieceB[1]
        );
    }

    if (!generateFortress(
            worker,
            movedSeed,
            positions[2].x >> 4,
            positions[2].z >> 4,
            &forts[2],
            2))
    {
        return false;
    }

    if (!checkF3(
            &forts[2],
            &pieceA[2],
            &pieceB[2]))
    {
        return false;
    }

    worker->stats.fortressPassed[2]++;

    if (state->debug.enabled[2]) {
        printDebugFortress(
            state,
            2,
            baseSeed,
            movedSeed,
            moveX,
            moveZ,
            REGION_X[2],
            REGION_Z[2],
            translatedRegionX[2],
            translatedRegionZ[2],
            &positions[2],
            &forts[2],
            pieceA[2],
            pieceB[2]
        );
    }

    if (!generateFortress(
            worker,
            movedSeed,
            positions[3].x >> 4,
            positions[3].z >> 4,
            &forts[3],
            3))
    {
        return false;
    }

    if (!checkF4(
            &forts[3],
            &pieceA[3],
            &pieceB[3]))
    {
        return false;
    }

    worker->stats.fortressPassed[3]++;

    if (state->debug.enabled[3]) {
        printDebugFortress(
            state,
            3,
            baseSeed,
            movedSeed,
            moveX,
            moveZ,
            REGION_X[3],
            REGION_Z[3],
            translatedRegionX[3],
            translatedRegionZ[3],
            &positions[3],
            &forts[3],
            pieceA[3],
            pieceB[3]
        );
    }

    if (!generateFortress(
            worker,
            movedSeed,
            positions[4].x >> 4,
            positions[4].z >> 4,
            &forts[4],
            4))
    {
        return false;
    }

    if (!checkF5(
            &forts[4],
            &pieceA[4],
            &pieceB[4]))
    {
        return false;
    }

    worker->stats.fortressPassed[4]++;

    if (state->debug.enabled[4]) {
        printDebugFortress(
            state,
            4,
            baseSeed,
            movedSeed,
            moveX,
            moveZ,
            REGION_X[4],
            REGION_Z[4],
            translatedRegionX[4],
            translatedRegionZ[4],
            &positions[4],
            &forts[4],
            pieceA[4],
            pieceB[4]
        );
    }

    worker->stats.fastMatches++;

    int totalIntersections = 0;

    for (int i = 0; i < NUM_FORTS - 1; i++) {
        int intersections =
            countPairIntersections(
                &forts[i],
                &forts[i + 1]
            );

        totalIntersections +=
            intersections;

        if (intersections == 0)
            return false;
    }


    worker->stats.completeMatches++;

    pthread_mutex_lock(
        &state->printMutex
    );

printf(
    "\n"
    "============================================================\n"
    "FOUND 5-FORTRESS CHAIN\n"
    "============================================================\n"
    "BASE SEED       : %llu\n"
    "MOVED SEED      : %llu\n"
    "MOVEMENT        : (%d, %d)\n"
    "DISTANCE        : %d\n"
    "INTERSECTIONS   : %d\n"
    "\n"
    "F1 BLOCK        : (%d, %d)\n"
    "F1 CHUNK        : (%d, %d)\n"
    "F2 BLOCK        : (%d, %d)\n"
    "F2 CHUNK        : (%d, %d)\n"
    "F3 BLOCK        : (%d, %d)\n"
    "F3 CHUNK        : (%d, %d)\n"
    "F4 BLOCK        : (%d, %d)\n"
    "F4 CHUNK        : (%d, %d)\n"
    "F5 BLOCK        : (%d, %d)\n"
    "F5 CHUNK        : (%d, %d)\n"
    "\n"
    "F1 REGION       : (%d, %d) -> (%d, %d)\n"
    "F2 REGION       : (%d, %d) -> (%d, %d)\n"
    "F3 REGION       : (%d, %d) -> (%d, %d)\n"
    "F4 REGION       : (%d, %d) -> (%d, %d)\n"
    "F5 REGION       : (%d, %d) -> (%d, %d)\n"
    "============================================================\n",
    (unsigned long long) baseSeed,
    (unsigned long long) movedSeed,
    moveX,
    moveZ,
    abs(moveX) + abs(moveZ),
    totalIntersections,

    positions[0].x,
    positions[0].z,
    positions[0].x >> 4,
    positions[0].z >> 4,

    positions[1].x,
    positions[1].z,
    positions[1].x >> 4,
    positions[1].z >> 4,

    positions[2].x,
    positions[2].z,
    positions[2].x >> 4,
    positions[2].z >> 4,

    positions[3].x,
    positions[3].z,
    positions[3].x >> 4,
    positions[3].z >> 4,

    positions[4].x,
    positions[4].z,
    positions[4].x >> 4,
    positions[4].z >> 4,

    REGION_X[0],
    REGION_Z[0],
    translatedRegionX[0],
    translatedRegionZ[0],

    REGION_X[1],
    REGION_Z[1],
    translatedRegionX[1],
    translatedRegionZ[1],

    REGION_X[2],
    REGION_Z[2],
    translatedRegionX[2],
    translatedRegionZ[2],

    REGION_X[3],
    REGION_Z[3],
    translatedRegionX[3],
    translatedRegionZ[3],

    REGION_X[4],
    REGION_Z[4],
    translatedRegionX[4],
    translatedRegionZ[4]
);

    fflush(stdout);

    pthread_mutex_unlock(
        &state->printMutex
    );

    return true;
}


static Translation *makeTranslations(
    int maxDistance,
    size_t *outCount)
{
    size_t capacity =
        1ULL +
        2ULL *
        (size_t) maxDistance *
        (size_t) (maxDistance + 1);

    Translation *list =
        (Translation *) malloc(
            capacity * sizeof(Translation)
        );

    if (list == NULL) {
        *outCount = 0;
        return NULL;
    }

    size_t count = 0;

    list[count++] =
        (Translation) { 0, 0 };

    for (int d = 1; d <= maxDistance; d++) {
        for (int dx = -d; dx <= d; dx++) {
            int z =
                d - abs(dx);

            if (z == 0) {
                list[count++] =
                    (Translation) {
                        dx,
                        0
                    };
            }
            else {
                list[count++] =
                    (Translation) {
                        dx,
                        -z
                    };

                list[count++] =
                    (Translation) {
                        dx,
                        +z
                    };
            }
        }
    }

    *outCount = count;

    return list;
}


static void printUsage(
    const char *program)
{
    printf(
        "Usage:\n"
        "  %s [threads]\n"
        "  %s [threads] --debug 1 2 3 4 5\n"
        "\n"
        "Examples:\n"
        "  %s 12\n"
        "  %s 12 --debug 1\n"
        "  %s 12 --debug 1 2\n"
        "  %s 12 --debug 1 2 3 4\n"
        "  %s 12 --debug 1 2 3 4 5\n",
        program,
        program,
        program,
        program,
        program,
        program,
        program
    );
}


static bool parseArguments(
    int argc,
    char **argv,
    int *threads,
    DebugOptions *debug)
{
    *threads =
        DEFAULT_THREADS;

    memset(
        debug,
        0,
        sizeof(*debug)
    );

    int i = 1;

    if (i < argc &&
        argv[i][0] != '-')
    {
        char *end = NULL;

        long v =
            strtol(
                argv[i],
                &end,
                10
            );

        if (end == argv[i] ||
            *end != '\0' ||
            v <= 0 ||
            v > 10000)
        {
            fprintf(
                stderr,
                "Invalid thread count: %s\n",
                argv[i]
            );

            return false;
        }

        *threads =
            (int) v;

        i++;
    }

    while (i < argc) {
        if (strcmp(
                argv[i],
                "--debug") != 0)
        {
            fprintf(
                stderr,
                "Unknown argument: %s\n",
                argv[i]
            );

            return false;
        }

        i++;

        if (i >= argc) {
            fprintf(
                stderr,
                "--debug requires fortress numbers.\n"
            );

            return false;
        }

        while (i < argc) {
            char *end = NULL;

            long f =
                strtol(
                    argv[i],
                    &end,
                    10
                );

            if (end == argv[i] ||
                *end != '\0')
            {
                break;
            }

            if (f < 1 ||
                f > NUM_FORTS)
            {
                fprintf(
                    stderr,
                    "Invalid fortress number: %ld\n",
                    f
                );

                return false;
            }

            debug->enabled[f - 1] =
                true;

            i++;
        }
    }

    return true;
}

static void reportProgress(
    SearchState *state,
    WorkerContext *workers,
    int workerCount,
    size_t completed,
    size_t totalJobs)
{
    if (completed > totalJobs)
        completed = totalJobs;

    size_t fortressPassed[NUM_FORTS] = {
        0, 0, 0, 0, 0
    };

    for (int i = 0; i < workerCount; i++) {
        for (int f = 0; f < NUM_FORTS; f++) {
            fortressPassed[f] +=
                workers[i].stats.fortressPassed[f];
        }
    }

    size_t translationIndex =
        completed / RESULT_COUNT;

    if (translationIndex >=
        state->translationCount)
    {
        translationIndex =
            state->translationCount - 1;
    }

    int distance = 0;

    if (state->translationCount > 0) {
        distance =
            abs(
                state->translations[
                    translationIndex
                ].x
            ) +
            abs(
                state->translations[
                    translationIndex
                ].z
            );
    }

    double percent =
        totalJobs
            ? 100.0 *
              (double) completed /
              (double) totalJobs
            : 100.0;

    printf(
        "\nJobs: %zu/%zu "
        "(%.3f%%)"
        " | distance: <=%d"
        " | F1: %zu"
        " | F2: %zu"
        " | F3: %zu"
        " | F4: %zu"
        " | F5: %zu"
        "   ",
        completed,
        totalJobs,
        percent,
        distance,
        fortressPassed[0],
        fortressPassed[1],
        fortressPassed[2],
        fortressPassed[3],
        fortressPassed[4]
    );

    fflush(stdout);
}

static void *worker(
    void *arg)
{
    WorkerContext *worker =
        (WorkerContext *) arg;

    SearchState *state =
        worker->state;

    const size_t totalJobs =
        state->translationCount *
        RESULT_COUNT;

    setupGenerator(
        &worker->generator,
        MC_VERSION,
        0
    );


    while (!atomic_load_explicit(
                &state->stop,
                memory_order_relaxed))
    {
        size_t begin =
            atomic_fetch_add_explicit(
                &state->nextJob,
                JOB_BATCH,
                memory_order_relaxed
            );

        if (begin >= totalJobs)
            break;

        size_t end =
            begin + JOB_BATCH;

        if (end > totalJobs)
            end = totalJobs;


        for (size_t job = begin;
             job < end;
             job++)
        {
            if (atomic_load_explicit(
                    &state->stop,
                    memory_order_relaxed))
            {
                break;
            }

            size_t translationIndex =
                job / RESULT_COUNT;

            size_t resultIndex =
                job % RESULT_COUNT;

            const Translation *translation =
                &state->translations[
                    translationIndex
                ];

            int moveX =
                translation->x;

            int moveZ =
                translation->z;

            uint64_t baseSeed =
                results[resultIndex];

            uint64_t movedSeed =
                moveStructureLocal(
                    baseSeed,
                    moveX,
                    moveZ
                );

            (void) testSeed(
                worker,
                movedSeed,
                moveX,
                moveZ,
                baseSeed
            );

            atomic_fetch_add_explicit(
                &state->jobsCompleted,
                1,
                memory_order_relaxed
            );
        }
    }

    return NULL;
}

int main(
    int argc,
    char **argv)
{
    int threadCount;

    DebugOptions debug;

    if (!parseArguments(
            argc,
            argv,
            &threadCount,
            &debug))
    {
        printUsage(argv[0]);
        return 1;
    }

    size_t translationCount = 0;

    Translation *translations =
        makeTranslations(
            MAX_DISTANCE,
            &translationCount
        );

    if (translations == NULL) {
        fprintf(
            stderr,
            "ERROR: unable to allocate translations\n"
        );

        return 1;
    }


    SearchState state;

    memset(
        &state,
        0,
        sizeof(state)
    );

    state.translations =
        translations;

    state.translationCount =
        translationCount;

    state.debug =
        debug;

    atomic_init(
        &state.nextJob,
        0
    );

    atomic_init(
        &state.jobsCompleted,
        0
    );

    atomic_init(
        &state.stop,
        false
    );

    if (pthread_mutex_init(
            &state.printMutex,
            NULL) != 0)
    {
        fprintf(
            stderr,
            "ERROR: pthread_mutex_init failed\n"
        );

        free(translations);

        return 1;
    }

    pthread_t *threads =
        (pthread_t *) calloc(
            (size_t) threadCount,
            sizeof(pthread_t)
        );

    WorkerContext *workers =
        (WorkerContext *) calloc(
            (size_t) threadCount,
            sizeof(WorkerContext)
        );

    if (threads == NULL ||
        workers == NULL)
    {
        fprintf(
            stderr,
            "ERROR: thread allocation failed\n"
        );

        free(threads);
        free(workers);
        free(translations);

        pthread_mutex_destroy(
            &state.printMutex
        );

        return 1;
    }

    size_t totalJobs =
        translationCount *
        RESULT_COUNT;

    printf("Starting search...\n");

    printf("Debug:");

    bool anyDebug = false;

    for (int i = 0; i < NUM_FORTS; i++) {
        if (debug.enabled[i]) {
            printf(
                " F%d",
                i + 1
            );

            anyDebug = true;
        }
    }

    if (!anyDebug)
        printf(" none");

    printf(
        "\n"
        "============================================================\n"
    );

    fflush(stdout);

    int created = 0;

    for (int i = 0;
         i < threadCount;
         i++)
    {
        workers[i].state =
            &state;

        workers[i].threadId =
            i;

        memset(
            &workers[i].stats,
            0,
            sizeof(workers[i].stats)
        );

        int rc =
            pthread_create(
                &threads[i],
                NULL,
                worker,
                &workers[i]
            );

        if (rc != 0) {
            fprintf(
                stderr,
                "ERROR: pthread_create failed "
                "for thread %d: %s\n",
                i,
                strerror(rc)
            );

            atomic_store_explicit(
                &state.stop,
                true,
                memory_order_relaxed
            );

            break;
        }

        created++;
    }

    size_t lastReported = 0;

    while (true) {
        size_t completed =
            atomic_load_explicit(
                &state.jobsCompleted,
                memory_order_relaxed
            );

        if (completed != lastReported) {
            reportProgress(
                &state,
                workers,
                created,
                completed,
                totalJobs
            );

            lastReported =
                completed;
        }

        bool allDone =
            completed >= totalJobs;

        if (allDone)
            break;

        /*
         * usleep() is only used by the main progress thread.
         * Workers never sleep.
         */
        usleep(
            PROGRESS_INTERVAL_MS * 1000
        );
    }

    for (int i = 0;
         i < created;
         i++)
    {
        pthread_join(
            threads[i],
            NULL
        );
    }


    /*
     * One final progress update.
     */
    size_t finalCompleted =
        atomic_load_explicit(
            &state.jobsCompleted,
            memory_order_relaxed
        );

    reportProgress(
        &state,
        workers,
        created,
        finalCompleted,
        totalJobs
    );

    printf("\n\n");

    size_t fortressPassed[NUM_FORTS] = {
        0, 0, 0, 0, 0
    };

    size_t fastMatches = 0;
    size_t completeMatches = 0;

    for (int i = 0;
         i < created;
         i++)
    {
        for (int f = 0;
             f < NUM_FORTS;
             f++)
        {
            fortressPassed[f] +=
                workers[i]
                    .stats
                    .fortressPassed[f];
        }

        fastMatches +=
            workers[i]
                .stats
                .fastMatches;

        completeMatches +=
            workers[i]
                .stats
                .completeMatches;
    }

    printf(
        "============================================================\n"
        "SEARCH COMPLETE\n"
        "============================================================\n"
        "Jobs completed : %zu / %zu\n",
        finalCompleted,
        totalJobs
    );

    for (int i = 0;
         i < NUM_FORTS;
         i++)
    {
        printf(
            "F%d conditional passes : %zu\n",
            i + 1,
            fortressPassed[i]
        );
    }

    printf(
        "Fast matches    : %zu\n"
        "5/5 chains      : %zu\n"
        "============================================================\n",
        fastMatches,
        completeMatches
    );

    free(workers);
    free(threads);
    free(translations);

    pthread_mutex_destroy(
        &state.printMutex
    );

    return 0;
}
