
microcode = '''
def macroop VMULPS_XMM_XMM {
    mmulf dest=xmm0, src1=xmm0v, op2=xmm0m, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=xmm1m, size=4, ext=0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMULPS_XMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    mmulf dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext=0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMULPS_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    mmulf dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext=0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMULPS_YMM_YMM {
    mmulf dest=xmm0, src1=xmm0v, op2=xmm0m, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=xmm1m, size=4, ext=0
    mmulf dest=xmm2, src1=xmm2v, op2=xmm2m, size=4, ext=0
    mmulf dest=xmm3, src1=xmm3v, op2=xmm3m, size=4, ext=0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMULPS_YMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    mmulf dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext=0
    mmulf dest=xmm2, src1=xmm2v, op2=ufp3, size=4, ext=0
    mmulf dest=xmm3, src1=xmm3v, op2=ufp4, size=4, ext=0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMULPS_YMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    mmulf dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext=0
    mmulf dest=xmm2, src1=xmm2v, op2=ufp3, size=4, ext=0
    mmulf dest=xmm3, src1=xmm3v, op2=ufp4, size=4, ext=0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMULPS_ZMM_ZMM {
    mmulf dest=xmm0, src1=xmm0v, op2=xmm0m, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=xmm1m, size=4, ext=0
    mmulf dest=xmm2, src1=xmm2v, op2=xmm2m, size=4, ext=0
    mmulf dest=xmm3, src1=xmm3v, op2=xmm3m, size=4, ext=0
    mmulf dest=xmm4, src1=xmm4v, op2=xmm4m, size=4, ext=0
    mmulf dest=xmm5, src1=xmm5v, op2=xmm5m, size=4, ext=0
    mmulf dest=xmm6, src1=xmm6v, op2=xmm6m, size=4, ext=0
    mmulf dest=xmm7, src1=xmm7v, op2=xmm7m, size=4, ext=0
};

def macroop VMULPS_ZMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, sib, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, sib, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, sib, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, sib, "DISPLACEMENT + 56", dataSize=8
    mmulf dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext=0
    mmulf dest=xmm2, src1=xmm2v, op2=ufp3, size=4, ext=0
    mmulf dest=xmm3, src1=xmm3v, op2=ufp4, size=4, ext=0
    mmulf dest=xmm4, src1=xmm4v, op2=ufp5, size=4, ext=0
    mmulf dest=xmm5, src1=xmm5v, op2=ufp6, size=4, ext=0
    mmulf dest=xmm6, src1=xmm6v, op2=ufp7, size=4, ext=0
    mmulf dest=xmm7, src1=xmm7v, op2=ufp8, size=4, ext=0
};

def macroop VMULPS_ZMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, riprel, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, riprel, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, riprel, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, riprel, "DISPLACEMENT + 56", dataSize=8
    mmulf dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext=0
    mmulf dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext=0
    mmulf dest=xmm2, src1=xmm2v, op2=ufp3, size=4, ext=0
    mmulf dest=xmm3, src1=xmm3v, op2=ufp4, size=4, ext=0
    mmulf dest=xmm4, src1=xmm4v, op2=ufp5, size=4, ext=0
    mmulf dest=xmm5, src1=xmm5v, op2=ufp6, size=4, ext=0
    mmulf dest=xmm6, src1=xmm6v, op2=ufp7, size=4, ext=0
    mmulf dest=xmm7, src1=xmm7v, op2=ufp8, size=4, ext=0
};

'''
