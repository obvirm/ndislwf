param([string]$Dump)
$b = [IO.File]::ReadAllBytes($Dump)
$sig = [Text.Encoding]::ASCII.GetString($b, 0, 8)
"signature: $sig  version=0x{0:X}" -f [BitConverter]::ToUInt32($b, 8)
$n = [BitConverter]::ToUInt32($b, 12)
"streams: $n"
$base = [BitConverter]::ToUInt32($b, 16)
for ($i = 0; $i -lt $n; $i++) {
    $off = $base + 12 * $i
    $type = [BitConverter]::ToUInt32($b, $off)
    $size = [BitConverter]::ToUInt32($b, $off + 4)
    $rva = [BitConverter]::ToUInt32($b, $off + 8)
    "stream type=$type size=$size rva=0x$($rva.ToString('X'))"
    if ($type -eq 6) {
        $er = $rva + 12   # ThreadId(4) Alignment(4) FixedRecordSize(4)
        $code = [BitConverter]::ToUInt32($b, $er)
        $addr = [BitConverter]::ToUInt64($b, $er + 16)
        $num = [BitConverter]::ToUInt32($b, $er + 24)
        "BUGCHECK code=0x{0:X8} addr=0x{1:X16} num={2}" -f $code, $addr, $num
        for ($p = 0; $p -lt [Math]::Min($num, 4); $p++) {
            $v = [BitConverter]::ToUInt64($b, $er + 32 + 8 * $p)
            ("  param$p = 0x{0:X16}" -f $v)
        }
    }
    if ($type -eq 7) { "CommentStream $size bytes at rva 0x$($rva.ToString('X'))" }
}
