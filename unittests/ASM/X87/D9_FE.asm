%ifdef CONFIG
{
  "RegData": {
    "MM7":  ["0xD76AA47848677021", "0x3FFE"]
  }
}
%endif

lea rdx, [rel data]
fld tword [rdx + 8 * 0]

fsin

hlt

align 8
data:
  dt 1.0
  dq 0
