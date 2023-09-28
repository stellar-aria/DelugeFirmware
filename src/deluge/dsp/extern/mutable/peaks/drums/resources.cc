// Copyright 2013 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Resources definitions.
//
// Automatically generated with:
// make resources

#include "resources.h"

namespace peaks {

static const char str_dummy[] = "dummy";

const char* string_table[] = {
    str_dummy,
};

const uint16_t lut_raised_cosine[] = {
    0,     2,     9,     22,    39,    61,    88,    120,   157,   199,   246,   298,   354,   416,   482,   553,
    629,   710,   796,   886,   982,   1082,  1186,  1296,  1410,  1530,  1653,  1782,  1915,  2053,  2195,  2342,
    2494,  2650,  2811,  2976,  3146,  3320,  3498,  3681,  3869,  4060,  4256,  4457,  4661,  4870,  5083,  5300,
    5522,  5747,  5977,  6210,  6448,  6689,  6935,  7184,  7437,  7694,  7955,  8220,  8488,  8760,  9035,  9314,
    9597,  9883,  10172, 10465, 10762, 11061, 11364, 11670, 11980, 12292, 12607, 12926, 13247, 13572, 13899, 14229,
    14562, 14898, 15236, 15578, 15921, 16267, 16616, 16967, 17321, 17676, 18034, 18395, 18757, 19122, 19488, 19857,
    20227, 20600, 20974, 21350, 21728, 22107, 22488, 22871, 23255, 23641, 24027, 24416, 24805, 25196, 25588, 25980,
    26374, 26769, 27165, 27562, 27959, 28357, 28756, 29155, 29555, 29956, 30356, 30758, 31159, 31561, 31963, 32365,
    32767, 33169, 33571, 33973, 34375, 34776, 35178, 35578, 35979, 36379, 36778, 37177, 37575, 37972, 38369, 38765,
    39160, 39554, 39946, 40338, 40729, 41118, 41507, 41893, 42279, 42663, 43046, 43427, 43806, 44184, 44560, 44934,
    45307, 45677, 46046, 46412, 46777, 47139, 47500, 47858, 48213, 48567, 48918, 49267, 49613, 49956, 50298, 50636,
    50972, 51305, 51635, 51962, 52287, 52608, 52927, 53242, 53554, 53864, 54170, 54473, 54772, 55069, 55362, 55651,
    55937, 56220, 56499, 56774, 57046, 57314, 57579, 57840, 58097, 58350, 58599, 58845, 59086, 59324, 59557, 59787,
    60012, 60234, 60451, 60664, 60873, 61077, 61278, 61474, 61665, 61853, 62036, 62214, 62388, 62558, 62723, 62884,
    63040, 63192, 63339, 63481, 63619, 63752, 63881, 64004, 64124, 64238, 64348, 64452, 64552, 64648, 64738, 64824,
    64905, 64981, 65052, 65118, 65180, 65236, 65288, 65335, 65377, 65414, 65446, 65473, 65495, 65512, 65525, 65532,
    65532,
};
const uint16_t lut_svf_cutoff[] = {
    35,    37,    39,    41,    44,    46,    49,    52,    55,    58,    62,    66,    70,    74,    78,    83,
    88,    93,    99,    105,   111,   117,   124,   132,   140,   148,   157,   166,   176,   187,   198,   210,
    222,   235,   249,   264,   280,   297,   314,   333,   353,   374,   396,   420,   445,   471,   499,   529,
    561,   594,   629,   667,   706,   748,   793,   840,   890,   943,   999,   1059,  1122,  1188,  1259,  1334,
    1413,  1497,  1586,  1681,  1781,  1886,  1999,  2117,  2243,  2377,  2518,  2668,  2826,  2994,  3172,  3361,
    3560,  3772,  3996,  4233,  4485,  4751,  5033,  5332,  5648,  5983,  6337,  6713,  7111,  7532,  7978,  8449,
    8949,  9477,  10037, 10628, 11254, 11916, 12616, 13356, 14138, 14964, 15837, 16758, 17730, 18756, 19837, 20975,
    22174, 23435, 24761, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078, 25078,
    25078,
};
const uint16_t lut_svf_damp[] = {
    65534, 49166, 46069, 43993, 42386, 41058, 39917, 38910, 38007, 37184, 36427, 35726, 35070, 34454, 33873, 33322,
    32798, 32299, 31820, 31361, 30920, 30496, 30086, 29690, 29306, 28935, 28574, 28224, 27883, 27551, 27228, 26912,
    26605, 26304, 26010, 25723, 25441, 25166, 24896, 24631, 24371, 24116, 23866, 23620, 23379, 23141, 22908, 22678,
    22452, 22229, 22010, 21794, 21581, 21371, 21164, 20960, 20759, 20560, 20365, 20171, 19980, 19791, 19605, 19421,
    19239, 19059, 18882, 18706, 18532, 18360, 18190, 18022, 17856, 17691, 17528, 17367, 17207, 17049, 16892, 16737,
    16583, 16431, 16280, 16131, 15982, 15836, 15690, 15546, 15403, 15261, 15120, 14981, 14843, 14705, 14569, 14434,
    14300, 14167, 14036, 13905, 13775, 13646, 13518, 13391, 13265, 13140, 13015, 12892, 12769, 12648, 12527, 12407,
    12287, 12169, 12051, 11934, 11818, 11703, 11588, 11474, 11361, 11249, 11137, 11026, 10915, 10805, 10696, 10588,
    10480, 10373, 10266, 10160, 10055, 9950,  9846,  9742,  9639,  9537,  9435,  9333,  9233,  9132,  9033,  8933,
    8835,  8737,  8639,  8542,  8445,  8349,  8253,  8158,  8063,  7969,  7875,  7782,  7689,  7596,  7504,  7413,
    7321,  7231,  7140,  7050,  6961,  6872,  6783,  6695,  6607,  6519,  6432,  6346,  6259,  6173,  6088,  6003,
    5918,  5833,  5749,  5665,  5582,  5499,  5416,  5334,  5251,  5170,  5088,  5007,  4926,  4846,  4766,  4686,
    4607,  4527,  4449,  4370,  4292,  4214,  4136,  4059,  3982,  3905,  3828,  3752,  3676,  3601,  3525,  3450,
    3375,  3301,  3226,  3152,  3078,  3005,  2932,  2859,  2786,  2713,  2641,  2569,  2497,  2426,  2355,  2284,
    2213,  2142,  2072,  2002,  1932,  1862,  1793,  1724,  1655,  1586,  1518,  1449,  1381,  1313,  1246,  1178,
    1111,  1044,  977,   911,   844,   778,   712,   647,   581,   516,   450,   385,   321,   256,   192,   127,
    63,
};
const uint16_t lut_svf_scale[] = {
    32767, 28381, 27473, 26846, 26352, 25936, 25573, 25248, 24953, 24682, 24429, 24193, 23970, 23759, 23557, 23365,
    23181, 23003, 22832, 22667, 22507, 22352, 22201, 22055, 21912, 21772, 21636, 21503, 21373, 21245, 21120, 20998,
    20877, 20759, 20643, 20528, 20416, 20305, 20196, 20088, 19982, 19877, 19774, 19672, 19571, 19471, 19373, 19275,
    19179, 19083, 18989, 18896, 18803, 18712, 18621, 18531, 18442, 18353, 18266, 18179, 18092, 18007, 17922, 17837,
    17754, 17671, 17588, 17506, 17424, 17343, 17263, 17183, 17103, 17024, 16946, 16868, 16790, 16713, 16636, 16559,
    16483, 16407, 16331, 16256, 16181, 16107, 16033, 15959, 15885, 15812, 15739, 15666, 15594, 15522, 15450, 15378,
    15306, 15235, 15164, 15093, 15022, 14952, 14882, 14812, 14742, 14672, 14602, 14533, 14464, 14395, 14326, 14257,
    14188, 14120, 14051, 13983, 13915, 13847, 13779, 13711, 13643, 13575, 13508, 13440, 13372, 13305, 13238, 13170,
    13103, 13036, 12969, 12902, 12835, 12768, 12701, 12634, 12567, 12500, 12433, 12366, 12299, 12232, 12165, 12098,
    12031, 11964, 11897, 11830, 11762, 11695, 11628, 11561, 11493, 11426, 11359, 11291, 11223, 11156, 11088, 11020,
    10952, 10884, 10816, 10747, 10679, 10610, 10542, 10473, 10404, 10335, 10266, 10196, 10127, 10057, 9987,  9917,
    9846,  9776,  9705,  9634,  9563,  9491,  9420,  9348,  9276,  9203,  9130,  9057,  8984,  8910,  8836,  8762,
    8687,  8613,  8537,  8461,  8385,  8309,  8232,  8155,  8077,  7998,  7920,  7841,  7761,  7680,  7600,  7518,
    7436,  7354,  7270,  7187,  7102,  7017,  6931,  6844,  6756,  6668,  6578,  6488,  6397,  6305,  6211,  6117,
    6021,  5925,  5827,  5727,  5626,  5524,  5420,  5315,  5207,  5098,  4987,  4873,  4758,  4639,  4518,  4394,
    4267,  4137,  4002,  3864,  3720,  3571,  3417,  3255,  3086,  2907,  2717,  2514,  2293,  2049,  1774,  1447,
    1022,
};
const uint16_t lut_env_expo[] = {
    0,     1035,  2054,  3057,  4045,  5018,  5975,  6918,  7846,  8760,  9659,  10545, 11416, 12275, 13120, 13952,
    14771, 15577, 16371, 17152, 17921, 18679, 19425, 20159, 20881, 21593, 22294, 22983, 23662, 24331, 24989, 25637,
    26274, 26902, 27520, 28129, 28728, 29318, 29899, 30471, 31034, 31588, 32133, 32670, 33199, 33720, 34232, 34737,
    35233, 35722, 36204, 36678, 37145, 37604, 38056, 38502, 38940, 39371, 39796, 40215, 40626, 41032, 41431, 41824,
    42211, 42592, 42967, 43336, 43699, 44057, 44409, 44756, 45097, 45434, 45764, 46090, 46411, 46727, 47037, 47344,
    47645, 47941, 48233, 48521, 48804, 49083, 49357, 49627, 49893, 50155, 50412, 50666, 50916, 51162, 51404, 51642,
    51877, 52108, 52335, 52559, 52780, 52997, 53210, 53421, 53628, 53831, 54032, 54230, 54424, 54616, 54804, 54990,
    55173, 55353, 55530, 55704, 55876, 56045, 56211, 56375, 56536, 56695, 56851, 57005, 57157, 57306, 57453, 57597,
    57740, 57880, 58018, 58153, 58287, 58419, 58548, 58676, 58801, 58925, 59047, 59167, 59285, 59401, 59515, 59628,
    59739, 59848, 59955, 60061, 60165, 60267, 60368, 60468, 60566, 60662, 60757, 60850, 60942, 61032, 61121, 61209,
    61295, 61380, 61464, 61546, 61628, 61707, 61786, 61863, 61939, 62014, 62088, 62161, 62233, 62303, 62372, 62441,
    62508, 62574, 62639, 62703, 62767, 62829, 62890, 62950, 63010, 63068, 63125, 63182, 63238, 63293, 63347, 63400,
    63452, 63504, 63554, 63604, 63654, 63702, 63750, 63797, 63843, 63888, 63933, 63977, 64021, 64063, 64105, 64147,
    64188, 64228, 64267, 64306, 64344, 64382, 64419, 64456, 64492, 64527, 64562, 64596, 64630, 64664, 64696, 64729,
    64760, 64792, 64822, 64853, 64883, 64912, 64941, 64969, 64997, 65025, 65052, 65079, 65105, 65131, 65157, 65182,
    65206, 65231, 65255, 65278, 65302, 65324, 65347, 65369, 65391, 65412, 65434, 65454, 65475, 65495, 65515, 65535,
    65535,
};
const uint32_t lut_env_increments[] = {
    178956970, 162203921, 147263779, 133914742, 121965179, 111249129, 101622525, 92960022, 85152324, 78103929, 71731214,
    65960813,  60728233,  55976673,  51656020,  47721976,  44135321,  40861270,  37868923, 35130786, 32622357, 30321772,
    28209484,  26268003,  24481648,  22836346,  21319451,  19919582,  18626484,  17430905, 16324491, 15299685, 14349647,
    13468178,  12649652,  11888959,  11181454,  10522909,  9909470,   9337624,   8804164,  8306157,  7840925,  7406012,
    6999170,   6618338,   6261623,   5927288,   5613733,   5319488,   5043200,   4783621,  4539601,  4310076,  4094068,
    3890668,   3699040,   3518407,   3348049,   3187303,   3035548,   2892213,   2756766,  2628711,  2507588,  2392971,
    2284460,   2181685,   2084301,   1991984,   1904435,   1821372,   1742534,   1667676,  1596567,  1528995,  1464759,
    1403670,   1345553,   1290243,   1237585,   1187435,   1139655,   1094119,   1050706,  1009303,  969803,   932108,
    896122,    861758,    828932,    797565,    767584,    738918,    711503,    685274,   660174,   636148,   613143,
    591109,    569999,    549770,    530379,    511787,    493956,    476850,    460436,   444683,   429558,   415035,
    401085,    387683,    374804,    362424,    350523,    339078,    328069,    317479,   307287,   297478,   288035,
    278942,    270184,    261748,    253620,    245786,    238236,    230956,    223936,   217166,   210635,   204334,
    198253,    192383,    186717,    181245,    175961,    170858,    165927,    161162,   156558,   152107,   147805,
    143644,    139621,    135729,    131964,    128321,    124796,    121384,    118081,   114883,   111787,   108788,
    105883,    103069,    100343,    97700,     95139,     92657,     90250,     87917,    85654,    83459,    81330,
    79265,     77261,     75316,     73428,     71596,     69817,     68090,     66413,    64784,    63202,    61666,
    60173,     58722,     57312,     55942,     54610,     53315,     52056,     50832,    49641,    48484,    47357,
    46262,     45196,     44159,     43149,     42167,     41211,     40280,     39374,    38492,    37632,    36796,
    35981,     35187,     34414,     33660,     32926,     32211,     31513,     30834,    30172,    29526,    28896,
    28282,     27684,     27100,     26531,     25975,     25434,     24905,     24390,    23886,    23395,    22916,
    22449,     21992,     21546,     21111,     20687,     20272,     19867,     19471,    19085,    18708,    18339,
    17979,     17627,     17283,     16947,     16619,     16298,     15985,     15678,    15378,    15085,    14799,
    14519,     14245,     13977,     13716,     13459,     13209,     12964,     12724,    12490,    12260,    12036,
    11816,     11601,     11390,     11184,
};
const uint32_t lut_oscillator_increments[] = {
    594570139,  598878640,  603218361,  607589530,  611992374,  616427123,  620894008,  625393262,  629925120,
    634489817,  639087591,  643718683,  648383334,  653081787,  657814287,  662581081,  667382416,  672218544,
    677089717,  681996188,  686938214,  691916051,  696929960,  701980202,  707067040,  712190739,  717351567,
    722549792,  727785686,  733059521,  738371572,  743722117,  749111434,  754539804,  760007511,  765514839,
    771062075,  776649508,  782277431,  787946136,  793655918,  799407076,  805199909,  811034720,  816911812,
    822831491,  828794068,  834799851,  840849155,  846942294,  853079587,  859261354,  865487916,  871759598,
    878076727,  884439633,  890848647,  897304104,  903806339,  910355693,  916952505,  923597121,  930289887,
    937031151,  943821265,  950660583,  957549461,  964488259,  971477339,  978517064,  985607802,  992749922,
    999943798,  1007189803, 1014488315, 1021839716, 1029244387, 1036702717, 1044215092, 1051781905, 1059403550,
    1067080425, 1074812930, 1082601467, 1090446444, 1098348268, 1106307352, 1114324111, 1122398963, 1130532329,
    1138724632, 1146976300, 1155287763, 1163659455, 1172091811, 1180585271, 1189140279,
};
const int16_t wav_overdrive[] = {
    -32767, -32767, -32767, -32767, -32767, -32767, -32767, -32767, -32766, -32766, -32766, -32766, -32766, -32766,
    -32766, -32766, -32766, -32766, -32766, -32766, -32766, -32765, -32765, -32765, -32765, -32765, -32765, -32765,
    -32765, -32765, -32765, -32765, -32764, -32764, -32764, -32764, -32764, -32764, -32764, -32764, -32763, -32763,
    -32763, -32763, -32763, -32763, -32763, -32763, -32762, -32762, -32762, -32762, -32762, -32762, -32761, -32761,
    -32761, -32761, -32761, -32761, -32760, -32760, -32760, -32760, -32760, -32759, -32759, -32759, -32759, -32759,
    -32758, -32758, -32758, -32758, -32757, -32757, -32757, -32757, -32756, -32756, -32756, -32756, -32755, -32755,
    -32755, -32754, -32754, -32754, -32753, -32753, -32753, -32752, -32752, -32752, -32751, -32751, -32751, -32750,
    -32750, -32749, -32749, -32749, -32748, -32748, -32747, -32747, -32746, -32746, -32745, -32745, -32744, -32744,
    -32743, -32743, -32742, -32742, -32741, -32741, -32740, -32740, -32739, -32738, -32738, -32737, -32736, -32736,
    -32735, -32734, -32734, -32733, -32732, -32732, -32731, -32730, -32729, -32728, -32728, -32727, -32726, -32725,
    -32724, -32723, -32722, -32721, -32720, -32719, -32718, -32717, -32716, -32715, -32714, -32713, -32712, -32711,
    -32710, -32709, -32707, -32706, -32705, -32704, -32702, -32701, -32700, -32698, -32697, -32695, -32694, -32692,
    -32691, -32689, -32688, -32686, -32684, -32683, -32681, -32679, -32678, -32676, -32674, -32672, -32670, -32668,
    -32666, -32664, -32662, -32660, -32658, -32655, -32653, -32651, -32649, -32646, -32644, -32641, -32639, -32636,
    -32633, -32631, -32628, -32625, -32622, -32619, -32617, -32614, -32610, -32607, -32604, -32601, -32597, -32594,
    -32591, -32587, -32584, -32580, -32576, -32572, -32568, -32564, -32560, -32556, -32552, -32548, -32543, -32539,
    -32534, -32530, -32525, -32520, -32515, -32510, -32505, -32500, -32495, -32489, -32484, -32478, -32473, -32467,
    -32461, -32455, -32448, -32442, -32436, -32429, -32422, -32416, -32409, -32402, -32394, -32387, -32380, -32372,
    -32364, -32356, -32348, -32340, -32331, -32323, -32314, -32305, -32296, -32287, -32277, -32268, -32258, -32248,
    -32237, -32227, -32216, -32206, -32195, -32183, -32172, -32160, -32148, -32136, -32124, -32111, -32098, -32085,
    -32072, -32058, -32044, -32030, -32016, -32001, -31986, -31971, -31955, -31939, -31923, -31907, -31890, -31873,
    -31855, -31838, -31819, -31801, -31782, -31763, -31743, -31723, -31703, -31682, -31661, -31640, -31618, -31596,
    -31573, -31550, -31526, -31502, -31478, -31453, -31427, -31401, -31375, -31348, -31320, -31293, -31264, -31235,
    -31205, -31175, -31145, -31113, -31082, -31049, -31016, -30983, -30948, -30913, -30878, -30842, -30805, -30767,
    -30729, -30690, -30650, -30610, -30569, -30527, -30484, -30440, -30396, -30351, -30305, -30258, -30211, -30162,
    -30113, -30063, -30012, -29959, -29906, -29853, -29798, -29742, -29685, -29627, -29568, -29508, -29447, -29385,
    -29321, -29257, -29191, -29125, -29057, -28988, -28918, -28846, -28774, -28700, -28625, -28548, -28470, -28391,
    -28311, -28229, -28146, -28061, -27975, -27887, -27798, -27708, -27616, -27522, -27427, -27331, -27232, -27132,
    -27031, -26928, -26823, -26717, -26609, -26499, -26387, -26274, -26158, -26041, -25923, -25802, -25679, -25555,
    -25428, -25300, -25170, -25038, -24904, -24767, -24629, -24489, -24346, -24202, -24056, -23907, -23756, -23603,
    -23448, -23291, -23131, -22970, -22806, -22640, -22471, -22301, -22128, -21952, -21775, -21595, -21413, -21228,
    -21041, -20852, -20660, -20466, -20270, -20071, -19870, -19666, -19460, -19252, -19041, -18828, -18613, -18395,
    -18174, -17951, -17726, -17499, -17269, -17036, -16802, -16565, -16325, -16083, -15839, -15593, -15344, -15093,
    -14840, -14584, -14327, -14067, -13805, -13540, -13274, -13005, -12735, -12462, -12187, -11910, -11632, -11351,
    -11068, -10784, -10498, -10210, -9920,  -9628,  -9335,  -9040,  -8744,  -8446,  -8146,  -7845,  -7543,  -7239,
    -6934,  -6628,  -6320,  -6012,  -5702,  -5391,  -5079,  -4766,  -4453,  -4138,  -3823,  -3507,  -3190,  -2873,
    -2555,  -2237,  -1918,  -1599,  -1279,  -960,   -640,   -320,   0,      320,    640,    960,    1279,   1599,
    1918,   2237,   2555,   2873,   3190,   3507,   3823,   4138,   4453,   4766,   5079,   5391,   5702,   6012,
    6320,   6628,   6934,   7239,   7543,   7845,   8146,   8446,   8744,   9040,   9335,   9628,   9920,   10210,
    10498,  10784,  11068,  11351,  11632,  11910,  12187,  12462,  12735,  13005,  13274,  13540,  13805,  14067,
    14327,  14584,  14840,  15093,  15344,  15593,  15839,  16083,  16325,  16565,  16802,  17036,  17269,  17499,
    17726,  17951,  18174,  18395,  18613,  18828,  19041,  19252,  19460,  19666,  19870,  20071,  20270,  20466,
    20660,  20852,  21041,  21228,  21413,  21595,  21775,  21952,  22128,  22301,  22471,  22640,  22806,  22970,
    23131,  23291,  23448,  23603,  23756,  23907,  24056,  24202,  24346,  24489,  24629,  24767,  24904,  25038,
    25170,  25300,  25428,  25555,  25679,  25802,  25923,  26041,  26158,  26274,  26387,  26499,  26609,  26717,
    26823,  26928,  27031,  27132,  27232,  27331,  27427,  27522,  27616,  27708,  27798,  27887,  27975,  28061,
    28146,  28229,  28311,  28391,  28470,  28548,  28625,  28700,  28774,  28846,  28918,  28988,  29057,  29125,
    29191,  29257,  29321,  29385,  29447,  29508,  29568,  29627,  29685,  29742,  29798,  29853,  29906,  29959,
    30012,  30063,  30113,  30162,  30211,  30258,  30305,  30351,  30396,  30440,  30484,  30527,  30569,  30610,
    30650,  30690,  30729,  30767,  30805,  30842,  30878,  30913,  30948,  30983,  31016,  31049,  31082,  31113,
    31145,  31175,  31205,  31235,  31264,  31293,  31320,  31348,  31375,  31401,  31427,  31453,  31478,  31502,
    31526,  31550,  31573,  31596,  31618,  31640,  31661,  31682,  31703,  31723,  31743,  31763,  31782,  31801,
    31819,  31838,  31855,  31873,  31890,  31907,  31923,  31939,  31955,  31971,  31986,  32001,  32016,  32030,
    32044,  32058,  32072,  32085,  32098,  32111,  32124,  32136,  32148,  32160,  32172,  32183,  32195,  32206,
    32216,  32227,  32237,  32248,  32258,  32268,  32277,  32287,  32296,  32305,  32314,  32323,  32331,  32340,
    32348,  32356,  32364,  32372,  32380,  32387,  32394,  32402,  32409,  32416,  32422,  32429,  32436,  32442,
    32448,  32455,  32461,  32467,  32473,  32478,  32484,  32489,  32495,  32500,  32505,  32510,  32515,  32520,
    32525,  32530,  32534,  32539,  32543,  32548,  32552,  32556,  32560,  32564,  32568,  32572,  32576,  32580,
    32584,  32587,  32591,  32594,  32597,  32601,  32604,  32607,  32610,  32614,  32617,  32619,  32622,  32625,
    32628,  32631,  32633,  32636,  32639,  32641,  32644,  32646,  32649,  32651,  32653,  32655,  32658,  32660,
    32662,  32664,  32666,  32668,  32670,  32672,  32674,  32676,  32678,  32679,  32681,  32683,  32684,  32686,
    32688,  32689,  32691,  32692,  32694,  32695,  32697,  32698,  32700,  32701,  32702,  32704,  32705,  32706,
    32707,  32709,  32710,  32711,  32712,  32713,  32714,  32715,  32716,  32717,  32718,  32719,  32720,  32721,
    32722,  32723,  32724,  32725,  32726,  32727,  32728,  32728,  32729,  32730,  32731,  32732,  32732,  32733,
    32734,  32734,  32735,  32736,  32736,  32737,  32738,  32738,  32739,  32740,  32740,  32741,  32741,  32742,
    32742,  32743,  32743,  32744,  32744,  32745,  32745,  32746,  32746,  32747,  32747,  32748,  32748,  32749,
    32749,  32749,  32750,  32750,  32751,  32751,  32751,  32752,  32752,  32752,  32753,  32753,  32753,  32754,
    32754,  32754,  32755,  32755,  32755,  32756,  32756,  32756,  32756,  32757,  32757,  32757,  32757,  32758,
    32758,  32758,  32758,  32759,  32759,  32759,  32759,  32759,  32760,  32760,  32760,  32760,  32760,  32761,
    32761,  32761,  32761,  32761,  32761,  32762,  32762,  32762,  32762,  32762,  32762,  32763,  32763,  32763,
    32763,  32763,  32763,  32763,  32763,  32764,  32764,  32764,  32764,  32764,  32764,  32764,  32764,  32765,
    32765,  32765,  32765,  32765,  32765,  32765,  32765,  32765,  32765,  32765,  32766,  32766,  32766,  32766,
    32766,  32766,  32766,  32766,  32766,  32766,  32766,  32766,  32766,  32767,  32767,  32767,  32767,  32767,
    32767,  32767,  32767,
};

} // namespace peaks
