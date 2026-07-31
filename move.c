
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
 *
 * IMPORTANT:
 *
 * This version fixes THREE critical problems:
 *
 *   1. MOVED REGION POSITIONS
 *
 *      After moveStructure(baseSeed, moveX, moveZ), the original
 *      structure constellation must be queried at:
 *
 *          REGION_X[i] + moveX
 *          REGION_Z[i] + moveZ
 *
 *      NOT at the original untranslated regions.
 *
 *   2. isViableStructurePos()
 *
 *      getStructurePos() only gives the structure generation attempt.
 *      For MC 1.18+, fortress generation is biome/bastion dependent.
 *
 *      Therefore every translated fortress position is checked with:
 *
 *          isViableStructurePos(
 *              Fortress,
 *              &generator,
 *              pos.x,
 *              pos.z,
 *              0
 *          )
 *
 *      This is what prevents bastion locations from being accepted as
 *      fortresses.
 *
 *   3. POSITION UNITS
 *
 *      Pos returned by getStructurePos() is in BLOCK coordinates.
 *
 *      Therefore:
 *
 *          isViableStructurePos() -> pos.x, pos.z
 *
 *          getFortressPieces()   -> pos.x >> 4, pos.z >> 4
 *
 *      The >> 4 must NOT be applied before isViableStructurePos().
 *
 *
 * DEBUG BEHAVIOR:
 *
 *   --debug 1
 *       Only prints moved seeds that PASS F1.
 *
 *   --debug 2
 *       Only prints moved seeds that PASS F1 and F2.
 *
 *   --debug 3
 *       Only prints moved seeds that PASS F1-F3.
 *
 *   --debug 4
 *       Only prints moved seeds that PASS F1-F4.
 *
 *   --debug 5
 *       Only prints moved seeds that PASS F1-F5.
 *
 * Rejected seeds are NEVER debug-printed.
 */

#include "cubiomes/finders.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>


/*
 * Some cubiomes versions expose getFortressPieces()
 * through the library but not through the installed header.
 */
extern int getFortressPieces(
    Piece *list,
    int n,
    int mc,
    uint64_t seed,
    int chunkX,
    int chunkZ
);


/* ============================================================
 * CONFIG
 * ============================================================ */

#define MC_VERSION MC_1_18
#define STRUCT_TYPE Fortress

#define NUM_FORTS 5
#define MAX_PIECES 500

#define MAX_DISTANCE 1000
#define DEFAULT_THREADS 12
#define REPORT_EVERY 20

#define X_REACH 98
#define Z_REACH 40

#define PIECE_RADIUS 8

#define X_MUL 341873128712LL
#define Z_MUL 132897987541LL


/* ============================================================
 * SEEDS
 *
 * KEEP YOUR COMPLETE EXISTING results[] ARRAY.
 * ============================================================ */

