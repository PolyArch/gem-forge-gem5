
categories = [
    "arithmetic",
    "data_conversion",
    "data_transfer",
]

microcode = '''
# AVX512 instructions
'''
for category in categories:
    exec "import %s as cat" % category
    microcode += cat.microcode