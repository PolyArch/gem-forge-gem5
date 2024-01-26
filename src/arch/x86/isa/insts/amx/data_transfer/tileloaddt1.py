
microcode = """

def macroop TILELOADDT1_ZMM_M {
    tileload tmm0, seg, sib_tile_row0, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=0, tileCol=0, temporalHint="T1"
    tileload tmm8, seg, sib_tile_row1, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=1, tileCol=0, temporalHint="T1"
    tileload tmm16, seg, sib_tile_row2, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=2, tileCol=0, temporalHint="T1"
    tileload tmm24, seg, sib_tile_row3, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=3, tileCol=0, temporalHint="T1"
    tileload tmm32, seg, sib_tile_row4, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=4, tileCol=0, temporalHint="T1"
    tileload tmm40, seg, sib_tile_row5, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=5, tileCol=0, temporalHint="T1"
    tileload tmm48, seg, sib_tile_row6, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=6, tileCol=0, temporalHint="T1"
    tileload tmm56, seg, sib_tile_row7, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=7, tileCol=0, temporalHint="T1"
    tileload tmm64, seg, sib_tile_row8, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=8, tileCol=0, temporalHint="T1"
    tileload tmm72, seg, sib_tile_row9, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=9, tileCol=0, temporalHint="T1"
    tileload tmm80, seg, sib_tile_row10, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=10, tileCol=0, temporalHint="T1"
    tileload tmm88, seg, sib_tile_row11, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=11, tileCol=0, temporalHint="T1"
    tileload tmm96, seg, sib_tile_row12, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=12, tileCol=0, temporalHint="T1"
    tileload tmm104, seg, sib_tile_row13, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=13, tileCol=0, temporalHint="T1"
    tileload tmm112, seg, sib_tile_row14, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=14, tileCol=0, temporalHint="T1"
    tileload tmm120, seg, sib_tile_row15, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=15, tileCol=0, temporalHint="T1"
};

def macroop TILELOADDT1_ZMM_ZMM {
    panic "TILELOADD with Reg operand in R/M"
};

def macroop TILELOADDT1_ZMM_P {
    panic "TILELOADD with P operand in R/M"
};

"""