static const uint64_t results[] = {
    1516793877ULL,
    1959990366103ULL,
    2578497130519ULL,
    4604681572245ULL,
    4983647399863ULL,
    5121117810711ULL,
    5396029239349ULL,
    7182737788821ULL,
    7319069372437ULL,
    7663738384439ULL,
    8247820472213ULL,
    8865188409397ULL,
    9208852959287ULL,
    9689922826165ULL,
    9724282589079ULL,
    11443244625973ULL,
    11786909175863ULL,
    12266903162807ULL,
    12267979042741ULL,
    12302338805655ULL,
    13331987943445ULL,
    14809523842999ULL,
    14809525940245ULL,
    15737138212789ULL,
    15771497975703ULL,
    15771596533813ULL,
    15910044160021ULL,
    17386506276887ULL,
    17387580059575ULL,
    18314118549431ULL,
    18349652750389ULL,
    18451588960279ULL,
    18452664840213ULL,
    18932658827157ULL,
    19414735433781ULL,
    19929126957079ULL,
    20856739229623ULL,
    20891197550647ULL,
    21200368103319ULL,
    21510715043733ULL,
    22471747530807ULL,
    22472785637301ULL,
    23055829618581ULL,
    23778424319895ULL,
    25014332534805ULL,
    26285609357239ULL,
    26938412937271ULL,
    26939585171349ULL,
    27592388751381ULL,
    28141070872501ULL,
    28175430635415ULL,
    28657471434773ULL,
    28863665573815ULL,
    29482172338231ULL,
    30718051209143ULL,
    32059186188183ULL,
    32986800557973ULL,
    33604300763031ULL,
    34222744580149ULL,
    35528345358231ULL,
    37280794783669ULL,
    37383806873493ULL,
    38106401574807ULL,
    38345877467061ULL,
    38724908339223ULL,
    39342345596983ULL,
    39651516149655ULL,
    40923933683637ULL,
    41267529019415ULL,
    41542440448053ULL,
    42812677001109ULL,
    43465480581141ULL,
    43810149593143ULL,
    44394231680917ULL,
    45355264167991ULL,
    45836334034869ULL,
    45870693797783ULL,
    46009239982101ULL,
    47933320384567ULL,
    48413314371511ULL,
    48414390251445ULL,
    48551860662293ULL,
    49478399152149ULL,
    49479472934837ULL,
    49513832697751ULL,
    50955935051703ULL,
    51128840998935ULL,
    52021019832341ULL,
    52056453271479ULL,
    52056455368725ULL,
    53533991268279ULL,
    53671461679127ULL,
    53877620141975ULL,
    54598000168983ULL,
    54599076048917ULL,
    55079070035861ULL,
    55561146642485ULL,
    56075538165783ULL,
    56214082252855ULL,
    57140620849175ULL,
    57415532277813ULL,
    57657126252437ULL,
    58618158739511ULL,
    58619196846005ULL,
    58962861395895ULL,
    59202240827285ULL,
    59683241422903ULL,
    59718677065751ULL,
    60680747659319ULL,
    61882331811861ULL,
    62432020565943ULL,
    63085996380053ULL,
    64287482081205ULL,
    64321841844119ULL,
    65010076782519ULL,
    66864462417847ULL,
    67483002695573ULL,
    68205597396887ULL,
    69407083098039ULL,
    69750711971735ULL,
    70369155788853ULL,
    71023129505717ULL,
    73530218082197ULL,
    73564676403221ULL,
    74492288675765ULL,
    74835953225655ULL,
    75488756805687ULL,
    77070344892341ULL,
    77414009442231ULL,
    77688851656757ULL,
    78032516206647ULL,
    78376082067383ULL,
    78959088209813ULL,
    79611891789845ULL,
    81501675376695ULL,
    81537144426389ULL,
    82155651190805ULL,
    84078689226647ULL,
    84079731593271ULL,
    84698271870997ULL,
    87275252207639ULL,
    87378299973685ULL,
    89301436649365ULL,
    89817872887831ULL,
    92360493461559ULL,
    92944575549333ULL,
    93561943486517ULL,
    93905608036407ULL,
    94386677903285ULL,
    94421037666199ULL,
    95109272604599ULL,
    95348652035989ULL,
    96963658239927ULL,
    97445770391445ULL,
    98028743020565ULL,
    99506278920119ULL,
    99506281017365ULL,
    100433893289909ULL,
    100468253052823ULL,
    100468351610933ULL,
    100571363700757ULL,
    103010873626551ULL,
    103148344037399ULL,
    103149419917333ULL,
    103527444099093ULL,
    103629413904277ULL,
    104111490510901ULL,
    104352008605591ULL,
    105553494306743ULL,
    105587952627767ULL,
    105897123180439ULL,
    106515566997557ULL,
    107169540714421ULL,
    107752584695701ULL,
    109711087611925ULL,
    110982364434359ULL,
    111635168014391ULL,
    111636340248469ULL,
    112837825949621ULL,
    113354226511893ULL,
    113560420650935ULL,
    114041456922519ULL,
    114178927415351ULL,
    115105499418517ULL,
    117683555635093ULL,
    118301055840151ULL,
    118919499657269ULL,
    120225100435351ULL,
    120844683079701ULL,
    122080561950613ULL,
    123042632544181ULL,
    123524711182389ULL,
    123868375732279ULL,
    124348271226775ULL,
    124383805362071ULL,
    125964284096535ULL,
    126926425935799ULL,
    127509432078229ULL,
    127991510716437ULL,
    128506904670263ULL,
    129090986758037ULL,
    129467972833303ULL,
    129469046615991ULL,
    130052019245111ULL,
    130431119306805ULL,
    130533089111989ULL,
    130567448874903ULL,
    132010593513495ULL,
    133110069448631ULL,
    133111111733271ULL,
    133248615739413ULL,
    133592181600149ULL,
    134175154229269ULL,
    134176228011957ULL,
    134210587774871ULL,
    134553214087223ULL,
    135550720323639ULL,
    135652690128823ULL,
    136717774909461ULL,
    136753208348599ULL,
    138574375219095ULL,
    139294755246103ULL,
    139673855307797ULL,
    139775825112981ULL,
    140256897191831ULL,
    140257901719605ULL,
    141734363836471ULL,
    141837375926295ULL,
    142112287354933ULL,
    142799517765559ULL,
    143315951923125ULL,
    143659616473015ULL,
    143898995904405ULL,
    144379996500023ULL,
    145377502736439ULL,
    146579086888981ULL,
    147128775643063ULL,
    147782751457173ULL,
    148263823536023ULL,
    148984237158325ULL,
    149362261340085ULL,
    150187868131223ULL,
    151423812153399ULL,
    152179757772693ULL,
    153005400240053ULL,
    153623907004469ULL,
    154447467048855ULL,
    155065910865973ULL,
    157472199774101ULL,
    159189043752885ULL,
    159532708302775ULL,
    160014786940983ULL,
    160530216570775ULL,
    163072837144503ULL,
    163210307555351ULL,
    163655843286933ULL,
    164137921925141ULL,
    165615457824695ULL,
    165752928235543ULL,
    166198430453815ULL,
    168295548809271ULL,
    168431916150837ULL,
    168775444303767ULL,
    169257522941975ULL,
    169395026948117ULL,
    169738592808853ULL,
    170699625295927ULL,
    171697131532343ULL,
    171799101337527ULL,
    171800143622167ULL,
    172075055050805ULL,
    173998191726485ULL,
    174342764195895ULL,
    176403308400535ULL,
    176541854584853ULL,
    177641330626453ULL,
    178258698563637ULL,
    178602363113527ULL,
    178945928974263ULL,
    179083432980405ULL,
    179806027681719ULL,
    180045407113109ULL,
    180287063953303ULL,
    181488549654455ULL,
    182142525468565ULL,
    182725498097685ULL,
    183104596062133ULL,
    183687640043413ULL,
    184410234744727ULL,
    185130648367029ULL,
    185165106688053ULL,
    185268118777877ULL,
    187570223362103ULL,
    188224199176213ULL,
    188326168981397ULL,
    188808245588021ULL,
    189151811448757ULL,
    189495475998647ULL,
    190113982763063ULL,
    190593878257559ULL,
    191693358346261ULL,
    193618610982805ULL,
    194237117747221ULL,
    194854555004981ULL,
    195095073099671ULL,
    195679119511479ULL,
    196161198149687ULL,
    198738211999639ULL,
    199356718764055ULL,
    199802254495637ULL,
    201899339444247ULL,
    202174250872885ULL,
    203960959422357ULL,
    204097291005973ULL,
    204441960017975ULL,
    204921855512471ULL,
    205541438156821ULL,
    208221466259509ULL,
    208565130809399ULL,
    209045124796343ULL,
    209046200676277ULL,
    209080560439191ULL,
    211587747573781ULL,
    211623181012919ULL,
    212549719609239ULL,
    212688265793557ULL,
    213787741835157ULL,
    214164727910423ULL,
    214165801693111ULL,
    214748774322231ULL,
    215092340182967ULL,
    215127874383925ULL,
    215229810593815ULL,
    215229844189109ULL,
    215230886473749ULL,
    216707348590615ULL,
    217634960863159ULL,
    217669419184183ULL,
    217807866810391ULL,
    218288936677269ULL,
    218871909306389ULL,
    218872983089077ULL,
    219249969164343ULL,
    219251007270837ULL,
    219834051252117ULL,
    220247475400759ULL,
    220556645953431ULL,
    221414529986581ULL,
    223716634570807ULL,
    223717806804885ULL,
    224370610384917ULL,
    224919292506037ULL,
    224953652268951ULL,
    225435693068309ULL,
    225641887207351ULL,
    227496272842679ULL,
    228837407821719ULL,
    229765022191509ULL,
    231000966213685ULL,
    231825530720183ULL,
    232960578613143ULL,
    234059016417205ULL,
    234162028507029ULL,
    234884623208343ULL,
    236120567230519ULL,
    236429737783191ULL,
    237702155317173ULL,
    238045750652951ULL,
    238320662081589ULL,
    240243702214677ULL,
    240588371226679ULL,
    242168954851221ULL,
    242787461615637ULL,
    244711542018103ULL,
    245191536005047ULL,
    245192611884981ULL,
    245226971647895ULL,
    245330082295829ULL,
    246292054331287ULL,
    247769592221623ULL,
    247907062632471ULL,
    248352598364053ULL,
    248834674905015ULL,
    248834677002261ULL,
    250312212901815ULL,
    250449683312663ULL,
    250655841775511ULL,
    251376221802519ULL,
    251377297682453ULL,
    252853759799319ULL,
    252992303886391ULL,
    253128671227957ULL,
    253954278019095ULL,
    254193753911349ULL,
    254435347885973ULL,
    255396380373047ULL,
    255397418479541ULL,
    255741083029431ULL,
    255980462460821ULL,
    256393886609463ULL,
    256461463056439ULL,
    256496898699287ULL,
    256771810127925ULL,
    257458969292855ULL,
    258660553445397ULL,
    259039519273015ULL,
    259864218013589ULL,
    261065703714741ULL,
    261100063477655ULL,
    261238609661973ULL,
    261788298416055ULL,
    263642684051383ULL,
    264261224329109ULL,
    264983819030423ULL,
    266185304731575ULL,
    266528933605271ULL,
    266839280545685ULL,
    267147377422389ULL,
    267801351139253ULL,
    268384395120533ULL,
    269106989821847ULL,
    270308439715733ULL,
    270342898036757ULL,
    272266978439223ULL,
    272920954253333ULL,
    273848566525877ULL,
    274192231075767ULL,
    274467073290293ULL,
    274810737840183ULL,
    276390113423381ULL,
    278315366059925ULL,
    278933872824341ULL,
    279551310082101ULL,
    279791828176791ULL,
    280856910860183ULL,
    280857953226807ULL
};

