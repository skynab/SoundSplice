#pragma once

#include <array>

#include "model/Effects.h"

namespace soundsplice::model
{
/** The 31-band graphic EQ's gains, stored as flat fields (a parameter each),
    as an array in band order (20 Hz first), and back. */
inline std::array<float, 31> graphicEq31Gains(const GraphicEq31Settings& eq)
{
    std::array<float, 31> gains {};
    gains[0] = eq.band1;
    gains[1] = eq.band2;
    gains[2] = eq.band3;
    gains[3] = eq.band4;
    gains[4] = eq.band5;
    gains[5] = eq.band6;
    gains[6] = eq.band7;
    gains[7] = eq.band8;
    gains[8] = eq.band9;
    gains[9] = eq.band10;
    gains[10] = eq.band11;
    gains[11] = eq.band12;
    gains[12] = eq.band13;
    gains[13] = eq.band14;
    gains[14] = eq.band15;
    gains[15] = eq.band16;
    gains[16] = eq.band17;
    gains[17] = eq.band18;
    gains[18] = eq.band19;
    gains[19] = eq.band20;
    gains[20] = eq.band21;
    gains[21] = eq.band22;
    gains[22] = eq.band23;
    gains[23] = eq.band24;
    gains[24] = eq.band25;
    gains[25] = eq.band26;
    gains[26] = eq.band27;
    gains[27] = eq.band28;
    gains[28] = eq.band29;
    gains[29] = eq.band30;
    gains[30] = eq.band31;
    return gains;
}

inline void setGraphicEq31Gains(GraphicEq31Settings& eq, const std::array<float, 31>& gains)
{
    eq.band1 = gains[0];
    eq.band2 = gains[1];
    eq.band3 = gains[2];
    eq.band4 = gains[3];
    eq.band5 = gains[4];
    eq.band6 = gains[5];
    eq.band7 = gains[6];
    eq.band8 = gains[7];
    eq.band9 = gains[8];
    eq.band10 = gains[9];
    eq.band11 = gains[10];
    eq.band12 = gains[11];
    eq.band13 = gains[12];
    eq.band14 = gains[13];
    eq.band15 = gains[14];
    eq.band16 = gains[15];
    eq.band17 = gains[16];
    eq.band18 = gains[17];
    eq.band19 = gains[18];
    eq.band20 = gains[19];
    eq.band21 = gains[20];
    eq.band22 = gains[21];
    eq.band23 = gains[22];
    eq.band24 = gains[23];
    eq.band25 = gains[24];
    eq.band26 = gains[25];
    eq.band27 = gains[26];
    eq.band28 = gains[27];
    eq.band29 = gains[28];
    eq.band30 = gains[29];
    eq.band31 = gains[30];
}

} // namespace soundsplice::model
