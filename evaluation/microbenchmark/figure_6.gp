set term epscairo enhanced dashed size 8in, 4.5in font "Arial, " background rgb 'white'
set output "figure_6.eps"

f(x) = 0.11 + ((x+0.5) * 0.87 / 5)

set multiplot
set xrange [-0.5:4.5]
set yrange [0:20]

set grid y
set style data histogram

set style fill solid 1.00 border -1
set boxwidth 0.15

unset xtics
set ytics (1, 4, 8, 12, 16) font ", 20" scale 0.0

set lmargin at screen 0.11
set rmargin at screen 0.98
set tmargin at screen 1. - 0.13
set bmargin at screen 1. - 0.45

unset xlabel
set ylabel "Latancy" font "Arial, 24" offset -1, 0

set style line 1 lw 3 lt 6 lc rgb "black"
set pointintervalbox 0

set boxwidth 0.12

plot "seq_results_0/zicio_0.dat" using ($0-0.36):(1) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "seq_results_0/zicio_0.dat" using ($0-0.36):($4) with boxes fs solid lt rgb "red" notitle, \
	 \
	 "seq_results_0/pread_O_0.dat" using ($0-0.24):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "seq_results_0/pread_O_0.dat" using ($0-0.24):($4) with boxes fs solid lt rgb "#000000" notitle, \
	 \
	 "seq_results_0/pread_X_0.dat" using ($0-0.12):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "seq_results_0/pread_X_0.dat" using ($0-0.12):($4) with boxes fs solid lt rgb "#000000" notitle, \
	 \
	 "seq_results_0/uring_O_O_0.dat" using ($0):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
 	 "seq_results_0/uring_O_O_0.dat" using ($0):($4+$5) with boxes fillstyle pattern 2 transparent lt rgb "black" notitle, \
	 \
	 "seq_results_0/uring_X_O_0.dat" using ($0+0.12):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "seq_results_0/uring_X_O_0.dat" using ($0+0.12):($4+$5) with boxes fs solid lt rgb "#aaaaaa" notitle, \
 	 "seq_results_0/uring_X_O_0.dat" using ($0+0.12):($4+$5) with boxes fillstyle pattern 7 transparent lt rgb "black" notitle, \
	 "seq_results_0/uring_X_O_0.dat" using ($0+0.12):($5) with boxes fs solid lt rgb "#666666" notitle, \
	 \
	 "seq_results_0/uring_O_X_0.dat" using ($0+0.24):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
 	 "seq_results_0/uring_O_X_0.dat" using ($0+0.24):($4+$5) with boxes fillstyle pattern 2 transparent lt rgb "black" notitle, \
	 \
	 "seq_results_0/uring_X_X_0.dat" using ($0+0.36):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
	 "seq_results_0/uring_X_X_0.dat" using ($0+0.36):($4+$5) with boxes fs solid lt rgb "#aaaaaa" notitle, \
 	 "seq_results_0/uring_X_X_0.dat" using ($0+0.36):($4+$5) with boxes fillstyle pattern 7 transparent lt rgb "black" notitle, \
	 "seq_results_0/uring_X_X_0.dat" using ($0+0.36):($5) with boxes fs solid lt rgb "#666666" notitle, \
	 \
	 "<(sed -n '1p' seq_results_0/zicio_0.dat)" using ($0-0.36 - 0.02):(5.5):(sprintf("%.1f s",$2)) with labels font ", 20" rotate by 90 notitle, \
	 \
	 "<(sed -n '1p' seq_results_0/zicio_0.dat)" using (0-0.36):(16.3):(0):(1-16.3 + 9) with vectors filled head lw 3 lc "black" notitle, \
     "<(sed -n '3p' seq_results_0/pread_O_0.dat)" using (2-0.24):(11.8):(0):($2-11.8) with vectors filled head lw 3 lc "black" notitle, \
	 "<(sed -n '3p' seq_results_0/pread_X_0.dat)" using (2-0.12):(9.2):(0):($2-9.2) with vectors filled head lw 3 lc "black" notitle, \
	 "<(sed -n '1p' seq_results_0/uring_O_O_0.dat)" using (0):(17):(0):($2-17) with vectors filled head lw 3 lc "black" notitle, \
     "<(sed -n '1p' seq_results_0/uring_X_O_0.dat)" using (0+0.12):(14.4):(0):($2-14.4) with vectors filled head lw 3 lc "black" notitle, \
	 "<(sed -n '1p' seq_results_0/uring_O_X_0.dat)" using (0+0.24):(11.8):(0):($2-11.8) with vectors filled head lw 3 lc "black" notitle, \
	 "<(sed -n '1p' seq_results_0/uring_X_X_0.dat)" using (0+0.36):(9.2):(0):($2-9.2) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (0-0.36 - 0.1):(16.3 + 1.5):(sprintf('{/Times-New-Roman-Bold=20 {/=16 ZIC}IO}')) with labels left font "Arial Bold, 20" notitle, \
	 "" using (2-0.24 - 0.04):(11.8 + 1.5):(sprintf('+ Direct I/O')) with labels left font "Arial Bold, 20" notitle, \
	 "" using (2-0.24 - 0.04 - 0.12):(11.8 + 1.5):(sprintf('P')) with labels left font "Arial Bold, 20" textcolor "red" notitle, \
	 "" using (2-0.12 - 0.04):(9.2 + 1.5):(sprintf('pread (   )')) with labels left font "Arial Bold, 20" notitle, \
	 "" using (2-0.12 - 0.04 + 0.45):(9.2 + 1.5):(sprintf('P')) with labels left font "Arial Bold, 20" textcolor "red" notitle, \
	 "" using (0 - 0.04 + 0.09):(17 + 1.5):(sprintf(' + Helper + Direct I/O')) with labels left font "Arial Bold, 20" notitle, \
	 "" using (0 - 0.04):(17 + 1.5):(sprintf('U')) with labels left font "Arial Bold, 20" textcolor "red" notitle, \
	 "" using (0+0.12 - 0.04 + 0.09):(14.4 + 1.5):(sprintf(' + Direct I/O')) with labels left font "Arial Bold, 20" notitle, \
	 "" using (0+0.12 - 0.04):(14.4 + 1.5):(sprintf('U')) with labels left font "Arial Bold, 20" textcolor "red" notitle, \
	 "" using (0+0.24 - 0.04 + 0.09):(11.8 + 1.5):(sprintf(' + Helper')) with labels left font "Arial Bold, 20" notitle, \
	 "" using (0+0.24 - 0.04):(11.8 + 1.5):(sprintf('U')) with labels left font "Arial Bold, 20" textcolor "red" notitle, \
	 "" using (0+0.36 - 0.04):(9.2 + 1.5):(sprintf('io\\_uring (   )')) with labels left font "Arial Bold, 20" notitle, \
	 "" using (0+0.36 - 0.04 + 0.61):(9.2 + 1.5):(sprintf('U')) with labels left font "Arial Bold, 20" textcolor "red" notitle, \