#define RESULT_COUNT (sizeof(results) / sizeof(results[0]))


/* ============================================================
 * BASE REGION LAYOUT
 * ============================================================ */

static const int REGION_X[NUM_FORTS] = {
    0, 0, 1, 1, 2
};

static const int REGION_Z[NUM_FORTS] = {
    0, -1, 0, -1, 0
};


/* ============================================================
 * TRANSLATION
 * ============================================================ */

typedef struct {
    int x;
    int z;
} Translation;


/* ============================================================
 * DEBUG
 * ============================================================ */

typedef struct {
    bool enabled[NUM_FORTS];
} DebugOptions;


/* ============================================================
 * PIECES
 * ============================================================ */

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


/* ============================================================
 * SEARCH STATE
 * ============================================================ */

typedef struct {
    const Translation *translations;
    size_t translationCount;

    DebugOptions debug;

    atomic_size_t nextJob;
    atomic_size_t jobsCompleted;

    atomic_size_t fortressPassed[NUM_FORTS];

    atomic_size_t fastMatches;
    atomic_size_t completeMatches;

    pthread_mutex_t printMutex;
} SearchState;

typedef struct {
    SearchState *state;
    int threadId;
} WorkerArgs;


/* ============================================================
 * STRUCTURE MOVEMENT
 * ============================================================ */

