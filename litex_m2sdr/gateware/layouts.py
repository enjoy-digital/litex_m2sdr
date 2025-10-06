#
# This file is responsible defining layouts used for data streams
#
# author: Ismail Essaidi
# 


# modified dma layout to include timestamp
def dma_layout_with_ts(data_width):
    return [
        ("data", data_width),
        ("timestamp", 64),  
    ]

# metadata layout for scheduler tx
def metadata_layout(timestamp_width=64, ptr_width=8):
    return [("timestamp", timestamp_width), ("ptr", ptr_width)]