
categories = [
    "tdpbssd",
    "tdpbusd",
]

microcode = """
# AMX Tile compute instructions
"""
for category in categories:
    exec("from . import {s} as cat".format(s=category))
    microcode += cat.microcode
