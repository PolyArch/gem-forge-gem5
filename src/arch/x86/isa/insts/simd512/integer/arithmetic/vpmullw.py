microcode = """
def macroop VPMULLW_XMM_XMM {
    vmulilow dest=xmm0, src1=xmm0v, src2=xmm0m, size=2, VL=16
};

def macroop VPMULLW_XMM_M {
    ldfp128 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=16
    vmulilow dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=16
};

def macroop VPMULLW_XMM_P {
    rdip t7
    ldfp128 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=16
    vmulilow dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=16
};

def macroop VPMULLW_YMM_YMM {
    vmulilow dest=xmm0, src1=xmm0v, src2=xmm0m, size=2, VL=32
};

def macroop VPMULLW_YMM_M {
    ldfp256 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=32
    vmulilow dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=32
};

def macroop VPMULLW_YMM_P {
    rdip t7
    ldfp256 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=32
    vmulilow dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=32
};

def macroop VPMULLW_ZMM_ZMM {
    vmulilow dest=xmm0, src1=xmm0v, src2=xmm0m, size=2, VL=64
};

def macroop VPMULLW_ZMM_M {
    ldfp512 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=64
    vmulilow dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=64
};

def macroop VPMULLW_ZMM_P {
    rdip t7
    ldfp512 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=64
    vmulilow dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=64
};
"""