unset ylabel

set tmargin at screen 1. - 0.16
set bmargin at screen 1. - 0.38

set yrange [0:4]
set ytics (1, 2, 3) font ", 16" scale 0.0

do for [pos=3:4] {
	set lmargin at screen f(pos-0.42)
	set rmargin at screen f(pos+0.42)

	set xrange [pos-0.47:pos+0.47]
	plot "<(sed -n '".(pos+1)."p' seq_results_0/zicio_0.dat)" using (pos-0.36):(1) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' seq_results_0/zicio_0.dat)" using (pos-0.36):($4) with boxes fs solid lt rgb "red" notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' seq_results_0/pread_O_0.dat)" using (pos-0.24):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' seq_results_0/pread_O_0.dat)" using (pos-0.24):($4) with boxes fs solid lt rgb "#000000" notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' seq_results_0/pread_X_0.dat)" using (pos-0.12):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' seq_results_0/pread_X_0.dat)" using (pos-0.12):($4) with boxes fs solid lt rgb "#000000" notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' seq_results_0/uring_O_O_0.dat)" using (pos):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
	 	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_O_O_0.dat)" using (pos):($4+$5) with boxes fillstyle pattern 2 transparent lt rgb "black" notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_O_0.dat)" using (pos+0.12):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_O_0.dat)" using (pos+0.12):($4+$5) with boxes fs solid lt rgb "#aaaaaa" notitle, \
	 	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_O_0.dat)" using (pos+0.12):($4+$5) with boxes fillstyle pattern 7 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_O_0.dat)" using (pos+0.12):($5) with boxes fs solid lt rgb "#666666" notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' seq_results_0/uring_O_X_0.dat)" using (pos+0.24):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
	 	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_O_X_0.dat)" using (pos+0.24):($4+$5) with boxes fillstyle pattern 2 transparent lt rgb "black" notitle, \
		 \
		 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_X_0.dat)" using (pos+0.36):($2) with boxes fs solid lt rgb "#dddddd" notitle, \
		 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_X_0.dat)" using (pos+0.36):($4+$5) with boxes fs solid lt rgb "#aaaaaa" notitle, \
	 	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_X_0.dat)" using (pos+0.36):($4+$5) with boxes fillstyle pattern 7 transparent lt rgb "black" notitle, \
		 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_X_0.dat)" using (pos+0.36):($5) with boxes fs solid lt rgb "#666666" notitle
}

