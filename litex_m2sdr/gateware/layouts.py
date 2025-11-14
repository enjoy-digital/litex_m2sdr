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
def metadata_layout(meta_data_width=64):
    return [ ("header", meta_data_width), ("timestamp", meta_data_width)]