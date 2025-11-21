
from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *


class GPGPUSimRequestor(ClockedObject):
    type = "GPGPUSimRequestor"
    cxx_header = "mem/gpgpusim/gpgpusim_requestor.hh"
    cxx_class = "gem5::GPGPUSimRequestor"

    core_id = Param.Int(-1, "GPU core identifier")
    system = Param.System(Parent.any, "system object")
    req_port = RequestPort("Request port for memory accesses")