static uint64_t moveStructureLocal(
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


/* ============================================================
 * FREE FORTRESS
 * ============================================================ */

static void freeFortress(FortressInfo *f)
{
    if (f == NULL)
        return;

    free(f->pieces);

    f->pieces = NULL;
    f->pieceCount = 0;

    f->minX = 0;
    f->maxX = 0;
    f->minZ = 0;
    f->maxZ = 0;
}


/* ============================================================
 * GENERATE ACTUAL FORTRESS
 *
 * chunkX/chunkZ are ACTUAL GENERATION CHUNK COORDINATES.
 *
 * Caller obtains these from:
 *
 *     translatedPos.x >> 4
 *     translatedPos.z >> 4
 * ============================================================ */

static bool generateFortress(
    uint64_t seed,
    int chunkX,
    int chunkZ,
    FortressInfo *out)
{
    Piece *list;
    int count;

    memset(out, 0, sizeof(*out));

    out->startX = chunkX * 16;
    out->startZ = chunkZ * 16;

    out->minX = INT_MAX;
    out->maxX = INT_MIN;
    out->minZ = INT_MAX;
    out->maxZ = INT_MIN;

    list = (Piece *) calloc(
        MAX_PIECES,
        sizeof(Piece)
    );

    if (list == NULL) {
        fprintf(
            stderr,
            "ERROR: calloc(%d, sizeof(Piece)) failed\n",
            MAX_PIECES
        );
        return false;
    }

    count = getFortressPieces(
        list,
        MAX_PIECES,
        MC_VERSION,
        seed,
        chunkX,
        chunkZ
    );

    if (count <= 0) {
        free(list);
        return false;
    }

    if (count > MAX_PIECES) {
        fprintf(
            stderr,
            "ERROR: getFortressPieces returned %d pieces; "
            "MAX_PIECES=%d\n",
            count,
            MAX_PIECES
        );

        free(list);
        return false;
    }

    out->pieces = (PieceInfo *) malloc(
        (size_t) count * sizeof(PieceInfo)
    );

    if (out->pieces == NULL) {
        free(list);
        return false;
    }

    out->pieceCount = count;

    for (int i = 0; i < count; i++) {
        int x1 = list[i].bb0.x;
        int z1 = list[i].bb0.z;
        int x2 = list[i].bb1.x;
        int z2 = list[i].bb1.z;

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

    free(list);

    return true;
}


/* ============================================================
 * PIECE INTERSECTION
 * ============================================================ */

static bool piecesIntersect(
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

    if (ax2 < bx1) return false;
    if (bx2 < ax1) return false;
    if (az2 < bz1) return false;
    if (bz2 < az1) return false;

    return true;
}

static int countPairIntersections(
    const FortressInfo *a,
    const FortressInfo *b)
{
    int count = 0;

    for (int i = 0; i < a->pieceCount; i++) {
        for (int j = 0; j < b->pieceCount; j++) {
            if (piecesIntersect(
                    &a->pieces[i],
                    &b->pieces[j]))
            {
                count++;
            }
        }
    }

    return count;
}


/* ============================================================
 * DEBUG PIECE
 * ============================================================ */

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

    const PieceInfo *p = &f->pieces[index];

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


/* ============================================================
 * DEBUG FORTRESS
 * ============================================================ */

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
    pthread_mutex_lock(&state->printMutex);

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

    pthread_mutex_unlock(&state->printMutex);
}


/* ============================================================
 * CHECK F1
 * ============================================================ */

static bool checkF1(
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

    return left && right && *piece >= 0;
}


/* ============================================================
 * CHECK F2
 * ============================================================ */

static bool checkF2(
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


/* ============================================================
 * CHECK F3
 * ============================================================ */

static bool checkF3(
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


/* ============================================================
 * CHECK F4
 * ============================================================ */

static bool checkF4(
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


/* ============================================================
 * CHECK F5
 * ============================================================ */

static bool checkF5(
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


/* ============================================================
 * TEST ONE MOVED SEED
 *
 * CRITICAL:
 *
 * The five queried regions are TRANSLATED regions.
 *
 * For base region:
 *
 *     (REGION_X[i], REGION_Z[i])
 *
 * and movement:
 *
 *     (moveX, moveZ)
 *
 * the actual queried region is:
 *
 *     (REGION_X[i] + moveX,
 *      REGION_Z[i] + moveZ)
 *
 * ============================================================ */

static bool testSeed(
    Generator *g,
    SearchState *state,
    uint64_t movedSeed,
    int moveX,
    int moveZ,
    uint64_t baseSeed)
{
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
    }


    /* ========================================================
     * NETHER GENERATOR
     * ======================================================== */

    applySeed(
        g,
        DIM_NETHER,
        movedSeed
    );


    /* ========================================================
     * GET TRANSLATED STRUCTURE POSITIONS
     *
     * THIS IS THE IMPORTANT TRANSLATION FIX.
     * ======================================================== */

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
            goto reject;
        }


        /* ====================================================
         * IMPORTANT:
         *
         * getStructurePos() only gives the generation attempt.
         *
         * For MC 1.18+ Fortress, this must be followed by
         * isViableStructurePos().
         *
         * This is what rejects positions where a Bastion
         * occupies the structure location.
         *
         * pos.x / pos.z are BLOCK coordinates here.
         * ==================================================== */

        if (!isViableStructurePos(
                STRUCT_TYPE,
                g,
                positions[i].x,
                positions[i].z,
                0))
        {
            goto reject;
        }
    }


    /* ========================================================
     * F1
     * ======================================================== */

    if (!generateFortress(
            movedSeed,
            positions[0].x >> 4,
            positions[0].z >> 4,
            &forts[0]))
    {
        goto reject;
    }

    if (!checkF1(
            &forts[0],
            &pieceA[0]))
    {
        goto reject;
    }

    atomic_fetch_add_explicit(
        &state->fortressPassed[0],
        1,
        memory_order_relaxed
    );

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


    /* ========================================================
     * F2
     * ======================================================== */

    if (!generateFortress(
            movedSeed,
            positions[1].x >> 4,
            positions[1].z >> 4,
            &forts[1]))
    {
        goto reject;
    }

    if (!checkF2(
            &forts[1],
            &pieceA[1],
            &pieceB[1]))
    {
        goto reject;
    }

    atomic_fetch_add_explicit(
        &state->fortressPassed[1],
        1,
        memory_order_relaxed
    );

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


    /* ========================================================
     * F3
     * ======================================================== */

    if (!generateFortress(
            movedSeed,
            positions[2].x >> 4,
            positions[2].z >> 4,
            &forts[2]))
    {
        goto reject;
    }

    if (!checkF3(
            &forts[2],
            &pieceA[2],
            &pieceB[2]))
    {
        goto reject;
    }

    atomic_fetch_add_explicit(
        &state->fortressPassed[2],
        1,
        memory_order_relaxed
    );

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


    /* ========================================================
     * F4
     * ======================================================== */

    if (!generateFortress(
            movedSeed,
            positions[3].x >> 4,
            positions[3].z >> 4,
            &forts[3]))
    {
        goto reject;
    }

    if (!checkF4(
            &forts[3],
            &pieceA[3],
            &pieceB[3]))
    {
        goto reject;
    }

    atomic_fetch_add_explicit(
        &state->fortressPassed[3],
        1,
        memory_order_relaxed
    );

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


    /* ========================================================
     * F5
     * ======================================================== */

    if (!generateFortress(
            movedSeed,
            positions[4].x >> 4,
            positions[4].z >> 4,
            &forts[4]))
    {
        goto reject;
    }

    if (!checkF5(
            &forts[4],
            &pieceA[4],
            &pieceB[4]))
    {
        goto reject;
    }

    atomic_fetch_add_explicit(
        &state->fortressPassed[4],
        1,
        memory_order_relaxed
    );

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


    /* ========================================================
     * ALL FIVE PASSED
     * ======================================================== */

    atomic_fetch_add_explicit(
        &state->fastMatches,
        1,
        memory_order_relaxed
    );


    /* ========================================================
     * CONNECTIVITY
     * ======================================================== */

    {
        int totalIntersections = 0;
        bool connected = true;

        for (int i = 0; i < NUM_FORTS - 1; i++) {
            int intersections =
                countPairIntersections(
                    &forts[i],
                    &forts[i + 1]
                );

            totalIntersections += intersections;

            if (intersections == 0)
                connected = false;
        }

        if (connected) {
            atomic_fetch_add_explicit(
                &state->completeMatches,
                1,
                memory_order_relaxed
            );

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

                REGION_X[0], REGION_Z[0],
                translatedRegionX[0],
                translatedRegionZ[0],

                REGION_X[1], REGION_Z[1],
                translatedRegionX[1],
                translatedRegionZ[1],

                REGION_X[2], REGION_Z[2],
                translatedRegionX[2],
                translatedRegionZ[2],

                REGION_X[3], REGION_Z[3],
                translatedRegionX[3],
                translatedRegionZ[3],

                REGION_X[4], REGION_Z[4],
                translatedRegionX[4],
                translatedRegionZ[4]
            );

            fflush(stdout);

            pthread_mutex_unlock(
                &state->printMutex
            );
        }
    }


reject:

    for (int i = 0; i < NUM_FORTS; i++)
        freeFortress(&forts[i]);

    return false;
}


/* ============================================================
 * TRANSLATIONS
 * ============================================================ */

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

    list[count++] = (Translation) { 0, 0 };

    for (int d = 1; d <= maxDistance; d++) {
        for (int dx = -d; dx <= d; dx++) {
            int z = d - abs(dx);

            if (z == 0) {
                list[count++] =
                    (Translation) { dx, 0 };
            }
            else {
                list[count++] =
                    (Translation) { dx, -z };

                list[count++] =
                    (Translation) { dx, +z };
            }
        }
    }

    *outCount = count;

    return list;
}


