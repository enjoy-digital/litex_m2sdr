#
# This file is responsible for scheduling transmissions based on timestamps.
#
# author: Ismail Essaidi
# 


from migen import *

from litex.gen import *

from litepcie.common import *

from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import *

class Scheduler(LiteXModule):
    def __init__(self):
        self.sink   = stream.Buffer(dma_layout(64)) # not dma_layout(64) #FIXME
        self.source = stream.Buffer(dma_layout(64))