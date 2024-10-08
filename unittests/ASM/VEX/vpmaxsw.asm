%ifdef CONFIG
{
  "HostFeatures": ["AVX"],
  "RegData": {
    "XMM0": ["0x4142434445464748", "0x7172737475767778", "0x4142434445464748", "0x7172737475767778"],
    "XMM1": ["0x8000636465666768", "0x5152535455565758", "0x6162636465666768", "0x5152535455565758"],
    "XMM2": ["0x4142636465666768", "0x7172737475767778", "0x0000000000000000", "0x0000000000000000"],
    "XMM3": ["0x4142636465666768", "0x7172737475767778", "0x6162636465666768", "0x7172737475767778"],
    "XMM4": ["0x4142636465666768", "0x7172737475767778", "0x0000000000000000", "0x0000000000000000"],
    "XMM5": ["0x4142636465666768", "0x7172737475767778", "0x6162636465666768", "0x7172737475767778"],
    "XMM6": ["0x4142636465666768", "0x7172737475767778", "0x6162636465666768", "0x7172737475767778"],
    "XMM7": ["0x4142636465666768", "0x7172737475767778", "0x6162636465666768", "0x7172737475767778"]
  }
}
%endif

lea rdx, [rel .data]

vmovapd ymm0, [rdx]
vmovapd ymm1, [rdx + 32]

vpmaxsw xmm2, xmm0, xmm1
vpmaxsw ymm3, ymm0, ymm1

vpmaxsw xmm4, xmm0, [rdx + 32]
vpmaxsw ymm5, ymm0, [rdx + 32]

; Some funky combinations for testing fast paths
; Related to SVE sources aliasing the destination
vmovapd ymm6, ymm0
vpmaxsw ymm6, ymm6, ymm5

vmovapd ymm7, ymm0
vpmaxsw ymm7, ymm5, ymm7

hlt

align 32
.data:
dq 0x4142434445464748
dq 0x7172737475767778
dq 0x4142434445464748
dq 0x7172737475767778

dq 0x8000636465666768
dq 0x5152535455565758
dq 0x6162636465666768
dq 0x5152535455565758