/* ============================================================
 * ARGUMENTS
 * ============================================================ */

static void printUsage(const char *program)
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
    *threads = DEFAULT_THREADS;

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

        long v = strtol(
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

        *threads = (int) v;
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

            long f = strtol(
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

            debug->enabled[f - 1] = true;
            i++;
        }
    }

    return true;
}


/* ============================================================
 * PROGRESS
 * ============================================================ */

static void reportProgress(
    SearchState *state,
    size_t completed)
{
    size_t totalJobs =
        state->translationCount *
        RESULT_COUNT;

    size_t f[NUM_FORTS];

    for (int i = 0; i < NUM_FORTS; i++) {
        f[i] =
            atomic_load_explicit(
                &state->fortressPassed[i],
                memory_order_relaxed
            );
    }

    size_t matches =
        atomic_load_explicit(
            &state->completeMatches,
            memory_order_relaxed
        );

    pthread_mutex_lock(
        &state->printMutex
    );

    printf(
        "\rJobs: %zu/%zu (%.4f%%)"
        " | F1:%zu F2:%zu F3:%zu F4:%zu F5:%zu"
        " | chains:%zu       ",
        completed,
        totalJobs,
        totalJobs
            ? 100.0 *
              (double) completed /
              (double) totalJobs
            : 100.0,
        f[0],
        f[1],
        f[2],
        f[3],
        f[4],
        matches
    );

    fflush(stdout);

    pthread_mutex_unlock(
        &state->printMutex
    );
}


