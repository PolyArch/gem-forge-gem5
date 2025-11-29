
microcode = """

def macroop TILERELEASE {
    vtilerelease
    vtilezero dest=tmm0_0, destVL=1024
    vtilezero dest=tmm1_0, destVL=1024
    vtilezero dest=tmm2_0, destVL=1024
    vtilezero dest=tmm3_0, destVL=1024
    vtilezero dest=tmm4_0, destVL=1024
    vtilezero dest=tmm5_0, destVL=1024
    vtilezero dest=tmm6_0, destVL=1024
    vtilezero dest=tmm7_0, destVL=1024
};

"""
