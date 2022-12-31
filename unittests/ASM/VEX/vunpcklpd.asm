%ifdef CONFIG
{
  "HostFeatures": ["AVX"],
  "RegData": {
    "XMM2": ["0x4142434445464748", "0x6162636465666768", "0x0000000000000000", "0x0000000000000000"],
    "XMM3": ["0x4142434445464748", "0x6162636465666768", "0x0000000000000000", "0x0000000000000000"],
    "XMM4": ["0x4142434445464748", "0x6162636465666768", "0xFFFFFFFFFFFFFFFF", "0xBBBBBBBBBBBBBBBB"],
    "XMM5": ["0x4142434445464748", "0x6162636465666768", "0xFFFFFFFFFFFFFFFF", "0xBBBBBBBBBBBBBBBB"]
  },
  "MemoryRegions": {
    "0x100000000": "4096"
  }
}
%endif

lea rdx, [rel .data]

vmovapd ymm0, [rdx]
vmovapd ymm1, [rdx + 32]

vunpcklpd xmm2, xmm0, [rdx + 32]
vunpcklpd xmm3, xmm0, xmm1

vunpcklpd ymm4, ymm0, [rdx + 32]
vunpcklpd ymm5, ymm0, ymm1

hlt

align 32
.data:
dq 0x4142434445464748
dq 0x5152535455565758
dq 0xFFFFFFFFFFFFFFFF
dq 0xEEEEEEEEEEEEEEEE

dq 0x6162636465666768
dq 0x7172737475767778
dq 0xBBBBBBBBBBBBBBBB
dq 0xCCCCCCCCCCCCCCCC