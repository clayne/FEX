%ifdef CONFIG
{
  "RegData": {
    "MM0": "0x20A121A222A323A4",
    "MM1": "0x0041004300450047",
    "MM2": "0x0",
    "MM3": "0x0",
    "MM4": "0x0"
  }
}
%endif

mov rdx, 0xe0000000

mov rax, 0x4142434445464748
mov [rdx + 8 * 0], rax
mov rax, 0x5152535455565758
mov [rdx + 8 * 1], rax

mov rax, 0x1
mov [rdx + 8 * 2], rax
mov rax, 0x0
mov [rdx + 8 * 3], rax

mov rax, 0x8
mov [rdx + 8 * 4], rax
mov rax, 0x0
mov [rdx + 8 * 5], rax

; Will Zero
mov rax, 0x10
mov [rdx + 8 * 6], rax
mov rax, 0x0
mov [rdx + 8 * 7], rax

; Will Zero
mov rax, 0x20
mov [rdx + 8 * 8], rax
mov rax, 0x0
mov [rdx + 8 * 9], rax

; Will Zero
mov rax, 0x40
mov [rdx + 8 * 10], rax
mov rax, 0x0
mov [rdx + 8 * 11], rax

movq mm0, [rdx + 8 * 0]
movq mm1, [rdx + 8 * 0]
movq mm2, [rdx + 8 * 0]
movq mm3, [rdx + 8 * 0]
movq mm4, [rdx + 8 * 0]

psrlw mm0, [rdx + 8 * 2]
psrlw mm1, [rdx + 8 * 4]
psrlw mm2, [rdx + 8 * 6]
psrlw mm3, [rdx + 8 * 8]
psrlw mm4, [rdx + 8 * 10]

hlt