set lmargin at screen 0.11
set rmargin at screen 0.98
set tmargin at screen 1. - 0.45
set bmargin at screen 1. - 0.68

set xrange [-0.5:4.5]
set yrange [0:20]

unset grid
set ytics ("    "16) font ", 20" scale 0.0
set ylabel "Normalized performance metrics" font "Arial, 24" offset -5.0, 2.0
plot "" using (-1):0 with boxes fs solid lt rgb "#dddddd" notitle
set grid y

set ytics (1, 4, 8, 12) font ", 20" scale 0.0
set ylabel "Cycles" font "Arial, 24" offset -1, 0.0

plot "seq_results_0/zicio_0.dat" using ($0-0.36):(1) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/pread_O_0.dat" using ($0-0.24):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/pread_X_0.dat" using ($0-0.12):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/uring_O_O_0.dat" using ($0):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/uring_X_O_0.dat" using ($0+0.12):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/uring_O_X_0.dat" using ($0+0.24):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/uring_X_X_0.dat" using ($0+0.36):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 \
	 "<(sed -n '1p' seq_results_0/zicio_0.dat)" using ($0-0.36 - 0.02):(7.5):(sprintf("%.1f B",$6)) with labels font ", 20" rotate by 90 notitle, \
	 \
     "<(sed -n '1,3p' seq_results_0/pread_O_0.dat)" using ($0-0.24):(15.25):(0):($6-15.25) with vectors filled head lw 3 lc "black" notitle, \
     "<(sed -n '1,3p' seq_results_0/uring_X_O_0.dat)" using ($0+0.12):(15.25):(0):($6-15.25) with vectors filled head lw 3 lc "black" notitle, \
	 "" using ($0-0.4):(15.25+2.5):(sprintf(' Sleep wait')) with labels left font "Arial Bold, 20" notitle

unset ylabel

set tmargin at screen 1. - 0.47
set bmargin at screen 1. - 0.62

set yrange [0:6.5]
set ytics (1, 3, 5) font ", 16" scale 0.0

do for [pos=3:4] {
	set lmargin at screen f(pos-0.42)
	set rmargin at screen f(pos+0.42)

	set xrange [pos-0.47:pos+0.47]
plot "<(sed -n '".(pos+1)."p' seq_results_0/zicio_0.dat)" using (pos-0.36):(1) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/pread_O_0.dat)" using (pos-0.24):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/pread_X_0.dat)" using (pos-0.12):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_O_O_0.dat)" using (pos):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_O_0.dat)" using (pos+0.12):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_O_X_0.dat)" using (pos+0.24):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_X_0.dat)" using (pos+0.36):($6) with boxes fs solid lt rgb "brown4" notitle, \
	 \
     "<(sed -n '".(pos+1)."p' seq_results_0/pread_O_0.dat)" using (pos-0.24):(4.5):(0):($6-4.5) with vectors filled head lw 3 lc "black" notitle, \
     "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_O_0.dat)" using (pos+0.12):(4.5):(0):($6-4.5) with vectors filled head lw 3 lc "black" notitle, \
	 "" using (pos-0.4):(4.5+1):(sprintf(' Sleep wait')) with labels left font "Arial Bold, 20" notitle
}

