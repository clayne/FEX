%ifdef CONFIG
{
  "RegData": {
    "XMM0":  ["0x8000000000000000", "0x4000"],
    "XMM1":  ["0x8000000000000000", "0xC000"]
  }
}
%endif

mov rdx, 0xe0000000

mov rax, 0x3ff0000000000000 ; 1.0
mov [rdx + 8 * 0], rax
mov eax, 2
mov [rdx + 8 * 1], eax

fld qword [rdx + 8 * 0]
fimul dword [rdx + 8 * 1]
fstp tword [rel data2]
movups xmm0, [rel data2]

; Test negative
mov rax, 0x3ff0000000000000 ; 1.0
mov [rdx + 8 * 0], rax
mov eax, -2
mov [rdx + 8 * 1], eax

fld qword [rdx + 8 * 0]
fimul dword [rdx + 8 * 1]
fstp tword [rel data2]
movups xmm1, [rel data2]

hlt

data2:
dq 0
dq 0
