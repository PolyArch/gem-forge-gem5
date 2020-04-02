
microcode = '''
def macroop VSUBPD_XMM_XMM {
    vsubf dest=xmm0, src1=xmm0v, src2=xmm0m, size=8, VL=16
};

def macroop VSUBPD_XMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    vsubf dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=16
};

def macroop VSUBPD_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    vsubf dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=16
};

def macroop VSUBPD_YMM_YMM {
    vsubf dest=xmm0, src1=xmm0v, src2=xmm0m, size=8, VL=32
};

def macroop VSUBPD_YMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    vsubf dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=32
};

def macroop VSUBPD_YMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    vsubf dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=32
};

def macroop VSUBPD_ZMM_ZMM {
    vsubf dest=xmm0, src1=xmm0v, src2=xmm0m, size=8, VL=64
};

def macroop VSUBPD_ZMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, sib, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, sib, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, sib, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, sib, "DISPLACEMENT + 56", dataSize=8
    vsubf dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=64
};

def macroop VSUBPD_ZMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, riprel, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, riprel, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, riprel, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, riprel, "DISPLACEMENT + 56", dataSize=8
    vsubf dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=64
};

'''