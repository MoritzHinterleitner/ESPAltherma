#include "labeldef.h"
//  ROTEX protocol S - ORIGINAL labels + RAW byte dump for reverse engineering.
//  Based on def/PROTOCOL_S_ROTEX.h (confirmed on RDLQ014AA6V3).
//  Every raw label "Rxx_yy" = register 0xxx, data byte yy, published as unsigned 0..255 (conv 151).
//  Protocol S reply: [regID][16 data bytes][CRC] for 0x53-0x55, [regID][2 data bytes][CRC] for 0x56.
//  All queries are READ requests - same ones the original file already sends. No new registers yet.
//  2026-11-11: removed the 29 raw bytes that were 0 for 6 days (11-05..11-11, 6012 samples).
//  Full 40-byte version: espaltherma/firmware/PROTOCOL_S_ROTEX_RAW.h.before-trim

LabelDef labelDefs[] = {
    // ---------- original ROTEX labels (unchanged) ----------
    {0x53, 0, 200, 1, -1, "Circulation pump"},
    {0x53, 3, 200, 1, -1, "External heater?"},
    {0x53, 5, 200, 1, -1, "Priority to domestic water"},
    {0x53, 6, 200, 1, -1, "Burner inhibit from solaris"},
    {0x54, 0, 153, 2, -1, "Refrig. Temp. liquid side(C)"},
    {0x54, 2, 153, 2, -1, "Inlet water temp.(C)"},
    {0x54, 4, 153, 2, -1, "Outlet Water Temp.(C)"},
    {0x54, 6, 153, 2, -1, "D(C)"},
    {0x54, 8, 153, 2, -1, "DHW tank temp.(C)"},
    {0x54, 10, 103, 2, -1, "F(C)"},
    {0x54, 12, 101, 1, -1, "Delta-Tr(deg)"},
    {0x54, 13, 151, 1, -1, "R/C Setpoint(C)"},
    {0x55, 0, 201, 1, -1, "Operation Mode"},
    {0x55, 1, 204, 1, -1, "Error Code"},
    {0x55, 2, 204, 1, -1, "Thermo Off Error"},
    {0x55, 3, 204, 1, -1, "Warning Code"},
    {0x55, 4, 204, 1, -1, "Caution Code"},
    {0x56, 0, 103, 2, -1, "????"},

    // ---------- RAW 0x53: flags - full byte values (bits may be packed!) ----------
    {0x53, 0, 151, 1, -1, "R53_00"},
    {0x53, 3, 151, 1, -1, "R53_03"},
    {0x53, 5, 151, 1, -1, "R53_05"},
    {0x53, 6, 151, 1, -1, "R53_06"},

    // ---------- RAW 0x54: bytes 10-15 (F(C), Delta-Tr, setpoint, 2 unused) ----------
    {0x54, 10, 151, 1, -1, "R54_10"},
    {0x54, 12, 151, 1, -1, "R54_12"},

    // ---------- RAW 0x55: mode / error codes + 11 unused bytes ----------
    {0x55, 0, 151, 1, -1, "R55_00"},
    {0x55, 5, 151, 1, -1, "R55_05"},

    // ---------- RAW 0x56: the ???? value as raw bytes ----------
    {0x56, 0, 151, 1, -1, "R56_00"},
    {0x56, 1, 151, 1, -1, "R56_01"},

    // ---------- PHASE 2 (later): probe unknown registers, ONE at a time ----------
    // Unsupported -> log shows "Error 0x15 0xEA". Shorter reply -> "ERR: Time out ... got N/18 bytes" + hex dump in espaltherma/log.
    // {0x51, 0, 151, 1, -1, "R51_00"},
    // {0x52, 0, 151, 1, -1, "R52_00"},
    // {0x57, 0, 151, 1, -1, "R57_00"},
};

// Override protocol
#define PROTOCOL 'S'
