
microcode = """

def macroop LDTILECFG_M {
    ldfp512 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=64
    vsettilecfg src=ufp1
    vtilezero dest=tmm0_0, destVL=1024
    vtilezero dest=tmm1_0, destVL=1024
    vtilezero dest=tmm2_0, destVL=1024
    vtilezero dest=tmm3_0, destVL=1024
    vtilezero dest=tmm4_0, destVL=1024
    vtilezero dest=tmm5_0, destVL=1024
    vtilezero dest=tmm6_0, destVL=1024
    vtilezero dest=tmm7_0, destVL=1024
};

def macroop LDTILECFG_P {
    panic "TILELOADCFG with P operand in R/M"
};

"""
