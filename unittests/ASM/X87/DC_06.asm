%ifdef CONFIG
{
  "RegData": {
    "MM7":  ["0x8000000000000000", "0x3FFE"]
  }
}
%endif

lea rdx, [rel data]
fld tword [rdx + 8 * 0]

lea rdx, [rel data2]
fdiv qword [rdx + 8 * 0]

hlt

align 8
data:
  dt 1.0
  dq 0
data2:
  dq 2.0