set lmargin at screen 0.11
set rmargin at screen 0.98
set tmargin at screen 1. - 0.68
set bmargin at screen 1. - 0.88

set xrange [-0.5:4.5]
set yrange [0:12]

set xtics ("4 KiB"0, "8 KiB"1, "16 KiB"2, "64 KiB"3, "256 KiB"4) font ", 20" scale 0.0
set ytics (1, 4, 8, "   "12) font ", 20" scale 0.0
set xlabel 'Request Size' font "Arial, 22" offset 0, -0.8
set ylabel "Insn." font "Arial, 24" offset -1, 0.5

plot "seq_results_0/zicio_0.dat" using ($0-0.36):(1) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/pread_O_0.dat" using ($0-0.24):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/pread_X_0.dat" using ($0-0.12):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/uring_O_O_0.dat" using ($0):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/uring_X_O_0.dat" using ($0+0.12):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/uring_O_X_0.dat" using ($0+0.24):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "seq_results_0/uring_X_X_0.dat" using ($0+0.36):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 \
	 "<(sed -n '1p' seq_results_0/zicio_0.dat)" using ($0-0.36 - 0.02):(6):(sprintf("%.1f B",$7)) with labels font ", 20" rotate by 90 notitle, \

unset xtics
unset xlabel
unset ylabel

set tmargin at screen 1. - 0.70
set bmargin at screen 1. - 0.83

set yrange [0:2.5]
set ytics (1, 2) font ", 16" scale 0.0

do for [pos=3:4] {
	set lmargin at screen f(pos-0.42)
	set rmargin at screen f(pos+0.42)

	set xrange [pos-0.47:pos+0.47]
plot "<(sed -n '".(pos+1)."p' seq_results_0/zicio_0.dat)" using (pos-0.36):(1) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/pread_O_0.dat)" using (pos-0.24):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/pread_X_0.dat)" using (pos-0.12):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_O_O_0.dat)" using (pos):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_O_0.dat)" using (pos+0.12):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_O_X_0.dat)" using (pos+0.24):($7) with boxes fs solid lt rgb "brown4" notitle, \
	 "<(sed -n '".(pos+1)."p' seq_results_0/uring_X_X_0.dat)" using (pos+0.36):($7) with boxes fs solid lt rgb "brown4" notitle
}

set lmargin at screen 0.11
set rmargin at screen 0.98

set lmargin at screen 0
set rmargin at screen 1
set tmargin at screen 1
set bmargin at screen 0

set xrange [0:8]
set yrange [0.01:4.5]
unset border

unset logscale y

unset xtics
unset xlabel
unset grid

set key reverse Left outside horizontal font ", 22" samplen 3 width 5
set key at 3.58, 4.45
plot "" using (0):(0) with boxes fs solid lt rgb "#dddddd" title "Data Ingestion"
plot "" using (0):(0) with boxes fillstyle pattern 8 transparent lt rgb "black" title "              "
set key at 6.05, 4.45
plot "" using (0):(0) with boxes fs solid lt rgb "#aaaaaa" title "I/O Completion"
plot "" using (0):(0) with boxes fillstyle pattern 7 transparent lt rgb "black" title "              "
set key at 8.1, 4.45
plot "" using (0):(0) with boxes fs solid lt rgb "#666666" title "I/O Submission"
plot "" using (0):(0) with boxes fillstyle pattern 8 transparent lt rgb "black" title "              "
set key at 3.7, 4.2
plot "" using (0):(0) with boxes fs solid lt rgb "#dddddd" title "Wait for Helper"
plot "" using (0):(0) with boxes fillstyle pattern 2 transparent lt rgb "black" title "               "
set key at 5.7, 4.2
plot "" using (0):(0) with boxes fs solid lt rgb "#000000" title "System Call"
plot "" using (0):(0) with boxes fillstyle pattern 8 transparent lt rgb "black" title "           "
set key at 8.67, 4.2
plot "" using (0):(0) with boxes fs solid lt rgb "red" title "Wait Time in {/Times-New-Roman-Bold=22 U{/=18 ZIC}IO}"
plot "" using (0):(0) with boxes fs pattern 8 transparent lt rgb "black" title "                   "
unset key