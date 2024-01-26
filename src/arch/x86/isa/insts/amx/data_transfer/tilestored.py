
microcode = """

def macroop TILESTORED_ZMM_M {
    tilestore tmm0, seg, sib_tile_row0, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=0, tileCol=0
    tilestore tmm8, seg, sib_tile_row1, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=1, tileCol=0
    tilestore tmm16, seg, sib_tile_row2, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=2, tileCol=0
    tilestore tmm24, seg, sib_tile_row3, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=3, tileCol=0
    tilestore tmm32, seg, sib_tile_row4, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=4, tileCol=0
    tilestore tmm40, seg, sib_tile_row5, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=5, tileCol=0
    tilestore tmm48, seg, sib_tile_row6, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=6, tileCol=0
    tilestore tmm56, seg, sib_tile_row7, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=7, tileCol=0
    tilestore tmm64, seg, sib_tile_row8, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=8, tileCol=0
    tilestore tmm72, seg, sib_tile_row9, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=9, tileCol=0
    tilestore tmm80, seg, sib_tile_row10, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=10, tileCol=0
    tilestore tmm88, seg, sib_tile_row11, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=11, tileCol=0
    tilestore tmm96, seg, sib_tile_row12, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=12, tileCol=0
    tilestore tmm104, seg, sib_tile_row13, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=13, tileCol=0
    tilestore tmm112, seg, sib_tile_row14, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=14, tileCol=0
    tilestore tmm120, seg, sib_tile_row15, "DISPLACEMENT + 0", dataSize=64, tile="env.reg", tileRow=15, tileCol=0
};

def macroop TILESTORED_ZMM_P {
    panic "TILELOADD with P operand in R/M"
};

"""