/* ============================================================
 * WORKER
 * ============================================================ */

static void *worker(void *arg)
{
    WorkerArgs *wa =
        (WorkerArgs *) arg;

    SearchState *state =
        wa->state;

    Generator g;

    setupGenerator(
        &g,
        MC_VERSION,
        0
    );

    size_t totalJobs =
        state->translationCount *
        RESULT_COUNT;

    while (true) {
        size_t job =
            atomic_fetch_add_explicit(
                &state->nextJob,
                1,
                memory_order_relaxed
            );

        if (job >= totalJobs)
            break;

        size_t translationIndex =
            job / RESULT_COUNT;

        size_t resultIndex =
            job % RESULT_COUNT;

        int moveX =
            state->translations[
                translationIndex
            ].x;

        int moveZ =
            state->translations[
                translationIndex
            ].z;

        uint64_t baseSeed =
            results[resultIndex];

        uint64_t movedSeed =
            moveStructureLocal(
                baseSeed,
                moveX,
                moveZ
            );

        (void) testSeed(
            &g,
            state,
            movedSeed,
            moveX,
            moveZ,
            baseSeed
        );

        size_t completed =
            atomic_fetch_add_explicit(
                &state->jobsCompleted,
                1,
                memory_order_relaxed
            ) + 1;

        if (completed % REPORT_EVERY == 0) {
            reportProgress(
                state,
                completed
            );
        }
    }

    return NULL;
}


