
microcode = """

def macroop TDPBSSD_ZMM_R {
    tdpbssd dest=tmm0, src1=tmm0v, src2=tmm0m, size=8, VL=64
};

"""
