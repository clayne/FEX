%ifdef CONFIG
{
  "RegData": {
    "RAX": "0x1",
    "RBX": "0x0"
  }
}
%endif

mov r15, 0xe0000000

mov r10, 0x1
mov r11, 0x0
mov r12, 0x1

cmp r10d, r12d

mov rax, 0
mov rbx, 0
setns al
sets  bl

hlt