/* ============================================================
 * MAIN
 * ============================================================ */

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


    /* ========================================================
     * TRANSLATIONS
     * ======================================================== */

    Translation *translations;
    size_t translationCount;

    translations =
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


    /* ========================================================
     * SEARCH STATE
     * ======================================================== */

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

    for (int i = 0; i < NUM_FORTS; i++) {
        atomic_init(
            &state.fortressPassed[i],
            0
        );
    }

    atomic_init(
        &state.fastMatches,
        0
    );

    atomic_init(
        &state.completeMatches,
        0
    );


    /* ========================================================
     * PRINT MUTEX
     * ======================================================== */

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


    /* ========================================================
     * THREAD ALLOCATION
     * ======================================================== */

    pthread_t *threads =
        (pthread_t *) malloc(
            (size_t) threadCount *
            sizeof(pthread_t)
        );

    WorkerArgs *args =
        (WorkerArgs *) malloc(
            (size_t) threadCount *
            sizeof(WorkerArgs)
        );

    if (threads == NULL ||
        args == NULL)
    {
        fprintf(
            stderr,
            "ERROR: thread allocation failed\n"
        );

        free(threads);
        free(args);
        free(translations);

        pthread_mutex_destroy(
            &state.printMutex
        );

        return 1;
    }


    /* ========================================================
     * STARTUP INFORMATION
     * ======================================================== */

    size_t totalJobs =
        translationCount *
        RESULT_COUNT;

    printf(
        "============================================================\n"
        "FORTRESS CHAIN SEARCH\n"
        "============================================================\n"
        "Seeds                 : %zu\n"
        "Max movement distance : %d\n"
        "Translations          : %zu\n"
        "Jobs                  : %zu\n"
        "Threads               : %d\n"
        "X reach               : %d\n"
        "Z reach               : %d\n"
        "Piece radius          : %d\n"
        "Search order          : F1 -> F2 -> F3 -> F4 -> F5\n"
        "Viability check       : isViableStructurePos()\n"
        "Region translation    : BASE + MOVEMENT\n",
        RESULT_COUNT,
        MAX_DISTANCE,
        translationCount,
        totalJobs,
        threadCount,
        X_REACH,
        Z_REACH,
        PIECE_RADIUS
    );

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


    /* ========================================================
     * CREATE WORKER THREADS
     * ======================================================== */

    int created = 0;

    for (int i = 0; i < threadCount; i++) {
        args[i].state =
            &state;

        args[i].threadId =
            i;

        int rc =
            pthread_create(
                &threads[i],
                NULL,
                worker,
                &args[i]
            );

        if (rc != 0) {
            fprintf(
                stderr,
                "ERROR: pthread_create failed "
                "for thread %d: %s\n",
                i,
                strerror(rc)
            );

            break;
        }

        created++;
    }


    /* ========================================================
     * WAIT FOR THREADS
     * ======================================================== */

    for (int i = 0; i < created; i++) {
        pthread_join(
            threads[i],
            NULL
        );
    }


    /* ========================================================
     * FINAL RESULTS
     * ======================================================== */

    printf("\n\n");

    printf(
        "============================================================\n"
        "SEARCH COMPLETE\n"
        "============================================================\n"
        "Jobs completed : %zu / %zu\n",
        atomic_load_explicit(
            &state.jobsCompleted,
            memory_order_relaxed
        ),
        totalJobs
    );

    for (int i = 0; i < NUM_FORTS; i++) {
        printf(
            "F%d conditional passes : %zu\n",
            i + 1,
            atomic_load_explicit(
                &state.fortressPassed[i],
                memory_order_relaxed
            )
        );
    }

    printf(
        "Fast matches    : %zu\n"
        "5/5 chains      : %zu\n"
        "============================================================\n",
        atomic_load_explicit(
            &state.fastMatches,
            memory_order_relaxed
        ),
        atomic_load_explicit(
            &state.completeMatches,
            memory_order_relaxed
        )
    );


    /* ========================================================
     * CLEANUP
     * ======================================================== */

    free(args);
    free(threads);
    free(translations);

    pthread_mutex_destroy(
        &state.printMutex
    );

    return 0;
}

